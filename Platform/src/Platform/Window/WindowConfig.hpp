#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Platform/Window/WindowGeometry.hpp>

namespace SFT::Platform::Windowing {

    // Which windowing library backs a live Window. This reports provider identity after explicit
    // provider composition; it is not a runtime selector. Products choose optional providers through
    // a WindowFactory so unreferenced provider archives remain dead-strippable.
    enum class WindowBackendKind {
        SDL3,
        GLFW,
    };

    enum class WindowMode {
        Windowed,
        BorderlessFullscreen,
        ExclusiveFullscreen,
    };

    enum class WindowGraphicsApi {
        None,
        Vulkan,
        OpenGL,
        Metal,
        Direct3D,
        WebGPU,
    };

    enum class OperatingSystem {
        Unknown,
        Windows,
        Linux,
        MacOS,
        Web,
    };

    enum class NativeWindowSystem {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
    };

    struct NativeWindowHandle {
        NativeWindowSystem system = NativeWindowSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
    };

    struct WindowConfig {
        const char *title = "Sturdy Engine";
        WindowExtent extent = {1280, 720};
        WindowPosition position = {0, 0};
        bool use_default_position = true;
        bool visible = true;
        bool resizable = true;
        bool decorated = true;
        bool high_dpi = true;
        // Per-pixel window transparency, honored at creation time by every backend (SDL3:
        // SDL_WINDOW_TRANSPARENT, GLFW: GLFW_TRANSPARENT_FRAMEBUFFER). This is the *only* way to get
        // a transparent window on Linux (X11's visual — the thing that determines whether a window's
        // framebuffer even has an alpha channel — can only be chosen at XCreateWindow time; there is
        // no protocol operation to change it on a live window), so toggling this on Linux means going
        // through Window::recreate() rather than Window::set_transparent(). Windows/macOS additionally
        // support toggling transparency live via Window::set_transparent() without needing this flag.
        bool transparent = false;
        WindowMode mode = WindowMode::Windowed;
        WindowGraphicsApi graphics_api = WindowGraphicsApi::None;
    };

} // namespace SFT::Platform::Windowing
