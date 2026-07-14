#pragma once

#include <Foundation/Foundation.hpp>

#include "Types.hpp"
#include "Handles.hpp"
#include "Resources.hpp"

namespace SFT::RHI {

    // The windowing system backing a surface. The RHI takes neutral `void*` native handles rather
    // than depending on Platform/Core — a backend turns (system, display, window) into its own
    // surface object (Vulkan: a VkSurfaceKHR). Mirrors the neutral shape Core::RenderSurface uses,
    // but redeclared here so the RHI stays dependency-free.
    enum class WindowSystem : u32 {
        Unknown,
        Win32,
        Xlib,
        Xcb,
        Wayland,
        Cocoa,
        Android,
        UIKit,
    };

    struct SurfaceDesc {
        WindowSystem system = WindowSystem::Unknown;
        void *display = nullptr; // native display/connection (X11 Display*, wl_display*, HINSTANCE, ANativeWindow owner)
        void *window = nullptr;  // native window/layer (X11 Window, wl_surface*, HWND, NSView*/CAMetalLayer*, ANativeWindow*)
        const char *label = nullptr;
    };

    // How presented images are queued to the display. Mirrors the common WSI set; a backend picks
    // the nearest supported mode and reports it back (Fifo is the always-available fallback, the one
    // present mode every implementation must support).
    enum class PresentMode : u32 {
        // vsync, no tearing, never drops — the guaranteed-supported default.
        Fifo,
        // vsync, but allows a late frame to tear rather than stall (may be unsupported on some WSIs,
        // e.g. native Wayland — a backend should fall back to Fifo).
        FifoRelaxed,
        // triple-buffered, no tearing, drops stale frames for latency.
        Mailbox,
        // no synchronization, tears — lowest latency.
        Immediate,
    };

    enum class CompositeAlphaMode : u32 {
        // Prefer opaque presentation when supported; backend falls back to a supported alpha mode.
        Auto,
        Opaque,
        Premultiplied,
        PostMultiplied,
        Inherit,
    };

    enum class ColorSpace : u32 {
        SrgbNonlinear,
        Hdr10St2084,
    };

    struct SwapchainDesc {
        SurfaceHandle surface{};
        u32 width = 0;
        u32 height = 0;
        Format format = Format::BGRA8UnormSrgb;
        ColorSpace color_space = ColorSpace::SrgbNonlinear;
        PresentMode present_mode = PresentMode::Fifo;
        // How the swapchain image will be used by renderer code. ColorAttachment is the normal final
        // render target; TransferSrc/TransferDst cover screenshots, blit-based compositors, and debug copies.
        TextureUsage usage = TextureUsage::ColorAttachment;
        CompositeAlphaMode composite_alpha = CompositeAlphaMode::Auto;
        bool clipped = true;
        // Desired images in the swapchain (0 = backend's choice, typically 2–3). The backend clamps
        // to the surface's supported range and reports the actual count.
        u32 image_count = 0;
        // Optional retiring swapchain for resize/recreation handoff. Backends may pass this to native
        // APIs such as Vulkan's oldSwapchain/DXGI resize path to reuse presentation resources. The old
        // handle remains caller-owned: destroy it after this creation succeeds; keep it on failure.
        SwapchainHandle old_swapchain{};
        const char *label = nullptr;
    };

    // One acquired swapchain image, ready to render into and then present. `image_index` identifies
    // the backing image for the matching present() call; `suboptimal` is set when the swapchain
    // still works but should be rebuilt soon (e.g. a resize is pending).
    struct SurfaceTexture {
        SwapchainHandle swapchain{};
        TextureHandle texture{};
        TextureViewHandle view{};
        u32 image_index = 0;
        bool suboptimal = false;
    };

    struct PresentDesc {
        SurfaceTexture texture{};
        const char *label = nullptr;
    };

} // namespace SFT::RHI
