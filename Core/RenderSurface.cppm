module;
#include <Foundation/Foundation.hpp>

export module Sturdy.Core:RenderSurface;

import Sturdy.Platform;

export namespace SFT::Core {

    // Who owns the native window handle. The renderer uses this to choose the helper path
    // for surface creation (SDL_Vulkan_CreateSurface, glfwCreateWindowSurface, or native WSI).
    enum class SurfaceProvider {
        Unknown,
        Native,
        SDL3,
        GLFW,
    };

    // The windowing system backing a render surface. API-agnostic: a Vulkan backend turns
    // this into a VkSurfaceKHR, a future Metal backend into a CAMetalLayer, etc.
    enum class SurfaceSystem {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
    };

    // A neutral, non-owning description of a window the renderer can present into.
    // `provider_window` is the provider-specific window object (SDL_Window*, GLFWwindow*, etc.).
    // `display`/`window` are native OS handles for direct WSI paths and diagnostics.
    struct RenderSurfaceDescriptor {
        SurfaceProvider provider = SurfaceProvider::Unknown;
        SurfaceSystem system = SurfaceSystem::Unknown;
        void *display = nullptr;         // native display/connection (X11 Display*, wl_display*, HINSTANCE)
        void *window = nullptr;          // native window (X11 Window, wl_surface*, HWND, NSWindow*)
        void *provider_window = nullptr; // SDL_Window*, GLFWwindow*, etc.; interpreted by provider
    };

    struct Extent2D {
        u32 width = 0;
        u32 height = 0;

        [[nodiscard]] constexpr bool is_zero() const noexcept { return width == 0 || height == 0; }
    };

    // Stable handle used by the engine/glue to address one window's backend-side surface.
    // Backed directly by the owning window's WindowId; renderer-owned presentation resources are
    // addressed through RHI handles.
    struct RenderSurfaceHandle {
        Platform::Windowing::WindowId window_id = Platform::Windowing::invalid_window_id;

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return window_id != Platform::Windowing::invalid_window_id;
        }

        friend constexpr bool operator==(RenderSurfaceHandle, RenderSurfaceHandle) noexcept = default;
    };

} // namespace SFT::Core
