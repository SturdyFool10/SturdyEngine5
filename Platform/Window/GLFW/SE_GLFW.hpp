#pragma once

#include "../Window.hpp"

#include <deque>
#include <optional>

struct GLFWwindow;

namespace SFT::Platform::Windowing::GLFW {

    void glfw_close_callback(GLFWwindow *window);
    void glfw_window_pos_callback(GLFWwindow *window, int x, int y);
    void glfw_window_size_callback(GLFWwindow *window, int width, int height);
    void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height);
    void glfw_window_focus_callback(GLFWwindow *window, int focused);
    void glfw_cursor_enter_callback(GLFWwindow *window, int entered);
    void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods);
    void glfw_char_callback(GLFWwindow *window, unsigned int codepoint);
    void glfw_cursor_pos_callback(GLFWwindow *window, double x, double y);
    void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
    void glfw_scroll_callback(GLFWwindow *window, double x, double y);

    class GLFWWindow final : public Window {
      public:
        ~GLFWWindow() noexcept override;

        [[nodiscard]] static WindowExpected<std::unique_ptr<GLFWWindow>> construct(ConstructorKey key, const WindowConfig &config) noexcept;

        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
        [[nodiscard]] WindowingSystem type() const noexcept override;
        [[nodiscard]] void *native_backend_handle() const noexcept override;
        [[nodiscard]] NativeWindowHandle native_window_handle() const noexcept override;

        WindowResult pump_events() noexcept override;
        [[nodiscard]] std::optional<WindowEvent> poll_event() noexcept override;
        [[nodiscard]] bool close_requested() const noexcept override;
        void request_close() noexcept override;
        [[nodiscard]] bool resized() const noexcept override;
        [[nodiscard]] std::optional<WindowResize> consume_resize() noexcept override;

        WindowResult show() noexcept override;
        WindowResult hide() noexcept override;
        WindowResult focus() noexcept override;
        WindowResult raise() noexcept override;
        WindowResult maximize() noexcept override;
        WindowResult minimize() noexcept override;
        WindowResult restore() noexcept override;

        WindowResult set_title(const char *title) noexcept override;
        [[nodiscard]] WindowExpected<WindowPosition> position() const noexcept override;
        WindowResult set_position(WindowPosition position) noexcept override;
        [[nodiscard]] WindowExpected<WindowExtent> size() const noexcept override;
        WindowResult set_size(WindowExtent extent) noexcept override;
        [[nodiscard]] WindowExpected<WindowExtent> framebuffer_size() const noexcept override;
        WindowResult set_minimum_size(WindowExtent extent) noexcept override;
        WindowResult set_maximum_size(WindowExtent extent) noexcept override;

        WindowResult set_resizable(bool enabled) noexcept override;
        WindowResult set_decorated(bool enabled) noexcept override;
        WindowResult set_fullscreen(WindowMode mode) noexcept override;
        WindowResult set_opacity(float opacity) noexcept override;
        [[nodiscard]] WindowExpected<float> opacity() const noexcept override;

        WindowResult set_cursor_visible(bool visible) noexcept override;
        WindowResult set_cursor_grabbed(bool grabbed) noexcept override;
        WindowResult set_relative_mouse_mode(bool enabled) noexcept override;
        WindowResult set_mouse_locked(bool locked) noexcept override;
        [[nodiscard]] bool mouse_locked() const noexcept override;

        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
        WindowResult set_effect(WindowEffect effect) noexcept override;
        WindowResult set_blur_enabled(bool enabled) noexcept override;

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
        friend void glfw_cursor_pos_callback(GLFWwindow *window, double x, double y);
        friend void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods);
        friend void glfw_scroll_callback(GLFWwindow *window, double x, double y);

        GLFWWindow(ConstructorKey key, GLFWwindow *window) noexcept;

        GLFWwindow *window_ = nullptr;
        std::deque<WindowEvent> events_;
        std::optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};
        double last_mouse_x_ = 0.0;
        double last_mouse_y_ = 0.0;
        bool has_last_mouse_position_ = false;
        bool mouse_locked_ = false;
    };

} // namespace SFT::Platform::Windowing::GLFW
