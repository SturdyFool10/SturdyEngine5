#include "WindowManager.hpp"

#include <chrono>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing {

    namespace {

        [[nodiscard]] ManagedWindowEvents *find_accumulator_entry(vector<ManagedWindowEvents> &entries, WindowId id) noexcept {
            for (ManagedWindowEvents &entry : entries) {
                if (entry.window_id == id) {
                    return &entry;
                }
            }
            return nullptr;
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
            // Keep the accumulator in lockstep with windows_ (see its own doc comment in the header)
            // — no more entry for a window poll_loop() will never poll again.
            auto guard = accumulated_.lock();
            std::erase_if(*guard, [id](const ManagedWindowEvents &entry) { return entry.window_id == id; });
        });
    }

    [[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept {
        ZoneScopedN("WindowManager::primary_window_id");
        return dispatch([this]() { return primary_window_id_; });
    }

    [[nodiscard]] usize WindowManager::window_count() noexcept {
        ZoneScopedN("WindowManager::window_count");
        return dispatch([this]() { return windows_.size(); });
    }

    void WindowManager::poll_loop() noexcept {
        // Sub-millisecond idle interval: far more headroom than needed to keep an OS input queue from
        // ever backing up under a high-polling-rate device (an 8kHz mouse) — the OS/driver already
        // coalesces individual HID reports between our poll passes, so this loop doesn't need to
        // literally match 8kHz, just stay comfortably ahead of it — while still yielding the core
        // between passes rather than busy-spinning the way a task-stealing pool's spin phase would.
        constexpr auto idle_interval = std::chrono::microseconds(500);

        while (running_.load(std::memory_order_acquire)) {
            // Per-iteration, not per-call: poll_loop() itself runs for the whole background thread's
            // lifetime, so a single zone around the function body would show up as one enormous
            // "frame" in Tracy instead of the actual per-pass cadence this loop runs at.
            ZoneScopedN("WindowManager::poll_loop iteration");
            bool did_work = false;

            // 1. Drain and execute every one-shot op (spawn_window/destroy_window/with_window/
            //    with_windows, via dispatch()) queued since the last iteration.
            std::deque<unique_ptr<Async::Detail::TaskBase>> ready;
            {
                auto guard = pending_ops_.lock();
                ready.swap(*guard);
            }
            for (unique_ptr<Async::Detail::TaskBase> &task : ready) {
                task->execute();
                did_work = true;
            }

            // 2. Poll every still-live window and fold newly-arrived events into the accumulator.
            if (!windows_.empty()) {
                for (unique_ptr<Window> &window : windows_) {
                    if (auto pumped = window->pump_events(); !pumped) {
                        Foundation::log_warn(
                            "WindowManager: background pump_events() failed for window {}: {}",
                            static_cast<usize>(window->id()),
                            pumped.error().message);
                    }
                }

                auto guard = accumulated_.lock();
                for (unique_ptr<Window> &window : windows_) {
                    ManagedWindowEvents *entry = find_accumulator_entry(*guard, window->id());
                    if (entry == nullptr) {
                        continue; // spawn_window() always seeds one — defensive, should not happen
                    }
                    while (auto event = window->poll_event()) {
                        if (event->kind == WindowEventKind::CloseRequested) {
                            entry->close_requested = true;
                        } else if (event->kind == WindowEventKind::Resized ||
                                   event->kind == WindowEventKind::FramebufferResized) {
                            entry->resized = true;
                        }
                        entry->events.push_back(*event);
                        did_work = true;
                    }
                    if (window->close_requested()) {
                        entry->close_requested = true;
                    }
                    if (auto size = window->framebuffer_size()) {
                        entry->framebuffer_size = *size;
                    }
                }
            }

            if (did_work) {
                continue; // drain any backlog immediately rather than waiting out a full interval
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

        // Tear down every window on the thread that owns them, now that the loop is exiting for good
        // — mirrors Application's drain-before-destroy discipline for render_thread_.
        windows_.clear();
    }

    void WindowManager::wake_poll_loop() noexcept {
        ZoneScopedN("WindowManager::wake_poll_loop");
        wake_cv_.notify_all();
    }

    [[nodiscard]] expected<void, WindowError> WindowManager::pump(vector<ManagedWindowEvents> &out_events) noexcept {
        ZoneScopedN("WindowManager::pump");
        if (event_thread_) {
            // Non-blocking: swap out whatever poll_loop() has accumulated in the background since the
            // last call. Reseed an empty entry per still-swapped-out window in the same locked
            // section — poll_loop()'s own window loop only *updates* entries that already exist (see
            // find_accumulator_entry()), so a gap here would silently drop events until the next
            // reseed rather than just being empty this tick.
            vector<ManagedWindowEvents> swapped;
            {
                auto guard = accumulated_.lock();
                swapped.swap(*guard);
                for (const ManagedWindowEvents &prior : swapped) {
                    guard->push_back(ManagedWindowEvents{.window_id = prior.window_id, .events = {}, .framebuffer_size = {}});
                }
            }
            out_events = std::move(swapped);
            return {};
        }

        // CallerThread mode (macOS/Web, or wherever threads are disallowed): original synchronous
        // poll-and-drain, unchanged.
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

            out_events.reserve(windows_.size());
            for (unique_ptr<Window> &window : windows_) {
                ManagedWindowEvents collected{
                    .window_id = window->id(),
                    .events = {},
                    .framebuffer_size = {},
                };

                // Window event/state queries obey the same affinity contract as pump_events().
                // Keep queue draining here on the dispatch owner rather than handing live Window*
                // to Scheduler workers, even though current providers protect their queues with
                // locks internally.
                while (auto event = window->poll_event()) {
                    if (event->kind == WindowEventKind::CloseRequested) {
                        collected.close_requested = true;
                    } else if (event->kind == WindowEventKind::Resized ||
                               event->kind == WindowEventKind::FramebufferResized) {
                        collected.resized = true;
                    }
                    collected.events.push_back(*event);
                }

                if (window->close_requested()) {
                    collected.close_requested = true;
                }
                if (auto size = window->framebuffer_size()) {
                    collected.framebuffer_size = *size;
                }
                out_events.push_back(std::move(collected));
            }

            return {};
        });
    }

} // namespace SFT::Platform::Windowing
