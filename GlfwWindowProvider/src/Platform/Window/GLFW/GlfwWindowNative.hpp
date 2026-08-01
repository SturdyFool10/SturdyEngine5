#pragma once

#include <expected>

#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowError.hpp>

namespace SFT::Platform::Windowing::GLFW::Detail {

    [[nodiscard]] std::expected<NativeWindowHandle, WindowError>
    native_window_handle(void *window) noexcept;

} // namespace SFT::Platform::Windowing::GLFW::Detail
