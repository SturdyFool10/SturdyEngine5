module;

#pragma region Imports
#include <GLFW/glfw3.h>

#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

export module Sturdy.Platform.GLFW:Window;

#pragma region Imports
export import Sturdy.Platform;
#pragma endregion

using std::deque;
using std::expected;
using std::optional;
using std::unique_ptr;
using std::vector;

export namespace SFT::Platform::Windowing::GLFW {

    void glfw_close_callback(GLFWwindow *window);
    void glfw_window_pos_callback(GLFWwindow *window, int x, int y);
    void glfw_window_size_callback(GLFWwindow *window, int width, int height);
    void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height);
    void glfw_window_focus_callback(GLFWwindow *window, int focused);
    void glfw_cursor_enter_callback(GLFWwindow *window, int entered);
    void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
    void glfw_char_callback(GLFWwindow *window, unsigned int codepoint);
    void glfw_cursor_pos_callback(GLFWwindow *window, f64 x, f64 y);
    void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
    void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y);

    class GLFWWindow final : public Window {
      public:
        ~GLFWWindow() noexcept override;

        [[nodiscard]] static expected<unique_ptr<GLFWWindow>, WindowError> construct(ConstructorKey key, const WindowConfig &config) noexcept;

        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
        [[nodiscard]] WindowingSystem type() const noexcept override;
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override;
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override;

        expected<void, WindowError> pump_events() noexcept override;
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override;
        [[nodiscard]] bool close_requested() const noexcept override;
        void request_close() noexcept override;
        [[nodiscard]] bool resized() const noexcept override;
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override;

        expected<void, WindowError> show() noexcept override;
        expected<void, WindowError> hide() noexcept override;
        expected<void, WindowError> focus() noexcept override;
        expected<void, WindowError> raise() noexcept override;
        expected<void, WindowError> maximize() noexcept override;
        expected<void, WindowError> minimize() noexcept override;
        expected<void, WindowError> restore() noexcept override;

        expected<void, WindowError> set_title(const char *title) noexcept override;
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override;
        expected<void, WindowError> set_position(WindowPosition position) noexcept override;
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override;
        expected<void, WindowError> set_size(WindowExtent extent) noexcept override;
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override;
        expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept override;
        expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept override;

        expected<void, WindowError> set_resizable(bool enabled) noexcept override;
        expected<void, WindowError> set_decorated(bool enabled) noexcept override;
        expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept override;
        expected<void, WindowError> set_opacity(f32 opacity) noexcept override;
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override;

        expected<void, WindowError> set_cursor_visible(bool visible) noexcept override;
        expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept override;
        expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept override;
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override;
        [[nodiscard]] bool mouse_locked() const noexcept override;

        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
        expected<void, WindowError> set_effect(WindowEffect effect) noexcept override;
        expected<void, WindowError> set_blur_enabled(bool enabled) noexcept override;

        [[nodiscard]] expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept override;

      private:
        friend class ::SFT::Platform::Windowing::Window;
        friend void glfw_close_callback(GLFWwindow *window);
        friend void glfw_window_pos_callback(GLFWwindow *window, int x, int y);
        friend void glfw_window_size_callback(GLFWwindow *window, int width, int height);
        friend void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height);
        friend void glfw_window_focus_callback(GLFWwindow *window, int focused);
        friend void glfw_cursor_enter_callback(GLFWwindow *window, int entered);
        friend void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
        friend void glfw_char_callback(GLFWwindow *window, unsigned int codepoint);
        friend void glfw_cursor_pos_callback(GLFWwindow *window, f64 x, f64 y);
        friend void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
        friend void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y);

        GLFWWindow(ConstructorKey key, GLFWwindow *window) noexcept;

        GLFWwindow *window_ = nullptr;
        deque<WindowEvent> events_;
        optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};
        f64 last_mouse_x_ = 0.0;
        f64 last_mouse_y_ = 0.0;
        bool has_last_mouse_position_ = false;
        bool mouse_locked_ = false;
    };

} // namespace SFT::Platform::Windowing::GLFW
