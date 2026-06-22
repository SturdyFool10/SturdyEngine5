module;

#include "Platform/Window/SDL3/SE_SDL3.hpp"

export module Sturdy.Platform.SDL3;

export import Sturdy.Platform;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused"
#endif

export namespace SFT::Platform::Windowing::SDL3 {
    using ::SFT::Platform::Windowing::SDL3::SDL3Window;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
