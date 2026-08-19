#pragma once

#include <Foundation/Foundation.hpp>
#include <Async/Async.hpp>

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

#include <WindowManager/Window.hpp>
#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowEvent.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowGeometry.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::WindowManager {


    using WindowFactory = expected<unique_ptr<Window>, WindowError> (*)(const WindowConfig &) noexcept;

    enum class WindowEventPumpMode : u8 {

        CallerThread,


        DedicatedEventThread,
    };


#if defined(STURDY_PLATFORM_MACOS) || defined(STURDY_PLATFORM_WEB)
    inline constexpr bool compile_time_window_thread_allowed = false;
#else
    inline constexpr bool compile_time_window_thread_allowed = true;
#endif

    struct WindowManagerPolicy {
        WindowEventPumpMode event_pump_mode = WindowEventPumpMode::CallerThread;
        bool platform_allows_threads = true;


        bool coalesce_mouse_motion = true;


        usize max_accumulated_events_per_window = 16384;
    };

    struct ManagedWindowEvents {
        WindowId window_id = invalid_window_id;
        vector<WindowEvent> events;
        bool close_requested = false;
        bool resized = false;


        optional<WindowExtent> framebuffer_size;
    };


    class WindowEventChannel;


    using WindowEventChannelEntry = std::pair<WindowId, std::shared_ptr<WindowEventChannel>>;


    class WindowManager {
      public:
        /// Constructs a `WindowManager` from the supplied initialization values.
        ///
        /// @param policy `policy` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit WindowManager(WindowManagerPolicy policy = {}) noexcept;

        /// Destroys the `WindowManager` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~WindowManager();

        /// Disables this construction form for `WindowManager`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WindowManager(const WindowManager &) = delete;
        /// Assigns a new value to this `WindowManager`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WindowManager &operator=(const WindowManager &) = delete;
        /// Disables this construction form for `WindowManager`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WindowManager(WindowManager &&) = delete;
        /// Assigns a new value to this `WindowManager`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WindowManager &operator=(WindowManager &&) = delete;

        /// Returns the current or globally available policy value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowManagerPolicy &policy() const noexcept;
        /// Reports whether this `WindowManager` has dedicated event thread.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_dedicated_event_thread() const noexcept;


        /// Spawns window.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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
                seed_channel(id);
                return id;
            });
        }


        /// Spawns window.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] expected<WindowId, WindowError> spawn_window(
            const WindowConfig &config,
            WindowFactory factory);

        /// Destroys the window identified by the supplied parameters.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_window(WindowId id) noexcept;

        /// Returns the current or globally available primary window ID value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowId> primary_window_id() const noexcept;

        /// Returns the window count for this `WindowManager`.
        ///
        /// @return Returns the current window count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize window_count() noexcept;


        /// Returns a copy or derived value with window applied.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
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


        /// Performs the post to window operation for `WindowManager` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        void post_to_window(WindowId id, F &&fn) {
            ZoneScopedN("WindowManager::post_to_window");
            post([this, id, fn = std::forward<F>(fn)]() mutable {
                for (unique_ptr<Window> &w : windows_) {
                    if (w->id() == id) {
                        (void)fn(*w);
                        return;
                    }
                }
            });
        }


        /// Returns a copy or derived value with windows applied.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        auto with_windows(F &&fn) -> std::invoke_result_t<F &, vector<unique_ptr<Window>> &> {
            ZoneScopedN("WindowManager::with_windows");
            return dispatch([this, &fn]() { return fn(windows_); });
        }


        /// Pumps the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param out_events Event used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void, WindowError> pump(vector<ManagedWindowEvents> &out_events) noexcept;

      private:


        /// Dispatches the requested work.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


        /// Performs the post operation for `WindowManager` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        void post(F &&fn) {
            ZoneScopedN("WindowManager::post");
            if (!event_thread_) {
                fn();
                return;
            }
            auto state = std::make_shared<Async::Detail::TaskState<void>>();
            auto task = std::make_unique<Async::Detail::ConcreteTask<std::decay_t<F>, void>>(std::forward<F>(fn), state);
            {
                auto guard = pending_ops_.lock();
                guard->push_back(std::move(task));
            }
            wake_poll_loop();
        }

        /// Dispatches the requested work.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        auto dispatch(F &&fn) const -> std::invoke_result_t<F &> {


            return const_cast<WindowManager *>(this)->dispatch(std::forward<F>(fn));
        }


        /// Polls loop for available work or state changes.
        ///
        /// @note This function does not throw exceptions.
        void poll_loop() noexcept;
        /// Performs the wake poll loop operation for `WindowManager` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void wake_poll_loop() noexcept;


        /// Performs the seed channel operation for `WindowManager` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void seed_channel(WindowId id) noexcept;


        /// Drains window into using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param channel `channel` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize drain_window_into(Window &window, WindowEventChannel &channel) noexcept;


        /// Flushes pending.
        ///
        /// @param channel `channel` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void flush_pending(WindowEventChannel &channel) noexcept;


        /// Adds the supplied value to the end or work queue.
        ///
        /// @param channel `channel` value used by the operation.
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void push_or_warn(WindowEventChannel &channel, const WindowEvent &event) noexcept;


        Async::Mutex<vector<WindowEventChannelEntry>> channels_;

        WindowManagerPolicy policy_{};
        vector<unique_ptr<Window>> windows_;
        optional<WindowId> primary_window_id_;


        Async::Mutex<std::deque<std::unique_ptr<Async::Detail::TaskBase>>> pending_ops_;


        std::atomic<bool> running_{false};
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;


        bool accumulator_overflow_warned_ = false;


        unique_ptr<Async::DedicatedThread> event_thread_;
    };

} // namespace SFT::WindowManager
