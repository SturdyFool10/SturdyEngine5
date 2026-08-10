#include "WindowManager.hpp"

#include <Async/src/SpscRingBuffer.hpp>

#include <chrono>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing {

    // Per-window lock-free event channel: one Async::SpscRingBuffer<WindowEvent> (the actual
    // producer/consumer handoff -- no lock, fixed capacity allocated once at construction) plus the
    // small amount of ancillary per-window state that isn't part of the event stream itself.
    //
    // Ownership/threading: constructed by seed_channel() (called from spawn_window(), which runs on
    // whichever thread owns dispatch() -- poll_loop()'s background thread, or the CallerThread
    // caller). `pending` is producer-thread-confined -- only drain_window_into() (running on the
    // window's owning thread) ever touches it, so it needs no synchronization of its own.
    // `close_requested` and framebuffer_size_packed_ are written by that same producer thread once
    // per poll pass and read by pump()'s (consumer) thread, so those two go through atomics.
    class WindowEventChannel {
      public:
        explicit WindowEventChannel(usize ring_capacity) noexcept : ring(ring_capacity) {}

        Async::SpscRingBuffer<WindowEvent> ring;

        // In-flight MouseMoved coalescing run not yet published to `ring`. Producer-thread-confined;
        // never touched by the consumer (pump()).
        optional<WindowEvent> pending;

        // Set true two ways, matching this manager's pre-existing combined behavior: (1)
        // drain_window_into() latches this the moment it observes a CloseRequested event, *before*
        // attempting to push it into the ring -- so a CloseRequested that gets dropped for being past
        // the ring's capacity still isn't lost (a stranded "user asked to close and we silently
        // forgot" would be far worse than an over-eager latch); (2) window->close_requested() is
        // itself a monotonic per-window latch (see Window.hpp's own doc comment), so re-querying it
        // every producer pass and OR-ing the result in covers any backend where the flag can flip
        // true through a path that doesn't also queue a CloseRequested event. Never reset to false —
        // matches the original accumulator's same-shaped sticky field.
        std::atomic<bool> close_requested{false};

        // Set true by drain_window_into() the moment it observes a Resized/FramebufferResized event,
        // *before* attempting to push it into the ring -- same "latch before a possible drop" reasons
        // as close_requested above. Unlike close_requested, this is genuinely per-pump()-tick: only
        // materialize_channel() ever clears it (via take_resized()), matching the original
        // accumulator's per-tick (not sticky) resized field.
        std::atomic<bool> resized{false};

        [[nodiscard]] bool take_resized() noexcept { return resized.exchange(false, std::memory_order_acq_rel); }

        void set_framebuffer_size(WindowExtent size) noexcept {
            const u64 packed = (static_cast<u64>(size.x) << 32U) | static_cast<u64>(size.y);
            framebuffer_size_packed_.store(packed, std::memory_order_release);
        }

        [[nodiscard]] optional<WindowExtent> framebuffer_size() const noexcept {
            const u64 packed = framebuffer_size_packed_.load(std::memory_order_acquire);
            if (packed == unknown_framebuffer_size) {
                return std::nullopt;
            }
            return WindowExtent{static_cast<u32>(packed >> 32U), static_cast<u32>(packed & 0xFFFFFFFFU)};
        }

      private:
        static constexpr u64 unknown_framebuffer_size = ~u64{0};
        std::atomic<u64> framebuffer_size_packed_{unknown_framebuffer_size};
    };

    namespace {

        using ChannelDirectory = vector<WindowEventChannelEntry>;

        [[nodiscard]] WindowEventChannel *find_channel(ChannelDirectory &channels, WindowId id) noexcept {
            for (auto &[channel_id, channel] : channels) {
                if (channel_id == id) {
                    return channel.get();
                }
            }
            return nullptr;
        }

        // Merges `incoming` into `previous` when both are MouseMoved, per
        // WindowManagerPolicy::coalesce_mouse_motion's own doc comment: position is last-write-wins,
        // motion is summed so nothing is actually lost, and the button mask is OR-ed so a button
        // held during any part of the merged run is still reported as held for it.
        [[nodiscard]] bool coalesce_mouse_motion(WindowEvent &previous, const WindowEvent &incoming) noexcept {
            if (previous.kind != WindowEventKind::MouseMoved || incoming.kind != WindowEventKind::MouseMoved) {
                return false;
            }
            previous.mouse_move.x = incoming.mouse_move.x;
            previous.mouse_move.y = incoming.mouse_move.y;
            previous.mouse_move.delta_x += incoming.mouse_move.delta_x;
            previous.mouse_move.delta_y += incoming.mouse_move.delta_y;
            previous.mouse_move.buttons |= incoming.mouse_move.buttons;
            // Same last-write-wins rule as position above: the merged event should read as "the mouse
            // was last observed here at this time," not carry the first sample's now-stale timestamp.
            previous.timestamp_ns = incoming.timestamp_ns;
            return true;
        }

        // Drains `channel`'s ring into a fresh ManagedWindowEvents and reads its separately-tracked
        // close_requested/resized/framebuffer_size state (see WindowEventChannel's own doc comments
        // for why those are latched independently of the ring rather than derived by scanning what
        // was actually drained -- a dropped-for-being-past-capacity Resized/CloseRequested event must
        // still not be lost). Shared by both of pump()'s branches so there is exactly one place that
        // materializes the public ManagedWindowEvents shape from a channel.
        [[nodiscard]] ManagedWindowEvents materialize_channel(WindowId id, WindowEventChannel &channel) noexcept {
            ManagedWindowEvents collected{.window_id = id, .events = {}, .framebuffer_size = {}};
            // A modest fixed reserve rather than trying to track a per-channel high-water mark: real
            // per-pump-call counts are small (coalescing collapses an entire high-rate motion burst
            // to one or a few events -- see WindowManagerPolicy::coalesce_mouse_motion's own comment),
            // so this just avoids the first couple of reallocations on a typical tick.
            collected.events.reserve(16);
            channel.ring.drain_into(collected.events);
            collected.close_requested = channel.close_requested.load(std::memory_order_acquire);
            collected.resized = channel.take_resized();
            collected.framebuffer_size = channel.framebuffer_size();
            return collected;
        }

    } // namespace

    WindowManager::WindowManager(WindowManagerPolicy policy) noexcept
        : policy_(policy) {
        ZoneScopedN("WindowManager::WindowManager");
        if (policy_.event_pump_mode == WindowEventPumpMode::DedicatedEventThread &&
            policy_.platform_allows_threads && compile_time_window_thread_allowed) {
            event_thread_ = std::make_unique<Async::DedicatedThread>("WindowEventThread");
            running_.store(true, std::memory_order_release);
            // Submitted once, runs for event_thread_'s whole lifetime (see poll_loop()'s own doc
            // comment) — the returned handle is deliberately discarded, nothing waits on this task
            // finishing except ~WindowManager() via running_/wake_poll_loop() below.
            (void)event_thread_->run([this]() { poll_loop(); });
        }
    }

    WindowManager::~WindowManager() {
        ZoneScopedN("WindowManager::~WindowManager");
        if (event_thread_) {
            // Signals poll_loop() to finish its current iteration, clear windows_ (on the thread that
            // owns them — same drain-before-destroy discipline Application uses for render_thread_),
            // and return. event_thread_'s own destructor (implicit member teardown, right after this
            // body runs) then joins the underlying OS thread, which only completes once poll_loop()
            // has actually returned — wake_poll_loop() means that's immediate, not a wait up to a
            // full idle interval.
            running_.store(false, std::memory_order_release);
            wake_poll_loop();
        } else {
            windows_.clear();
        }
    }

    [[nodiscard]] const WindowManagerPolicy &WindowManager::policy() const noexcept { return policy_; }

    [[nodiscard]] bool WindowManager::has_dedicated_event_thread() const noexcept { return event_thread_ != nullptr; }

    void WindowManager::destroy_window(WindowId id) noexcept {
        ZoneScopedN("WindowManager::destroy_window");
        dispatch([this, id]() {
            std::erase_if(windows_, [id](const unique_ptr<Window> &w) { return w->id() == id; });
            if (primary_window_id_ == id) {
                primary_window_id_ = windows_.empty() ? optional<WindowId>{} : optional<WindowId>{windows_.front()->id()};
            }
            // Keep the channel directory in lockstep with windows_ (see channels_'s own doc comment
            // in the header) — no more entry for a window poll_loop() will never poll again. Erasing
            // here only drops the *directory's* reference; a concurrent pump() that already snapshot
            // this channel keeps it alive via its own shared_ptr copy until that call finishes.
            auto guard = channels_.lock();
            std::erase_if(*guard, [id](const WindowEventChannelEntry &entry) { return entry.first == id; });
        });
    }

    void WindowManager::seed_channel(WindowId id) noexcept {
        ZoneScopedN("WindowManager::seed_channel");
        auto channel = std::make_shared<WindowEventChannel>(policy_.max_accumulated_events_per_window);
        auto guard = channels_.lock();
        guard->emplace_back(id, std::move(channel));
    }

    [[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept {
        ZoneScopedN("WindowManager::primary_window_id");
        return dispatch([this]() { return primary_window_id_; });
    }

    [[nodiscard]] usize WindowManager::window_count() noexcept {
        ZoneScopedN("WindowManager::window_count");
        return dispatch([this]() { return windows_.size(); });
    }

    void WindowManager::flush_pending(WindowEventChannel &channel) noexcept {
        if (!channel.pending.has_value()) {
            return;
        }
        push_or_warn(channel, *channel.pending);
        channel.pending.reset();
    }

    void WindowManager::push_or_warn(WindowEventChannel &channel, const WindowEvent &event) noexcept {
        if (channel.ring.try_push(event)) {
            return;
        }
        // Only reachable when the consumer has stopped calling pump() entirely while input keeps
        // arriving. Dropping is the correct failure mode here — the alternative is growing until the
        // process dies — and the state that actually matters across a stall (close/resize, tracked
        // separately from the ring; see WindowEventChannel) is already preserved regardless.
        if (!accumulator_overflow_warned_) {
            accumulator_overflow_warned_ = true;
            Foundation::log_warn(
                "WindowManager: a window's {}-slot ring is full; dropping further events until "
                "pump() drains it. The consumer is not pumping.",
                channel.ring.capacity());
        }
    }

    [[nodiscard]] usize WindowManager::drain_window_into(Window &window, WindowEventChannel &channel) noexcept {
        ZoneScopedN("WindowManager::drain_window_into");
        if (channel.ring.size() == 0) {
            // pump() has caught up since the last overflow, so re-arm the one-time warning below —
            // done here, on the event thread that solely owns this flag, rather than from pump()'s
            // own thread, which would be a plain data race on it.
            accumulator_overflow_warned_ = false;
        }
        usize drained = 0;
        while (auto event = window.poll_event()) {
            ++drained;

            // Latch the two state bits that must survive regardless of what happens to the event
            // itself below — done *before* any coalescing/ring-capacity decision, so a CloseRequested
            // or Resized/FramebufferResized that ends up dropped for being past the ring's capacity
            // still isn't lost (see WindowEventChannel's own doc comments on close_requested/resized).
            if (event->kind == WindowEventKind::CloseRequested) {
                channel.close_requested.store(true, std::memory_order_release);
            } else if (event->kind == WindowEventKind::Resized || event->kind == WindowEventKind::FramebufferResized) {
                channel.resized.store(true, std::memory_order_release);
            }

            if (policy_.coalesce_mouse_motion && channel.pending.has_value() &&
                coalesce_mouse_motion(*channel.pending, *event)) {
                continue;
            }

            // A non-coalescible event (or coalescing is off): whatever was pending publishes first,
            // in order, before this one.
            flush_pending(channel);

            if (policy_.coalesce_mouse_motion && event->kind == WindowEventKind::MouseMoved) {
                // Start a new coalescible run rather than publishing immediately -- this is what
                // lets a burst of adjacent MouseMoved events collapse to a single ring slot instead
                // of one each (see coalesce_mouse_motion()'s own doc comment for why that matters at
                // 8kHz). Only *this* call's run coalesces together: flush_pending() below (both the
                // one at the top of this loop's next non-coalescible event, and the final one after
                // the loop) always publishes before this function returns, rather than holding a run
                // open across poll_loop() iterations the way the previous mutex/vector accumulator
                // did. That's an intentional, minor relaxation -- never loses motion data (still
                // fully summed/last-write-wins), just occasionally yields one extra coalesced event
                // per window instead of one across a longer window.
                channel.pending = *event;
                continue;
            }

            push_or_warn(channel, *event);
        }
        flush_pending(channel); // don't leave a coalesced run invisible to the consumer between calls
        return drained;
    }

    void WindowManager::poll_loop() noexcept {
        // Sub-millisecond idle interval: far more headroom than needed to keep an OS input queue from
        // ever backing up under a high-polling-rate device (an 8kHz mouse) — the OS/driver already
        // coalesces individual HID reports between our poll passes, so this loop doesn't need to
        // literally match 8kHz, just stay comfortably ahead of it — while still yielding the core
        // between passes rather than busy-spinning the way a task-stealing pool's spin phase would.
        constexpr auto idle_interval = std::chrono::microseconds(500);

        // How many events one pass has to drain before it's treated as a *backlog* worth re-polling
        // immediately instead of sleeping out an interval first. This threshold is the whole reason
        // the loop stays cheap under a high-polling-rate mouse: "did any event arrive?" is true on
        // essentially every pass at 8kHz (one HID report every 125us, so ~4 land per idle interval),
        // so re-polling on that condition alone turned every mouse movement into an unbounded busy
        // spin — pinning a core and, far worse, re-taking the provider's global window mutex
        // (sdl_window_mutex()/glfw_window_mutex(), which every Window query also needs) tens of
        // thousands of times a second. That starved the main thread's pump()/with_window() calls for
        // the entire duration of a mouse movement, which is the stutter this threshold exists to
        // prevent. A pass that drains less than this has already emptied the OS queue, so sleeping
        // loses nothing.
        constexpr usize backlog_threshold = 64;

        while (running_.load(std::memory_order_acquire)) {
            // Per-iteration, not per-call: poll_loop() itself runs for the whole background thread's
            // lifetime, so a single zone around the function body would show up as one enormous
            // "frame" in Tracy instead of the actual per-pass cadence this loop runs at.
            ZoneScopedN("WindowManager::poll_loop iteration");

            // 1. Drain and execute every one-shot op (spawn_window/destroy_window/with_window/
            //    with_windows, via dispatch()) queued since the last iteration.
            std::deque<unique_ptr<Async::Detail::TaskBase>> ready;
            {
                auto guard = pending_ops_.lock();
                ready.swap(*guard);
            }
            const bool ran_ops = !ready.empty();
            for (unique_ptr<Async::Detail::TaskBase> &task : ready) {
                task->execute();
            }

            // 2. Poll every still-live window and push newly-arrived events into its lock-free
            //    channel. The channels_ lock is only held long enough to copy out the directory
            //    (shared_ptr refcount bumps) -- all the real drain/coalesce/push work below runs
            //    unlocked, unlike the old accumulated_-locked-across-the-whole-loop shape this
            //    replaced.
            usize drained_events = 0;
            if (!windows_.empty()) {
                for (unique_ptr<Window> &window : windows_) {
                    if (auto pumped = window->pump_events(); !pumped) {
                        Foundation::log_warn(
                            "WindowManager: background pump_events() failed for window {}: {}",
                            static_cast<usize>(window->id()),
                            pumped.error().message);
                    }
                }

                ChannelDirectory snapshot;
                {
                    auto guard = channels_.lock();
                    snapshot = *guard;
                }

                for (unique_ptr<Window> &window : windows_) {
                    WindowEventChannel *channel = find_channel(snapshot, window->id());
                    if (channel == nullptr) {
                        continue; // spawn_window() always seeds one — defensive, should not happen
                    }
                    drained_events += drain_window_into(*window, *channel);
                    // OR in, never overwrite: drain_window_into() may have just latched this true
                    // from the CloseRequested event's own content (see its doc comment), and
                    // window->close_requested() returning false right now (e.g. a test double that
                    // never sets its own flag) must not stomp that.
                    if (window->close_requested()) {
                        channel->close_requested.store(true, std::memory_order_release);
                    }
                    if (auto size = window->framebuffer_size()) {
                        channel->set_framebuffer_size(*size);
                    }
                }
            }

            // Re-poll immediately only for work that a sleep would actually delay: a queued
            // dispatch() op whose caller is blocked waiting on it right now, or a genuine event
            // backlog (see backlog_threshold above for why "any event at all" is the wrong test).
            if (ran_ops || drained_events >= backlog_threshold) {
                continue;
            }

            std::unique_lock<std::mutex> idle_lock(wake_mutex_);
            wake_cv_.wait_for(idle_lock, idle_interval, [this]() noexcept {
                // Must also check pending_ops_, not just running_: dispatch()'s wake_poll_loop() call
                // (a bare notify_all(), no wake_mutex_ held) only actually cuts this wait short if the
                // predicate can observe the new op. A shutdown-only predicate leaves every dispatch()/
                // with_window() caller blocked for up to the full idle_interval even though the whole
                // point of waking here was to run their queued op immediately — with_window() itself
                // has no idea how the manager is pumped internally, so that latency silently lands on
                // every per-frame caller (window state sync, cursor-icon updates, ...).
                if (!running_.load(std::memory_order_acquire)) {
                    return true;
                }
                auto guard = pending_ops_.lock();
                return !guard->empty();
            });
        }

        // Run whatever dispatch() queued but this loop never got to. Every one of those has a caller
        // blocked in TaskHandle::wait() on a TaskState that only execute() ever marks done — leaving
        // them queued hangs that caller forever, and since ~WindowManager() joins event_thread_ right
        // after this returns, a caller racing shutdown would deadlock the whole teardown. windows_ is
        // still alive here, so these run exactly as they would have on any normal iteration.
        {
            std::deque<unique_ptr<Async::Detail::TaskBase>> ready;
            {
                auto guard = pending_ops_.lock();
                ready.swap(*guard);
            }
            for (unique_ptr<Async::Detail::TaskBase> &task : ready) {
                task->execute();
            }
        }

        // Tear down every window on the thread that owns them, now that the loop is exiting for good
        // — mirrors Application's drain-before-destroy discipline for render_thread_.
        windows_.clear();
    }

    void WindowManager::wake_poll_loop() noexcept {
        ZoneScopedN("WindowManager::wake_poll_loop");
        // Taking wake_mutex_ before notifying is what makes the wake reliable rather than
        // best-effort. poll_loop() evaluates its wait predicate (which reads pending_ops_) while
        // holding this mutex; a bare notify_all() can land in the window between that predicate
        // returning false and the wait actually registering, and is then simply lost — the caller
        // that just queued an op sits blocked for a full idle_interval instead of microseconds.
        // That latency lands on every per-frame with_window() caller (window state sync, cursor
        // updates), so it is worth an uncontended lock here.
        {
            std::lock_guard<std::mutex> wake_lock(wake_mutex_);
        }
        wake_cv_.notify_all();
    }

    [[nodiscard]] expected<void, WindowError> WindowManager::pump(vector<ManagedWindowEvents> &out_events) noexcept {
        ZoneScopedN("WindowManager::pump");
        if (event_thread_) {
            // Non-blocking: snapshot the channel directory (shared_ptr copies only -- brief lock),
            // then drain each channel's lock-free ring outside it. No reseeding needed here the way
            // the old mutex/vector accumulator required: each channel's ring is a fixed, permanent
            // allocation that poll_loop() keeps pushing into regardless of when/whether pump() drains
            // it, so there is no per-tick "give the producer somewhere to write next" step at all.
            ChannelDirectory snapshot;
            {
                auto guard = channels_.lock();
                snapshot = *guard;
            }
            out_events.clear();
            out_events.reserve(snapshot.size());
            for (auto &[id, channel] : snapshot) {
                out_events.push_back(materialize_channel(id, *channel));
            }
            return {};
        }

        // CallerThread mode (macOS/Web, or wherever threads are disallowed): synchronous poll-and-
        // drain, still sharing drain_window_into()/materialize_channel() with the threaded path so
        // mouse-motion coalescing and the ring's drop-when-full cap are properties of the manager,
        // not of whichever pump mode a platform happens to be on.
        return dispatch([this, &out_events]() -> expected<void, WindowError> {
            out_events.clear();
            if (windows_.empty()) {
                return {};
            }

            for (unique_ptr<Window> &window : windows_) {
                if (auto pumped = window->pump_events(); !pumped) {
                    return unexpected(pumped.error());
                }
            }

            ChannelDirectory snapshot;
            {
                auto guard = channels_.lock();
                snapshot = *guard;
            }

            out_events.reserve(windows_.size());
            for (unique_ptr<Window> &window : windows_) {
                WindowEventChannel *channel = find_channel(snapshot, window->id());
                if (channel == nullptr) {
                    continue; // spawn_window() always seeds one — defensive, should not happen
                }

                // Window event/state queries obey the same affinity contract as pump_events(). Keep
                // queue draining here on the dispatch owner rather than handing live Window* to
                // Scheduler workers, even though current providers protect their queues with locks
                // internally.
                (void)drain_window_into(*window, *channel);
                // OR in, never overwrite -- see the DedicatedEventThread branch's identical check in
                // poll_loop() for why.
                if (window->close_requested()) {
                    channel->close_requested.store(true, std::memory_order_release);
                }
                if (auto size = window->framebuffer_size()) {
                    channel->set_framebuffer_size(*size);
                }

                out_events.push_back(materialize_channel(window->id(), *channel));
            }

            return {};
        });
    }

} // namespace SFT::Platform::Windowing
