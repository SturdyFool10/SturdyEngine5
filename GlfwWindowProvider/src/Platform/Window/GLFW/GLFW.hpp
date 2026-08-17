#pragma once

#include <Platform/Platform.hpp>
#include <Platform/Window/GLFW/Window.hpp>

namespace SFT::Platform::Windowing::GLFW {

    /// Explicit factory referenced by products that choose the optional provider. Merely building or
    /// linking Sturdy::GlfwWindowProvider does not retain it in a static executable.
    [[nodiscard]] expected<unique_ptr<Window>, WindowError>
    create_window(const WindowConfig &config) noexcept;

} // namespace SFT::Platform::Windowing::GLFW
