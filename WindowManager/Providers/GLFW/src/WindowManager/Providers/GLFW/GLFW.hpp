#pragma once

#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/GLFW/Window.hpp>

namespace SFT::WindowManager::GLFW {


    /// Creates a window from the supplied parameters.
    ///
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<unique_ptr<Window>, WindowError>
    create_window(const WindowConfig &config) noexcept;

} // namespace SFT::WindowManager::GLFW
