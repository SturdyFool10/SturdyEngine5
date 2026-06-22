#pragma once

#include "Window.hpp"

struct GLFWwindow;
struct SDL_Window;

namespace SFT::Platform::Windowing::Detail {

    [[nodiscard]] NativeWindowHandle native_window_handle_from_glfw(GLFWwindow *window) noexcept;
    [[nodiscard]] NativeWindowHandle native_window_handle_from_sdl(SDL_Window *window) noexcept;

} // namespace SFT::Platform::Windowing::Detail
