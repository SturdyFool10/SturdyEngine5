#pragma once

#include "Foundation/Types.hpp"

namespace SFT::Core {

    // The windowing system backing a render surface. API-agnostic: a Vulkan backend turns
    // this into a VkSurfaceKHR, a future Metal backend into a CAMetalLayer, etc.
    enum class SurfaceSystem {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
    };

    // A neutral description of the thing a renderer must draw into. Carries native handles for
    // a future hand-rolled surface path, plus the SDL_Window* when the host window is SDL3
    // backed (the robust path used today: SDL knows how to make a surface on any compositor).
    // This struct never mentions Vulkan, SDL, or Platform types so Core stays free of Platform.
    struct RenderSurfaceDescriptor {
        SurfaceSystem system = SurfaceSystem::Unknown;
        void* display = nullptr;     // native display/connection (X11 Display*, wl_display*, HINSTANCE)
        void* window = nullptr;      // native window (X11 Window, wl_surface*, HWND, NSWindow*)
        void* sdl_window = nullptr;  // SDL_Window* when the host window is SDL3-backed, else null
    };

    struct Extent2D {
        u32 width = 0;
        u32 height = 0;

        [[nodiscard]] constexpr bool is_zero() const noexcept { return width == 0 || height == 0; }
    };

} // namespace SFT::Core
