#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <GLFW/glfw3.h>

#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include <WindowManager/WindowManager.hpp>

using std::deque;
using std::expected;
using std::optional;
using std::unique_ptr;
using std::vector;

namespace SFT::WindowManager::GLFW {

    namespace Detail {

        /// Normalizes mouse button using the supplied arguments and current state.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr MouseButton normalize_mouse_button(int button) noexcept {
            switch (button) {
                case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
                case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
                case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
                case GLFW_MOUSE_BUTTON_4: return MouseButton::Extra1;
                case GLFW_MOUSE_BUTTON_5: return MouseButton::Extra2;
                case GLFW_MOUSE_BUTTON_6: return MouseButton::Extra3;
                case GLFW_MOUSE_BUTTON_7: return MouseButton::Extra4;
                case GLFW_MOUSE_BUTTON_8: return MouseButton::Extra5;
                default: return MouseButton::Unknown;
            }
        }

    } // namespace Detail

    /// Performs the GLFW close callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_close_callback(GLFWwindow *window);
    /// Performs the GLFW window pos callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_pos_callback(GLFWwindow *window, int x, int y);
    /// Performs the GLFW window size callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_size_callback(GLFWwindow *window, int width, int height);
    /// Performs the GLFW framebuffer size callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height);
    /// Performs the GLFW window focus callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param focused `focused` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_focus_callback(GLFWwindow *window, int focused);
    /// Performs the GLFW cursor enter callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param entered `entered` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_cursor_enter_callback(GLFWwindow *window, int entered);
    /// Performs the GLFW key callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param key Key used to identify the requested entry.
    /// @param scancode `scancode` value used by the operation.
    /// @param action `action` value used by the operation.
    /// @param mods `mods` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
    /// Performs the GLFW char callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param codepoint `codepoint` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_char_callback(GLFWwindow *window, unsigned int codepoint);
    /// Performs the GLFW cursor pos callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_cursor_pos_callback(GLFWwindow *window, f64 x, f64 y);
    /// Performs the GLFW mouse button callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param button `button` value used by the operation.
    /// @param action `action` value used by the operation.
    /// @param mods `mods` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
    /// Performs the GLFW scroll callback operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y);

    class GLFWWindow final : public Window {
      public:
        /// Destroys the `GLFWWindow` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~GLFWWindow() noexcept override;

        /// Constructs the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param key Key used to identify the requested entry.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static expected<unique_ptr<GLFWWindow>, WindowError> construct(ConstructorKey key, const WindowConfig &config) noexcept;

        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
        /// Returns the runtime or backend type represented by `GLFWWindow`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowingSystem type() const noexcept override;
        /// Returns the native backend handle associated with this `GLFWWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override;
        /// Returns the native window handle associated with this `GLFWWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override;

        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> pump_events() noexcept override;
        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override;
        /// Closes requested using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool close_requested() const noexcept override;
        /// Requests close using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void request_close() noexcept override;
        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool resized() const noexcept override;
        /// Returns the current or globally available consume resize value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override;

        /// Returns the current or globally available show value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> show() noexcept override;
        /// Returns the current or globally available hide value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> hide() noexcept override;
        /// Returns the current or globally available focus value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> focus() noexcept override;
        /// Returns the current or globally available raise value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> raise() noexcept override;
        /// Returns the current or globally available maximize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> maximize() noexcept override;
        /// Returns the current or globally available minimize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> minimize() noexcept override;
        /// Returns the current or globally available restore value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> restore() noexcept override;

        /// Sets the title for this `GLFWWindow`.
        ///
        /// @param title `title` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_title(const char *title) noexcept override;
        /// Returns the current or globally available position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override;
        /// Sets the position for this `GLFWWindow`.
        ///
        /// @param position `position` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_position(WindowPosition position) noexcept override;
        /// Returns the current or globally available global cursor position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override;
        /// Returns the size for this `GLFWWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override;
        /// Sets the size for this `GLFWWindow`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_size(WindowExtent extent) noexcept override;
        /// Returns the framebuffer size for this `GLFWWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override;
        /// Sets the minimum size for this `GLFWWindow`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept override;
        /// Sets the maximum size for this `GLFWWindow`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept override;

        /// Sets the resizable for this `GLFWWindow`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_resizable(bool enabled) noexcept override;
        /// Sets the decorated for this `GLFWWindow`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_decorated(bool enabled) noexcept override;
        /// Sets the fullscreen for this `GLFWWindow`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept override;
        /// Returns the current or globally available fullscreen mode value.
        ///
        /// @return Returns the current fullscreen mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowMode fullscreen_mode() const noexcept override;
        /// Sets the opacity for this `GLFWWindow`.
        ///
        /// @param opacity `opacity` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_opacity(f32 opacity) noexcept override;
        /// Returns the current or globally available opacity value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override;

        /// Sets the cursor visible for this `GLFWWindow`.
        ///
        /// @param visible `visible` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_visible(bool visible) noexcept override;
        /// Sets the cursor icon for this `GLFWWindow`.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_icon(CursorIcon icon) noexcept override;
        /// Sets the cursor grabbed for this `GLFWWindow`.
        ///
        /// @param grabbed `grabbed` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept override;
        /// Sets the relative mouse mode for this `GLFWWindow`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept override;
        /// Sets the mouse locked for this `GLFWWindow`.
        ///
        /// @param locked `locked` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override;
        /// Returns the current or globally available mouse locked value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_locked() const noexcept override;

        /// Enables window effect using the supplied arguments and current state.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
        /// Sets the effect for this `GLFWWindow`.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_effect(WindowEffect effect) noexcept override;
        /// Sets the blur enabled for this `GLFWWindow`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_blur_enabled(bool enabled) noexcept override;
        /// Sets the transparent for this `GLFWWindow`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_transparent(bool enabled) noexcept override;

        /// Returns the current or globally available required vulkan instance extensions value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept override;
        /// Creates a vulkan surface from the supplied parameters.
        ///
        /// @param instance Instance used or affected by the operation.
        /// @param allocation_callbacks Callable invoked by the operation.
        /// @param surface_out Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> create_vulkan_surface(
            void *instance,
            const void *allocation_callbacks,
            void *surface_out) const noexcept override;

        /// Returns the current or globally available clipboard text value.
        ///
        /// @return Returns the current clipboard text value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string clipboard_text() const noexcept override;
        /// Sets the clipboard text for this `GLFWWindow`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_clipboard_text(std::string_view text) noexcept override;

        /// Sets the text input area for this `GLFWWindow`.
        ///
        /// @param area `area` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_text_input_area(TextInputArea area) noexcept override;
        /// Starts text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> start_text_input() noexcept override;
        /// Stops text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> stop_text_input() noexcept override;

      private:
        friend class ::SFT::WindowManager::Window;
        /// Handles the GLFW close callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_close_callback(GLFWwindow *window);
        /// Handles the GLFW window pos callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_window_pos_callback(GLFWwindow *window, int x, int y);
        /// Handles the GLFW window size callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_window_size_callback(GLFWwindow *window, int width, int height);
        /// Handles the GLFW framebuffer size callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height);
        /// Handles the GLFW window focus callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param focused `focused` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_window_focus_callback(GLFWwindow *window, int focused);
        /// Handles the GLFW cursor enter callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param entered `entered` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_cursor_enter_callback(GLFWwindow *window, int entered);
        /// Handles the GLFW key callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param key Key used to identify the requested entry.
        /// @param scancode `scancode` value used by the operation.
        /// @param action `action` value used by the operation.
        /// @param mods `mods` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
        /// Handles the GLFW char callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param codepoint `codepoint` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_char_callback(GLFWwindow *window, unsigned int codepoint);
        /// Performs the GLFW native preedit trampoline operation using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        /// @param cursor_pos `cursor_pos` value used by the operation.
        /// @param user_data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_native_preedit_trampoline(const char *text, int cursor_pos, void *user_data);
        /// Handles the GLFW cursor pos callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_cursor_pos_callback(GLFWwindow *window, f64 x, f64 y);
        /// Handles the GLFW mouse button callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param button `button` value used by the operation.
        /// @param action `action` value used by the operation.
        /// @param mods `mods` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
        /// Handles the GLFW scroll callback callback and updates the associated platform state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        friend void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y);

        /// Constructs a `GLFWWindow` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        /// @param window Window used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        GLFWWindow(ConstructorKey key, GLFWwindow *window) noexcept;


        /// Closes requested locked using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool close_requested_locked() const noexcept;
        /// Returns the current or globally available size locked value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size_locked() const noexcept;
        /// Returns the current or globally available native window handle locked value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_locked() const noexcept;

        GLFWwindow *window_ = nullptr;
        deque<WindowEvent> events_;
        optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};
        f64 last_mouse_x_ = 0.0;
        f64 last_mouse_y_ = 0.0;
        bool has_last_mouse_position_ = false;
        bool mouse_locked_ = false;
        GLFWcursor *current_cursor_ = nullptr;
        optional<CursorIcon> current_cursor_icon_;


        WindowMode fullscreen_mode_ = WindowMode::Windowed;
    };

} // namespace SFT::WindowManager::GLFW
