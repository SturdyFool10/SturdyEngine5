#pragma once

#include <Foundation/src/Foundation.hpp>
#include <Async/src/Async.hpp>

#pragma region Imports
#include <atomic>
#include <condition_variable>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#pragma endregion

#include <Platform/Window/Window.hpp>
#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowEvent.hpp>
#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowGeometry.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing {

    // Explicit static-composition seam for optional window providers. A product references a
    // provider's factory symbol directly; no global registration or whole-archive linking is needed.
    using WindowFactory = expected<unique_ptr<Window>, WindowError> (*)(const WindowConfig &) noexcept;

    enum class WindowEventPumpMode : u8 {
        // Required on Web and safest for APIs/platforms that require all window calls on the main thread.
        CallerThread,
        // A dedicated Async::DedicatedThread owns every window in this manager: construction,
        // destruction, pumping, and any bespoke access via with_window()/with_windows(). Only takes
        // effect where compile_time_window_thread_allowed is true (see below) — requesting it elsewhere
        // silently falls back to CallerThread.
        DedicatedEventThread,
    };

    // Whether a dedicated window/event thread is ever safe on this platform, mirroring
    // RHI::compile_time_rhi_multithreading_allowed (RHI/Include/RHI/Threading.hpp). Cocoa's NSApplication
    // event loop and the browser's event loop both require the process's literal main thread — there is
    // no "a" main thread to hand this off to on those platforms, unlike the render thread.
#if defined(STURDY_PLATFORM_MACOS) || defined(STURDY_PLATFORM_WEB)
    inline constexpr bool compile_time_window_thread_allowed = false;
#else
    inline constexpr bool compile_time_window_thread_allowed = true;
