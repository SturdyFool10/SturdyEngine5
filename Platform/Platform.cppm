module;

#include "Platform/Window/Window.hpp"

export module Sturdy.Platform;

export import Sturdy.Foundation;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused"
#endif

export namespace SFT::Platform::Windowing {
    using ::SFT::Platform::Windowing::LinuxBlurProtocol;
    using ::SFT::Platform::Windowing::NativeWindowHandle;
    using ::SFT::Platform::Windowing::NativeWindowSystem;
    using ::SFT::Platform::Windowing::OperatingSystem;
    using ::SFT::Platform::Windowing::Window;
    using ::SFT::Platform::Windowing::WindowBackendKind;
    using ::SFT::Platform::Windowing::WindowConfig;
    using ::SFT::Platform::Windowing::WindowEffect;
    using ::SFT::Platform::Windowing::WindowEffectKind;
    using ::SFT::Platform::Windowing::WindowEffectResult;
    using ::SFT::Platform::Windowing::WindowEffectResultKind;
    using ::SFT::Platform::Windowing::WindowError;
    using ::SFT::Platform::Windowing::WindowErrorCode;
    using ::SFT::Platform::Windowing::WindowEvent;
    using ::SFT::Platform::Windowing::WindowEventKind;
    using ::SFT::Platform::Windowing::WindowExpected;
    using ::SFT::Platform::Windowing::WindowExtent;
    using ::SFT::Platform::Windowing::WindowGraphicsApi;
    using ::SFT::Platform::Windowing::WindowingSystem;
    using ::SFT::Platform::Windowing::WindowKeyboardEvent;
    using ::SFT::Platform::Windowing::WindowMode;
    using ::SFT::Platform::Windowing::WindowMouseButtonEvent;
    using ::SFT::Platform::Windowing::WindowMouseMoveEvent;
    using ::SFT::Platform::Windowing::WindowMouseWheelEvent;
    using ::SFT::Platform::Windowing::WindowPosition;
    using ::SFT::Platform::Windowing::WindowResize;
    using ::SFT::Platform::Windowing::WindowResult;
    using ::SFT::Platform::Windowing::WindowTextInputEvent;
} // namespace SFT::Platform::Windowing

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
