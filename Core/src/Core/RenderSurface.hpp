#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Platform/Platform.hpp>

#include <glm/vec2.hpp>

namespace SFT::Core {

    // Which provider owns the presentation window. Surface creation itself is dispatched through
    // Window's provider-owned virtual seam; this identity remains useful for diagnostics and policy.
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
    // `display`/`window` are native OS handles retained for RHI import and diagnostics.
    struct RenderSurfaceDescriptor {
        SurfaceProvider provider = SurfaceProvider::Unknown;
        SurfaceSystem system = SurfaceSystem::Unknown;
        void *display = nullptr; // native display/connection (X11 Display*, wl_display*, HINSTANCE)
        void *window = nullptr;  // native window (X11 Window, wl_surface*, HWND, NSWindow*)
    };

    // Pixel dimensions of a window, swapchain, or attachment. Plain glm::uvec2 so extents take
    // part in the engine's vector math (and swizzles) directly instead of being converted at every
    // seam that already speaks glm — `.x` is the width, `.y` the height.
    using Extent2D = glm::uvec2;

    [[nodiscard]] constexpr bool is_zero(Extent2D extent) noexcept {
        return extent.x == 0 || extent.y == 0;
    }

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
