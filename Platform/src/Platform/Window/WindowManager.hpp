#pragma once

#include <Foundation/src/Foundation.hpp>
#include <Async/src/Async.hpp>

#pragma region Imports
#include <expected>
#include <memory>
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

using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing {

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
            return dispatch([this, &fn]() { return fn(windows_); });
        }

        // Coordinator pump: first drain backend/OS events on the window-owner path, then spawn one short
        // poller task per window to empty that window's translated event queue into a ManagedWindowEvents
        // packet. If the snapshot is empty, the caller gets an empty event list and can conclude execution.
        [[nodiscard]] expected<void, WindowError> pump(vector<ManagedWindowEvents> &out_events) noexcept;

      private:
        // Core dispatch primitive: runs fn() on event_thread_ and blocks for the result if a dedicated
        // event thread is active, otherwise runs it inline on the caller. Every windows_-touching method
        // above goes through this — it is the only place windows_ is ever accessed.
        template <typename F>
        auto dispatch(F &&fn) -> std::invoke_result_t<F &> {
            if (event_thread_) {
                auto handle = event_thread_->run(std::forward<F>(fn));
                return handle.wait();
            }
            return fn();
        }

        WindowManagerPolicy policy_{};
        vector<unique_ptr<Window>> windows_;
        optional<WindowId> primary_window_id_;
        unique_ptr<Async::DedicatedThread> event_thread_;
    };

} // namespace SFT::Platform::Windowing
