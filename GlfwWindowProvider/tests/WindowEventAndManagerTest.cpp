#include <Platform/Window/GLFW/Window.hpp>
#include <Platform/Window/SDL3/Window.hpp>
#include <Platform/Window/WindowManager.hpp>

#include <chrono>
#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace SFT::Platform::Windowing;

    struct ThreadTrace {
        std::thread::id constructed;
        std::thread::id pumped;
        std::thread::id polled;
        std::thread::id sampled;
        std::thread::id destroyed;
    };

    ThreadTrace thread_trace;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    class AffinityWindow final : public Window {
      public:
        /// Destroys the `AffinityWindow` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~AffinityWindow() noexcept override { thread_trace.destroyed = std::this_thread::get_id(); }

        /// Constructs the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static expected<unique_ptr<AffinityWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig &           ) noexcept {
            auto *window = new (std::nothrow) AffinityWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            return unique_ptr<AffinityWindow>{window};
        }

        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override { return WindowBackendKind::SDL3; }
        /// Returns the runtime or backend type represented by `AffinityWindow`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowingSystem type() const noexcept override { return WindowingSystem::SDL3; }
        /// Returns the native backend handle associated with this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override { return nullptr; }
        /// Returns the native window handle associated with this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override { return NativeWindowHandle{}; }

        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> pump_events() noexcept override {
            thread_trace.pumped = std::this_thread::get_id();
            if (!event_queued_) {
                WindowEvent event{WindowEventKind::MouseButtonPressed};
                event.mouse_button = WindowMouseButtonEvent{
                    .button = 91,
                    .clicks = 2,
                    .x = 12.0F,
                    .y = 34.0F,
                    .button_code = MouseButton::Left,
                };
                events_.push_back(event);
                event_queued_ = true;
            }
            return {};
        }

        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override {
            thread_trace.polled = std::this_thread::get_id();
            if (events_.empty()) {
                return std::nullopt;
            }
            WindowEvent event = events_.front();
            events_.pop_front();
            return event;
        }

        /// Closes requested using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool close_requested() const noexcept override { return close_requested_; }
        /// Requests close using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void request_close() noexcept override { close_requested_ = true; }
        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool resized() const noexcept override { return false; }
        /// Returns the current or globally available consume resize value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override { return std::nullopt; }

        /// Returns the current or globally available show value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> show() noexcept override { return {}; }
        /// Returns the current or globally available hide value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> hide() noexcept override { return {}; }
        /// Returns the current or globally available focus value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> focus() noexcept override { return {}; }
        /// Returns the current or globally available raise value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> raise() noexcept override { return {}; }
        /// Returns the current or globally available maximize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> maximize() noexcept override { return {}; }
        /// Returns the current or globally available minimize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> minimize() noexcept override { return {}; }
        /// Returns the current or globally available restore value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> restore() noexcept override { return {}; }
        /// Sets the title for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_title(const char *          ) noexcept override { return {}; }
        /// Returns the current or globally available position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override { return WindowPosition{}; }
        /// Sets the position for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_position(WindowPosition             ) noexcept override { return {}; }
        /// Returns the current or globally available global cursor position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override { return WindowPosition{}; }
        /// Returns the size for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override { return WindowExtent{640, 480}; }
        /// Sets the size for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_size(WindowExtent           ) noexcept override { return {}; }
        /// Returns the framebuffer size for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override {
            thread_trace.sampled = std::this_thread::get_id();
            return WindowExtent{640, 480};
        }
        /// Sets the minimum size for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_minimum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the maximum size for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_maximum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the resizable for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_resizable(bool            ) noexcept override { return {}; }
        /// Sets the decorated for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_decorated(bool            ) noexcept override { return {}; }
        /// Sets the fullscreen for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_fullscreen(WindowMode         ) noexcept override { return {}; }
        /// Sets the opacity for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_opacity(f32            ) noexcept override { return {}; }
        /// Returns the current or globally available opacity value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override { return 1.0F; }
        /// Sets the cursor icon for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_icon(CursorIcon         ) noexcept override { return {}; }
        /// Sets the cursor visible for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_visible(bool            ) noexcept override { return {}; }
        /// Sets the cursor grabbed for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_grabbed(bool            ) noexcept override { return {}; }
        /// Sets the relative mouse mode for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_relative_mouse_mode(bool            ) noexcept override { return {}; }
        /// Sets the mouse locked for this `AffinityWindow`.
        ///
        /// @param locked `locked` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override {
            mouse_locked_ = locked;
            return {};
        }
        /// Returns the current or globally available mouse locked value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_locked() const noexcept override { return mouse_locked_; }
        /// Enables window effect using the supplied arguments and current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect           ) noexcept override {
            return WindowEffectResult::success();
        }
        /// Sets the effect for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_effect(WindowEffect           ) noexcept override { return {}; }
        /// Sets the blur enabled for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_blur_enabled(bool            ) noexcept override { return {}; }
        /// Sets the transparent for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_transparent(bool            ) noexcept override { return {}; }
        /// Returns the current or globally available required vulkan instance extensions value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<vector<const char *>, WindowError> required_vulkan_instance_extensions() const noexcept override {
            return vector<const char *>{};
        }
        /// Creates a vulkan surface from the supplied parameters.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> create_vulkan_surface(
            void *             ,
            const void *                         ,
            void *                ) const noexcept override {
            return {};
        }
        /// Returns the current or globally available clipboard text value.
        ///
        /// @return Returns the current clipboard text value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string clipboard_text() const noexcept override { return {}; }
        /// Sets the clipboard text for this `AffinityWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_clipboard_text(std::string_view         ) noexcept override { return {}; }

      private:
        /// Constructs a `AffinityWindow` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @note This function does not throw exceptions.
        explicit AffinityWindow(ConstructorKey key) noexcept : Window(key) {
            thread_trace.constructed = std::this_thread::get_id();
        }

        std::deque<WindowEvent> events_;
        bool event_queued_ = false;
        bool close_requested_ = false;
        bool mouse_locked_ = false;
    };

    /// Reports whether normalization is provider independent.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool normalization_is_provider_independent() {
        using SFT::Platform::Windowing::GLFW::Detail::normalize_mouse_button;

        bool passed = true;
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_LEFT) == MouseButton::Left,
                        "GLFW left button was not normalized");
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_MIDDLE) == MouseButton::Middle,
                        "GLFW middle button was not normalized");
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_RIGHT) == MouseButton::Right,
                        "GLFW right button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_LEFT) == MouseButton::Left,
                        "SDL left button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_MIDDLE) == MouseButton::Middle,
                        "SDL middle button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_RIGHT) == MouseButton::Right,
                        "SDL right button was not normalized");

        const WindowMouseButtonEvent legacy_aggregate{7, 2, 3.0F, 4.0F};
        passed &= check(legacy_aggregate.button == 7 && legacy_aggregate.clicks == 2,
                        "legacy native mouse-button aggregate fields changed");
        passed &= check(legacy_aggregate.button_code == MouseButton::Unknown,
                        "legacy aggregate did not default the appended normalized field");
        return passed;
    }

    /// Performs the manager keeps window access on one owner operation using the supplied arguments.
    ///
    /// @param policy `policy` value used by the operation.
    /// @param expect_dedicated `expect_dedicated` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool manager_keeps_window_access_on_one_owner(WindowManagerPolicy policy, bool expect_dedicated) {
        thread_trace = {};
        const std::thread::id caller = std::this_thread::get_id();
        bool passed = true;

        WindowManager manager{policy};
        passed &= check(manager.has_dedicated_event_thread() == expect_dedicated,
                        "WindowManager selected the wrong event-thread policy");

        const auto spawned = manager.spawn_window<AffinityWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "fake window could not be spawned");
        if (!spawned) {
            return false;
        }


        vector<ManagedWindowEvents> packets;
        bool observed_event = false;
        for (int attempt = 0; attempt < 200 && !observed_event; ++attempt) {
            const auto pumped = manager.pump(packets);
            passed &= check(pumped.has_value(), "WindowManager::pump failed");
            if (!packets.empty() && !packets[0].events.empty()) {
                observed_event = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        passed &= check(!packets.empty(), "pump never returned a window packet");
        if (!packets.empty()) {
            passed &= check(packets[0].events.size() == 1, "translated event queue was not drained");
            if (packets[0].events.size() == 1) {
                const WindowMouseButtonEvent &mouse = packets[0].events[0].mouse_button;
                passed &= check(mouse.button == 91, "provider-native button value was not preserved");
                passed &= check(mouse.button_code == MouseButton::Left, "normalized button value was not preserved");
            }
            passed &= check(packets[0].framebuffer_size == optional<WindowExtent>{WindowExtent{640, 480}},
                            "framebuffer state was not sampled");
        }

        const optional<WindowId> primary = manager.primary_window_id();
        passed &= check(primary == optional<WindowId>{*spawned}, "primary window ID was not returned");
        manager.destroy_window(*spawned);

        const std::thread::id owner = thread_trace.constructed;
        passed &= check(owner != std::thread::id{}, "window construction thread was not recorded");
        passed &= check(thread_trace.pumped == owner, "pump_events ran off the window owner thread");
        passed &= check(thread_trace.polled == owner, "poll_event ran off the window owner thread");
        passed &= check(thread_trace.sampled == owner, "framebuffer_size ran off the window owner thread");
        passed &= check(thread_trace.destroyed == owner, "window destruction ran off the window owner thread");
        passed &= check(expect_dedicated ? owner != caller : owner == caller,
                        "window owner thread did not match the selected policy");
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = normalization_is_provider_independent();
    passed &= manager_keeps_window_access_on_one_owner(
        SFT::Platform::Windowing::WindowManagerPolicy{
            .event_pump_mode = SFT::Platform::Windowing::WindowEventPumpMode::CallerThread,
            .platform_allows_threads = true,
        },
        false);
    passed &= manager_keeps_window_access_on_one_owner(
        SFT::Platform::Windowing::WindowManagerPolicy{
            .event_pump_mode = SFT::Platform::Windowing::WindowEventPumpMode::DedicatedEventThread,
            .platform_allows_threads = true,
        },
        SFT::Platform::Windowing::compile_time_window_thread_allowed);
    return passed ? 0 : 1;
}
