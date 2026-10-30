#pragma once

#include <Foundation/Foundation.hpp>

#include <optional>

#include <WindowManager/WindowFeatures.hpp>
#include <WindowManager/WindowGeometry.hpp>

namespace SFT::WindowManager {

                                                                                                  
                                                                                                      
                                                                                 
    class Window;

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
        /// Initial client-area size in physical pixels, on every platform and provider.
        WindowExtent extent = {1280, 720};
        WindowPosition position = {0, 0};
        bool use_default_position = true;
        bool visible = true;
        bool resizable = true;
        bool decorated = true;
        /// Requests a full-resolution framebuffer on platforms that scale windows (macOS, Wayland,
        /// Web). When false those platforms render at window-coordinate resolution and upscale.
        bool high_dpi = true;
                                                                                           
                                                                                                      
                                                                                                        
                                                                                                        
                                                                                                       
                                                                                                        
                                                                                                       
        bool transparent = false;
        WindowMode mode = WindowMode::Windowed;
        WindowGraphicsApi graphics_api = WindowGraphicsApi::None;
        /// Reverse-DNS application id (`org.example.App`): the Wayland `app_id`, X11 `WM_CLASS` and what desktop
        /// files / Flatpak match the window to. Process-wide and read when the window system is first initialised, so
        /// set it on the first window. Null = the window system's default.
        const char *app_id = nullptr;

        // -- Creation-time window behaviour (the union surface; each is applied through the matching `Window` setter
        //    right after the window exists, and one a provider cannot honour is skipped with a warning). --
        bool always_on_top = false;
        bool maximized = false;
        bool minimized = false;
        /// False creates a window that never takes keyboard focus (overlays, palettes).
        bool focusable = true;
        /// Mouse clicks fall through the window.
        bool mouse_passthrough = false;
        /// Unset leaves the provider's default (fullscreen windows minimise on focus loss on most systems).
        std::optional<bool> auto_minimize_on_focus_loss;
        WindowType window_type = WindowType::Normal;
        /// A child of this window (it must outlive the child's creation, not the child); null for a top-level window.
        Window *parent = nullptr;
        bool modal = false;
        /// 0 leaves a dimension unconstrained.
        WindowExtent min_size = {0, 0};
        WindowExtent max_size = {0, 0};
        /// Width / height limits; 0 and 0 mean none.
        f32 aspect_min = 0.0f;
        f32 aspect_max = 0.0f;
        /// The display and mode used when the window goes fullscreen (including creation in a fullscreen `mode`).
        FullscreenTarget fullscreen_target{};
    };

} // namespace SFT::WindowManager
