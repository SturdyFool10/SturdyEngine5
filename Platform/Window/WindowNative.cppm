module;

#pragma region Imports
#include <expected>
#pragma endregion

export module Sturdy.Platform:WindowNative;

#pragma region Imports
import :WindowError;
import :WindowConfig;
#pragma endregion

using std::expected;

export namespace SFT::Platform::Windowing::Detail {

    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_glfw(void *window) noexcept;
    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window) noexcept;

} // namespace SFT::Platform::Windowing::Detail
