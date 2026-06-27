module;

export module Sturdy.Platform:WindowConfig;

import :WindowGeometry;

export namespace SFT::Platform::Windowing {

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
    };

    enum class OperatingSystem {
        Unknown,
        Windows,
        Linux,
        MacOS,
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
        WindowMode mode = WindowMode::Windowed;
        WindowGraphicsApi graphics_api = WindowGraphicsApi::None;
    };

} // namespace SFT::Platform::Windowing
