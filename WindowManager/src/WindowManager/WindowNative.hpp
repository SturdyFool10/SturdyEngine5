#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>

using std::expected;

namespace SFT::WindowManager::Detail {

    /// Performs the native window handle from SDL operation using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window) noexcept;

} // namespace SFT::WindowManager::Detail
