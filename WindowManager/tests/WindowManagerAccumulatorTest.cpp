

#include <WindowManager/WindowManager.hpp>

#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {

    using namespace SFT::WindowManager;
    using SFT::f32;
    using SFT::usize;

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


    class ScriptedWindow final : public Window {
      public:


        static std::vector<WindowEvent> next_script;

        /// Destroys the `ScriptedWindow` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ScriptedWindow() noexcept override = default;

        /// Constructs the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static expected<unique_ptr<ScriptedWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig &           ) noexcept {
            auto *window = new (std::nothrow) ScriptedWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            window->script_ = next_script;
            return unique_ptr<ScriptedWindow>{window};
        }

        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> pump_events() noexcept override {
            if (!script_replayed_) {
                events_.insert(events_.end(), script_.begin(), script_.end());
                script_replayed_ = true;
            }
            return {};
        }

        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override {
            if (events_.empty()) {
                return std::nullopt;
            }
            WindowEvent event = events_.front();
            events_.pop_front();
            return event;
        }

        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override { return WindowBackendKind::SDL3; }
        /// Returns the runtime or backend type represented by `ScriptedWindow`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowingSystem type() const noexcept override { return WindowingSystem::SDL3; }
        /// Returns the native backend handle associated with this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override { return nullptr; }
        /// Returns the native window handle associated with this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override { return NativeWindowHandle{}; }

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
        /// Sets the title for this `ScriptedWindow`.
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
        /// Sets the position for this `ScriptedWindow`.
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
        /// Returns the size for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override { return WindowExtent{640, 480}; }
        /// Sets the size for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_size(WindowExtent           ) noexcept override { return {}; }
        /// Returns the framebuffer size for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override { return WindowExtent{640, 480}; }
        /// Sets the minimum size for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_minimum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the maximum size for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_maximum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the resizable for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_resizable(bool            ) noexcept override { return {}; }
        /// Sets the decorated for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_decorated(bool            ) noexcept override { return {}; }
        /// Sets the fullscreen for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_fullscreen(WindowMode         ) noexcept override { return {}; }
        /// Returns the current or globally available fullscreen mode value.
        ///
        /// @return Returns the current fullscreen mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowMode fullscreen_mode() const noexcept override { return WindowMode::Windowed; }
        /// Sets the opacity for this `ScriptedWindow`.
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
        /// Sets the cursor icon for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_icon(CursorIcon         ) noexcept override { return {}; }
        /// Sets the cursor visible for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_visible(bool            ) noexcept override { return {}; }
        /// Sets the cursor grabbed for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_grabbed(bool            ) noexcept override { return {}; }
        /// Sets the relative mouse mode for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_relative_mouse_mode(bool            ) noexcept override { return {}; }
        /// Sets the mouse locked for this `ScriptedWindow`.
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
        /// Sets the effect for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_effect(WindowEffect           ) noexcept override { return {}; }
        /// Sets the blur enabled for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_blur_enabled(bool            ) noexcept override { return {}; }
        /// Sets the transparent for this `ScriptedWindow`.
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
        /// Sets the clipboard text for this `ScriptedWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_clipboard_text(std::string_view         ) noexcept override { return {}; }

      private:
        /// Constructs a `ScriptedWindow` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @note This function does not throw exceptions.
        explicit ScriptedWindow(ConstructorKey key) noexcept : Window(key) {}

        std::vector<WindowEvent> script_;
        std::deque<WindowEvent> events_;
        bool script_replayed_ = false;
        bool close_requested_ = false;
        bool mouse_locked_ = false;
    };

    std::vector<WindowEvent> ScriptedWindow::next_script;

    /// Performs the motion operation using the supplied arguments.
    ///
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param delta_x `delta_x` value used by the operation.
    /// @param delta_y `delta_y` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] WindowEvent motion(f32 x, f32 y, f32 delta_x, f32 delta_y) {
        WindowEvent event{WindowEventKind::MouseMoved};
        event.mouse_move = WindowMouseMoveEvent{x, y, delta_x, delta_y, 0};
        return event;
    }


    /// Pumps script using the supplied arguments and current state.
    ///
    /// @param script `script` value used by the operation.
    /// @param policy `policy` value used by the operation.
    /// @param passed `passed` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<WindowEvent> pump_script(std::vector<WindowEvent> script, WindowManagerPolicy policy, bool &passed) {
        policy.event_pump_mode = WindowEventPumpMode::CallerThread;
        policy.platform_allows_threads = true;
        ScriptedWindow::next_script = std::move(script);

        WindowManager manager{policy};
        const auto spawned = manager.spawn_window<ScriptedWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "scripted window could not be spawned");
        if (!spawned) {
            return {};
        }

        vector<ManagedWindowEvents> packets;
        passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed");
        passed &= check(packets.size() == 1, "pump did not return exactly one window packet");
        vector<WindowEvent> events;
        if (packets.size() == 1) {
            events = packets[0].events;
        }
        manager.destroy_window(*spawned);
        ScriptedWindow::next_script.clear();
        return events;
    }


    /// Returns the current or globally available mouse motion coalesces without losing motion value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool mouse_motion_coalesces_without_losing_motion() {
        bool passed = true;

        WindowEvent press{WindowEventKind::MouseButtonPressed};
        press.mouse_button = WindowMouseButtonEvent{
            .button = 1, .clicks = 1, .x = 3.0F, .y = 3.0F, .button_code = MouseButton::Left};
        const std::vector<WindowEvent> script{
            motion(1.0F, 1.0F, 1.0F, 1.0F),
            motion(2.0F, 2.0F, 1.0F, 1.0F),
            motion(3.0F, 3.0F, 1.0F, 1.0F),
            press,
            motion(4.0F, 6.0F, 1.0F, 3.0F),
            motion(5.0F, 9.0F, 1.0F, 3.0F),
        };

        const vector<WindowEvent> coalesced = pump_script(script, WindowManagerPolicy{}, passed);
        passed &= check(coalesced.size() == 3, "adjacent mouse motion was not coalesced to one event per run");
        if (coalesced.size() == 3) {
            passed &= check(coalesced[0].kind == WindowEventKind::MouseMoved &&
                                coalesced[1].kind == WindowEventKind::MouseButtonPressed &&
                                coalesced[2].kind == WindowEventKind::MouseMoved,
                            "coalescing reordered events or merged across a button press");
            passed &= check(coalesced[0].mouse_move.x == 3.0F && coalesced[0].mouse_move.y == 3.0F,
                            "coalesced event did not keep the newest position");
            passed &= check(coalesced[0].mouse_move.delta_x == 3.0F && coalesced[0].mouse_move.delta_y == 3.0F,
                            "coalescing lost motion from the first run");
            passed &= check(coalesced[2].mouse_move.delta_x == 2.0F && coalesced[2].mouse_move.delta_y == 6.0F,
                            "coalescing lost motion from the second run");
        }


        const vector<WindowEvent> raw = pump_script(script, WindowManagerPolicy{.coalesce_mouse_motion = false}, passed);
        passed &= check(raw.size() == script.size(), "coalesce_mouse_motion=false still merged motion events");
        return passed;
    }


    /// Reports whether accumulator is bounded and keeps state latches.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool accumulator_is_bounded_and_keeps_state_latches() {
        bool passed = true;
        constexpr usize cap = 8;


        std::vector<WindowEvent> script;
        for (usize i = 0; i < cap * 4; ++i) {
            script.push_back(motion(static_cast<f32>(i), 0.0F, 1.0F, 0.0F));
            script.push_back(WindowEvent{WindowEventKind::FocusGained});
        }
        script.push_back(WindowEvent{WindowEventKind::CloseRequested});

        ScriptedWindow::next_script = script;
        WindowManager manager{WindowManagerPolicy{
            .event_pump_mode = WindowEventPumpMode::CallerThread,
            .platform_allows_threads = true,
            .max_accumulated_events_per_window = cap,
        }};
        const auto spawned = manager.spawn_window<ScriptedWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "scripted window could not be spawned");
        if (spawned) {
            vector<ManagedWindowEvents> packets;
            passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed");
            if (packets.size() == 1) {
                passed &= check(packets[0].events.size() <= cap, "accumulator grew past its configured cap");
                passed &= check(packets[0].close_requested,
                                "CloseRequested latch was lost among events dropped at the cap");
            }
            manager.destroy_window(*spawned);
        }
        ScriptedWindow::next_script.clear();
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = mouse_motion_coalesces_without_losing_motion();
    passed &= accumulator_is_bounded_and_keeps_state_latches();
    return passed ? 0 : 1;
}