#endif

    struct WindowManagerPolicy {
        WindowEventPumpMode event_pump_mode = WindowEventPumpMode::CallerThread;
        bool platform_allows_threads = true;

        // Merge a run of consecutive MouseMoved events into a single one as they're folded into the
        // accumulator (see WindowManager.cpp's coalesce_mouse_motion()). A high-polling-rate mouse
        // (8kHz) produces one motion event per HID report — ~133 per 60Hz frame — and every one of
        // them is copied four more times downstream (accumulator -> pump() -> PlatformEventInbox ->
        // both the lossless WindowEvent stream and the typed MouseMoveEvent stream, EcsEvents.hpp)
        // before three separate built-in systems iterate it (InputState, UiPointerState, plus
        // whatever the app registers). No information is lost by merging: absolute x/y is
        // last-write-wins and delta_x/delta_y are *summed*, which is exactly what every consumer in
        // this engine already does with them (InputState::apply(MouseMoveEvent) accumulates the
        // delta and overwrites the position; UiPointerState only ever takes the latest position).
        // Only *adjacent* events merge, so ordering against button/key/wheel events — and therefore
        // the pointer position a click is attributed to — is untouched.
        //
        // Turn this off only for something that genuinely needs every raw sample point rather than
        // the summed motion between frames (a pressure/stroke-capture drawing tool, an input-latency
        // measurement harness); ordinary game and UI input wants it on.
        bool coalesce_mouse_motion = true;

        // Hard bound on how many events one window may accumulate between two pump() calls, so a
        // stalled or blocked main thread (a long frame, a modal move/resize drag, a breakpoint)
        // cannot grow the accumulator without limit while the OS keeps delivering input. Well above
        // any real frame's worth: with coalesce_mouse_motion on, a 60Hz frame accumulates single
        // digits, and even a full second of stalled 8kHz motion coalesces to one event.
        usize max_accumulated_events_per_window = 16384;
    };

    struct ManagedWindowEvents {
        WindowId window_id = invalid_window_id;
        vector<WindowEvent> events;
        bool close_requested = false;
        bool resized = false;
        // Sampled on the window-owning thread during this pump() call, so callers never need a second
        // dispatch (or a raw Window*) just to learn the current framebuffer size for FrameInput.
        optional<WindowExtent> framebuffer_size;
    };

    // Real owner of every window in the process: construction, destruction, pumping, and any bespoke
    // per-window access all funnel through dispatch() below, which is the single choke point enforcing
    // Window's own documented invariant ("drive pump_events() and the event/state queries from the
    // thread that owns the platform message pump", Window.cppm) — callers never hold a raw Window*
    // across a call boundary or touch one from a thread WindowManager doesn't control. Supports any mix
    // of backends (SDL3/GLFW) simultaneously, since Window is already a pure abstraction seam; this
    // class just owns a heterogeneous collection of them.
    class WindowManager {
      public:
        explicit WindowManager(WindowManagerPolicy policy = {}) noexcept;

        ~WindowManager();

        WindowManager(const WindowManager &) = delete;
        WindowManager &operator=(const WindowManager &) = delete;
        WindowManager(WindowManager &&) = delete;
        WindowManager &operator=(WindowManager &&) = delete;

        [[nodiscard]] const WindowManagerPolicy &policy() const noexcept;
        [[nodiscard]] bool has_dedicated_event_thread() const noexcept;

        // Constructs a new window of the given backend and adds it to the managed set — the sole entry
        // point for spawning windows. Safe to call at any time, including after the event thread is up
        // (construction is marshaled the same as everything else).
        template <typename Backend>
        [[nodiscard]] expected<WindowId, WindowError> spawn_window(const WindowConfig &config) {
            ZoneScopedN("WindowManager::spawn_window");
            return dispatch([this, &config]() -> expected<WindowId, WindowError> {
                auto created = Window::create<Backend>(config);
                if (!created) {
                    return unexpected(created.error());
                }
                const WindowId id = (*created)->id();
                if (!primary_window_id_) {
                    primary_window_id_ = id;
                }
                windows_.push_back(std::move(*created));
                {
                    auto guard = accumulated_.lock();
                    guard->push_back(ManagedWindowEvents{.window_id = id, .events = {}, .framebuffer_size = {}});
                }
                return id;
            });
        }

        // Factory overload used by explicitly linked optional providers. The indirect call keeps the
        // base WindowManager free of concrete provider references, so an unselected static provider
        // archive is not extracted by the linker.
        [[nodiscard]] expected<WindowId, WindowError> spawn_window(
            const WindowConfig &config,
            WindowFactory factory) {
            ZoneScopedN("WindowManager::spawn_window(factory)");
            if (factory == nullptr) {
                return unexpected(WindowError{
                    WindowErrorCode::InvalidArgument,
                    "WindowManager::spawn_window requires a non-null WindowFactory.",
                });
            }
            return dispatch([this, &config, factory]() -> expected<WindowId, WindowError> {
                auto created = factory(config);
                if (!created) {
                    return unexpected(created.error());
                }
                const WindowId id = (*created)->id();
                if (!primary_window_id_) {
                    primary_window_id_ = id;
                }
                windows_.push_back(std::move(*created));
                {
                    auto guard = accumulated_.lock();
                    guard->push_back(ManagedWindowEvents{.window_id = id, .events = {}, .framebuffer_size = {}});
                }
                return id;
            });
        }

        void destroy_window(WindowId id) noexcept;

        [[nodiscard]] optional<WindowId> primary_window_id() const noexcept;

        [[nodiscard]] usize window_count() noexcept;

        // Runs fn(Window&) for the window matching id, on the thread that owns it, returning nullopt if
        // no such window exists. This — plus with_windows() below — is the generic escape hatch for any
        // windowing operation not already covered by a dedicated method on this class (title, size,
        // effects, cursor mode, ...): callers should route through here rather than caching a Window*.
        template <typename F>
        auto with_window(WindowId id, F &&fn) -> optional<std::invoke_result_t<F &, Window &>> {
            ZoneScopedN("WindowManager::with_window");
            using R = std::invoke_result_t<F &, Window &>;
            return dispatch([this, id, &fn]() -> optional<R> {
                for (unique_ptr<Window> &w : windows_) {
                    if (w->id() == id) {
                        return optional<R>{fn(*w)};
                    }
                }
                return optional<R>{};
            });
        }

        // Runs fn(vector<unique_ptr<Window>>&) with exclusive access to every managed window, on the
        // thread that owns them.
        template <typename F>
        auto with_windows(F &&fn) -> std::invoke_result_t<F &, vector<unique_ptr<Window>> &> {
            ZoneScopedN("WindowManager::with_windows");
            return dispatch([this, &fn]() { return fn(windows_); });
        }

        // Coordinator pump: hands back whatever's accumulated since the last call. In DedicatedEventThread
        // mode this is non-blocking — the event thread polls continuously and independently in the
        // background (see poll_loop() below), so an OS input queue never backs up waiting on this
        // tick's frame time, which matters for a high-polling-rate device (an 8kHz mouse) — this call
        // just swaps out whatever the background loop has already collected. In CallerThread mode
        // (macOS/Web, or wherever the platform disallows threads) this instead does the original
        // synchronous poll-and-drain inline, exactly as before. If the managed set is empty, the caller
        // gets an empty event list and can conclude execution.
        [[nodiscard]] expected<void, WindowError> pump(vector<ManagedWindowEvents> &out_events) noexcept;

      private:
        // Core dispatch primitive for one-shot windows_-touching operations (spawn/destroy/with_window/
        // with_windows) — every one of them still runs on the thread that owns the windows and the
        // caller still blocks for the result, exactly like before. What changed is *how* it gets there:
        // event_thread_ (when active) is permanently busy running poll_loop() below, not idle waiting
        // for DedicatedThread::run() calls, so a one-shot op is instead queued into pending_ops_ (the
        // same TaskState/ConcreteTask/TaskHandle machinery DedicatedThread::run() itself is built on —
        // Async/src/Task.hpp) for poll_loop() to drain and execute on its own next iteration.
        template <typename F>
        auto dispatch(F &&fn) -> std::invoke_result_t<F &> {
            ZoneScopedN("WindowManager::dispatch");
            using R = std::invoke_result_t<F &>;
            if (!event_thread_) {
                return fn();
            }
            auto state = std::make_shared<Async::Detail::TaskState<R>>();
            auto task = std::make_unique<Async::Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            {
                auto guard = pending_ops_.lock();
                guard->push_back(std::move(task));
            }
            wake_poll_loop();
            {
                ZoneScopedN("WindowManager::dispatch wait for poll_loop");
                return Async::TaskHandle<R>(std::move(state)).wait();
            }
        }

        template <typename F>
        auto dispatch(F &&fn) const -> std::invoke_result_t<F &> {
            // const-qualified callers (primary_window_id(), window_count()) only ever read windows_ —
            // safe to funnel through the same non-const queue-and-wait path via const_cast, matching
            // the const-outer/non-const-body shape the original implementation already used with
            // DedicatedThread::run() (which took the same closure-by-value approach regardless of
            // constness).
            return const_cast<WindowManager *>(this)->dispatch(std::forward<F>(fn));
        }

        // poll_loop() is the single long-running task event_thread_ ever runs (submitted once, from
        // the constructor) — continuous background polling replaces the old "one DedicatedThread::run()
        // per pump() call" shape, which only ever polled when the main thread asked, once per tick.
        void poll_loop() noexcept;
        void wake_poll_loop() noexcept;

        // Folds one window's freshly-polled events into its accumulator entry, applying
        // policy_.coalesce_mouse_motion and policy_.max_accumulated_events_per_window. Returns how
        // many events it actually drained from the window (before coalescing/dropping), which
        // poll_loop() uses to tell "a real backlog is queued up, keep draining" apart from "a
        // trickle arrived, go back to sleep". Runs on whichever thread owns `window` — poll_loop()
        // calls it with accumulated_ held (entry points into it); pump()'s CallerThread path calls
        // it on a local entry it has not published yet.
        [[nodiscard]] usize drain_window_into(Window &window, ManagedWindowEvents &entry) noexcept;

        // Per-window accumulator poll_loop() folds newly-polled events into every iteration; pump()
        // swaps it out (matching Async::MainThread's run_on_main_thread()/pump_main_thread() shape:
        // swap under lock, use the swapped-out copy outside it). Entries are added when
        // dispatch()-driven spawn_window() adds a window and removed when destroy_window() removes
        // one, both from inside poll_loop() itself (see WindowManager.cpp) so windows_ and this stay
        // in lockstep without a second lock ordering to reason about.
        Async::Mutex<vector<ManagedWindowEvents>> accumulated_;

        WindowManagerPolicy policy_{};
        vector<unique_ptr<Window>> windows_;
        optional<WindowId> primary_window_id_;
        unique_ptr<Async::DedicatedThread> event_thread_;

        // Queued one-shot dispatch() closures for poll_loop() to run on its own thread — see dispatch()'s
        // doc comment. Reuses Async::Detail::TaskBase rather than a bespoke closure type so dispatch()
        // can hand the caller a real Async::TaskHandle<R> to wait() on, identical to what
        // DedicatedThread::run() itself already returns.
        Async::Mutex<std::deque<std::unique_ptr<Async::Detail::TaskBase>>> pending_ops_;

        // running_ gates poll_loop()'s own while-condition; wake_mutex_/wake_cv_ let dispatch() and
        // the destructor interrupt its idle sleep immediately instead of waiting out a full idle
        // interval (same shape Async::Scheduler's own worker idle-wait uses).
        std::atomic<bool> running_{false};
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;

        // Latches the one-time "accumulator hit its cap, dropping events" warning. Only ever touched
        // from poll_loop()'s own thread, so it needs no synchronization — and it exists at all
        // because the condition that trips it (a stalled consumer under a live input stream) is
        // exactly the condition under which an unthrottled log_warn would itself become the stall.
        bool accumulator_overflow_warned_ = false;
    };

} // namespace SFT::Platform::Windowing
