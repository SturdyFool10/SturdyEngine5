module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#pragma endregion

export module Sturdy.Platform:WindowNative;

import :WindowError;
import :WindowConfig;

using std::expected;

export namespace SFT::Platform::Windowing::Detail {

    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_glfw(void *window) noexcept;
    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window) noexcept;

} // namespace SFT::Platform::Windowing::Detail
