#pragma once

#include <expected>

#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowError.hpp>

namespace SFT::Platform::Windowing::GLFW::Detail {

    /// Returns the requested native window handle.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::expected<NativeWindowHandle, WindowError>
    native_window_handle(void *window) noexcept;


    /// Applies composition window style using the supplied arguments and current state.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void apply_composition_window_style(void *window_handle) noexcept;


    using ImePreeditCallback = void (*)(const char *utf8Text, int cursorPos, void *userData);


    /// Performs the install ime composition hook operation using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param callback Callable invoked by the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept;


    /// Removes the ime composition hook from its owning collection or system.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void remove_ime_composition_hook(void *window_handle) noexcept;


    /// Sets the ime composition exclude rect from the supplied value.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_composition_exclude_rect(void *window_handle, int x, int y, int width, int height) noexcept;


    /// Sets the ime enabled for this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_enabled(void *window_handle, bool enabled) noexcept;

} // namespace SFT::Platform::Windowing::GLFW::Detail
