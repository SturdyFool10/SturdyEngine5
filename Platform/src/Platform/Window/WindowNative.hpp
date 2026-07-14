#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#pragma endregion

#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowConfig.hpp>

using std::expected;

namespace SFT::Platform::Windowing::Detail {

    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_glfw(void *window) noexcept;
    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window) noexcept;

} // namespace SFT::Platform::Windowing::Detail
