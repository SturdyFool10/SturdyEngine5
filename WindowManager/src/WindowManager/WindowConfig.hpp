#pragma once

#include <Foundation/Foundation.hpp>

#include <WindowManager/WindowGeometry.hpp>

namespace SFT::WindowManager {

                                                                                                  
                                                                                                      
                                                                                 
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
        Web,
    };

    struct NativeWindowHandle {
        NativeWindowSystem system = NativeWindowSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
        /// CSS selector (e.g. "#canvas") identifying the HTML canvas backing this window.
        /// Only meaningful when `system == NativeWindowSystem::Web`; owned by the Window that
        /// returned this handle, valid for at least as long as that Window.
        const char *canvas_selector = nullptr;
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
                                                                                           
                                                                                                      
                                                                                                        
                                                                                                        
                                                                                                       
                                                                                                        
                                                                                                       
        bool transparent = false;
        WindowMode mode = WindowMode::Windowed;
        WindowGraphicsApi graphics_api = WindowGraphicsApi::None;
    };

} // namespace SFT::WindowManager
