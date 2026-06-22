module;

#include "Platform/Window/GLFW/SE_GLFW.hpp"

export module Sturdy.Platform.GLFW;

export import Sturdy.Platform;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused"
#endif

export namespace SFT::Platform::Windowing::GLFW {
    using ::SFT::Platform::Windowing::GLFW::GLFWWindow;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
