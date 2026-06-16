#pragma once

#include "../Window.hpp"

struct GLFWwindow;

namespace SFT::Platform::Windowing::GLFW {

class GLFWWindow final : public Window {
public:
    ~GLFWWindow() noexcept override;

    [[nodiscard]] static WindowExpected<std::unique_ptr<GLFWWindow>> construct(ConstructorKey key, const WindowConfig& config) noexcept;

    [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
    [[nodiscard]] void* native_backend_handle() const noexcept override;
    [[nodiscard]] NativeWindowHandle native_window_handle() const noexcept override;

    WindowResult pump_events() noexcept override;
    [[nodiscard]] bool close_requested() const noexcept override;
    void request_close() noexcept override;

    WindowResult show() noexcept override;
    WindowResult hide() noexcept override;
    WindowResult focus() noexcept override;
    WindowResult raise() noexcept override;
    WindowResult maximize() noexcept override;
    WindowResult minimize() noexcept override;
    WindowResult restore() noexcept override;

    WindowResult set_title(const char* title) noexcept override;
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

    [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
    WindowResult set_effect(WindowEffect effect) noexcept override;
    WindowResult set_blur_enabled(bool enabled) noexcept override;

private:
    friend class ::SFT::Platform::Windowing::Window;

    GLFWWindow(ConstructorKey key, GLFWwindow* window) noexcept;

    GLFWwindow* window_ = nullptr;
};

} // namespace SFT::Platform::Windowing::GLFW
