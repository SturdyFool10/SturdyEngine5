#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#pragma endregion

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
    // present mode every implementation must support). Application/gameplay code never names one of
    // these directly — see PresentStrategy below and Core::PresentationSettings (Core/Renderer.hpp)
    // for the layers that sit in front of it.
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
        // Fifo's ordering/tear-free guarantee, but at vblank the presentation engine may skip ahead
        // to the latest *ready* queued request instead of strictly the next one in line — lower
        // latency than plain Fifo with the same never-tears guarantee. Backed by
        // VK_KHR_present_mode_fifo_latest_ready (RHI::Feature::PresentModeFifoLatestReady) — see
        // that feature's own doc comment for why it's engine-enabled whenever the device supports
        // it rather than gated behind an app request.
        FifoLatestReady,
    };

    // Human-readable names for logs/diagnostics/debug overlays — shared by every layer that needs
    // to print one of these (the Vulkan bridge's resolution logging, a Renderer debug overlay, ...)
    // so there's exactly one spelling of each enumerator's display name, not one per call site.
    [[nodiscard]] constexpr std::string_view present_mode_name(PresentMode mode) noexcept {
        switch (mode) {
            case PresentMode::Fifo: return "Fifo";
            case PresentMode::FifoRelaxed: return "FifoRelaxed";
            case PresentMode::Mailbox: return "Mailbox";
            case PresentMode::Immediate: return "Immediate";
            case PresentMode::FifoLatestReady: return "FifoLatestReady";
        }
        return "Unknown";
    }

    // A backend-agnostic presentation *strategy* — what Core::resolve_present_strategy()
    // (Core/Renderer.hpp) turns an app's Core::PresentationSettings intent into, and the only thing
    // choose_present_mode() below consumes. Kept independent of any single PresentMode so a
    // different graphics backend (e.g. a future WebGPU RHI) could implement the same strategies
    // with its own present-mode set, and so gameplay/settings code never has to name a raw
    // PresentMode itself.
    enum class PresentStrategy : u32 {
        // Tearing allowed, no intentional vertical-blank wait — lowest possible latency.
        Unsynchronized,
        // Tear-free, ordered vertical-blank presentation — standard "VSync On." This is Fifo's
        // exact match, not a fallback: don't prefer Mailbox/FifoLatestReady here just because
        // they're available (see present_mode_preference()'s own comment).
        TearFreeOrdered,
        // Tear-free, but the newest completed frame always replaces older queued ones — "VSync On,
        // low latency" and "VSync Off" once tearing itself isn't actually available.
        TearFreeLatest,
        // Tear-free normally, but tearing is permitted when a frame is late rather than stalling a
        // full extra refresh interval — the classic "Adaptive VSync" toggle.
        AdaptiveTearing,
        // Fifo's ordering/tear-free guarantee, but skips ahead to the latest ready request at
        // vblank instead of strictly the next queued one.
        TearFreeLatestReady,
        // Plain Fifo, deliberately: variable-refresh-rate presentation is a display/compositor-
        // level pacing behavior, not a distinct Vulkan present mode — see
        // Core::VariableRefreshMode's own doc comment (Core/Renderer.hpp). If the display/compositor
        // never actually engages VRR, this degrades gracefully to ordinary Fifo, not an error.
        VariableRefresh,
    };

    [[nodiscard]] constexpr std::string_view present_strategy_name(PresentStrategy strategy) noexcept {
        switch (strategy) {
            case PresentStrategy::Unsynchronized: return "Unsynchronized";
            case PresentStrategy::TearFreeOrdered: return "TearFreeOrdered";
            case PresentStrategy::TearFreeLatest: return "TearFreeLatest";
            case PresentStrategy::AdaptiveTearing: return "AdaptiveTearing";
            case PresentStrategy::TearFreeLatestReady: return "TearFreeLatestReady";
            case PresentStrategy::VariableRefresh: return "VariableRefresh";
        }
        return "Unknown";
    }

    // `strategy`'s present modes in priority order, most-preferred first, padded to length 4 by
    // repeating the last real entry (harmless — choose_present_mode() below just returns the first
    // supported match, so a repeated trailing entry is never re-examined for a different reason).
    // Every list's terminal entry is Fifo, the one mode every conformant Vulkan implementation is
    // required to support, so choose_present_mode() is guaranteed to find *something* even on a
    // maximally restrictive surface (native Wayland commonly supports only Fifo, sometimes
    // FifoRelaxed).
    [[nodiscard]] constexpr std::array<PresentMode, 4> present_mode_preference(PresentStrategy strategy) noexcept {
        switch (strategy) {
            case PresentStrategy::Unsynchronized:
                return {PresentMode::Immediate, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::TearFreeOrdered:
                return {PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo};
            case PresentStrategy::TearFreeLatest:
                return {PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::AdaptiveTearing:
                return {PresentMode::FifoRelaxed, PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::Fifo};
            case PresentStrategy::TearFreeLatestReady:
                return {PresentMode::FifoLatestReady, PresentMode::Mailbox, PresentMode::FifoLatestReady, PresentMode::Fifo};
            case PresentStrategy::VariableRefresh:
                return {PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo};
        }
        return {PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo, PresentMode::Fifo};
    }

    // First entry of present_mode_preference(strategy) that's actually in `supported` (the surface's
    // real, freshly-queried present-mode list — never assume a mode is available because it was
    // available on another GPU/window/monitor/OS/surface). Always terminates: every strategy's real
    // terminal fallback is Fifo, required-supported per spec, so the trailing `return Fifo` here is
    // unreachable in practice and kept only as a safe default.
    [[nodiscard]] inline PresentMode choose_present_mode(std::span<const PresentMode> supported,
                                                          PresentStrategy strategy) noexcept {
        for (PresentMode candidate : present_mode_preference(strategy)) {
            if (std::ranges::contains(supported, candidate)) {
                return candidate;
            }
        }
        return PresentMode::Fifo;
    }

    // Requested-vs-effective presentation state, for diagnostics (logs, a future debug overlay,
    // support reports) — the engine must never silently claim the requested strategy took effect
    // when the surface actually forced a fallback. `degraded` is true whenever `effective_mode`
    // isn't `present_mode_preference(strategy)[0]` (the strategy's own ideal mode).
    struct PresentationResolution {
        PresentStrategy strategy = PresentStrategy::TearFreeOrdered;
        PresentMode effective_mode = PresentMode::Fifo;
        bool degraded = false;
        // Whether this swapchain's vkQueuePresentKHR actually goes out on the compute queue rather
        // than the graphics queue — set once at swapchain-creation time from SwapchainDesc's own
        // allow_present_from_compute intent resolved against real per-family surface support (see
        // that field's doc comment). False whenever the request was denied, disabled, or the device
        // has no compute queue at all — presentation stays on the graphics queue in every such case.
        bool present_queue_is_compute = false;
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
        // scRGB: linear light encoded directly in a float format (Format::RGBA16Float is the
        // conventional pairing), 1.0 == the SDR reference white, values beyond 1.0 (and below 0.0,
        // for out-of-gamut colors) are legal — no PQ/HLG curve, no fixed-function tonemap needed on
        // the way out. Backed by VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT. Prefer this over
        // Hdr10St2084 when the compositor/OS supports it: it avoids baking a peak-nits assumption
        // into the output encode, so the OS/display handles tone mapping to its own real capability.
        ScrgbLinear,
        // Hybrid Log-Gamma (ARIB STD-B67 / BT.2100 HLG): the broadcast-oriented HDR transfer function
        // (BBC/NHK), scene-referred and self-describing — no HDR metadata is needed or sent for this
        // mode (see hlgEncode() in sturdy_common.slang), unlike Hdr10St2084. Backed by
        // VK_COLOR_SPACE_HDR10_HLG_EXT; despite the "HDR10" in the Vulkan enumerant name this is a
        // distinct, unrelated-to-HDR10 transfer function (Vulkan's naming, not this codebase's).
        // Pairs with a 10-bit UNORM format (RGB10A2Unorm), same as Hdr10St2084 — like PQ, HLG's OETF
        // output is a normalized [0,1] code value, not a linear-with-headroom one like ScrgbLinear.
        Hdr10Hlg,
        // Dolby Vision (VK_COLOR_SPACE_DOLBYVISION_EXT). Best-effort plumbing only: real Dolby Vision
        // requires per-frame dynamic metadata in Dolby's proprietary "RPU" format, which needs a
        // licensed Dolby SDK to produce — nothing here generates it. Selecting this color space is
        // therefore expected to report Unsupported on the overwhelming majority of real displays
        // (Dolby Vision output requires OS/driver-level certification this engine doesn't have), and
        // even where a surface *does* report the format pair, the shader falls back to plain PQ
        // encoding (pqEncode(), same as Hdr10St2084) as the closest available approximation — this is
        // not certified Dolby Vision output, just "don't crash, do something visually reasonable if a
        // surface ever claims to accept it."
        DolbyVision,

        // ── Wide color *gamut* spaces — orthogonal to HDR ──
        // All still nominally [0,1]-range SDR content, just against wider primaries than sRGB/BT.709.
        // Deliberately NOT part of Core::HdrColorSpaceMode/PresentationSettings::hdr_enabled: selecting
        // one of these correctly requires the renderer to actually chromatically adapt/gamut-map its
        // output to the target primaries, which this engine does not do yet (its internal working
        // space is sRGB/BT.709 primaries throughout) — tagging a swapchain with a wider gamut without
        // remapping the content would misrepresent colors to the display, not just render
        // conservatively. Real, selectable RHI-layer capability (create_swapchain() negotiates them
        // against the surface exactly like any other ColorSpace) for a future color-management pass to
        // build on; Core::PresentationSettings/Engine don't expose them today.
        AdobeRgbLinear,
        AdobeRgbNonlinear,
        DisplayP3Linear,
        DisplayP3Nonlinear,
        Bt2020Linear,
    };

    struct SwapchainDesc {
        SurfaceHandle surface{};
        u32 width = 0;
        u32 height = 0;
        Format format = Format::BGRA8UnormSrgb;
        ColorSpace color_space = ColorSpace::SrgbNonlinear;
        // The backend resolves this to a concrete PresentMode against the surface's actual
        // supported-mode list at creation time (choose_present_mode() above) — never a raw
        // PresentMode from the caller.
        PresentStrategy present_strategy = PresentStrategy::TearFreeOrdered;
        // How the swapchain image will be used by renderer code. ColorAttachment is the normal final
        // render target; TransferSrc/TransferDst cover screenshots, blit-based compositors, and debug copies.
        TextureUsage usage = TextureUsage::ColorAttachment;
        CompositeAlphaMode composite_alpha = CompositeAlphaMode::Auto;
        bool clipped = true;
        // Desired images in the swapchain (0 = backend's choice, typically 2–3). The backend clamps
        // to the surface's supported range and reports the actual count.
        u32 image_count = 0;
        // The resolved CPU-side frames-in-flight count (Core::RendererCapabilities::
        // max_frames_in_flight, itself produced by Core::resolve_frames_in_flight — see its doc
        // comment) — deliberately a separate field from image_count, not derived from it. This is
        // what sizes the backend's acquisition-semaphore ring: one semaphore per frame slot, indexed
        // by the caller's frame-slot index (acquire_next_texture's frame_slot_index parameter), so a
        // semaphore's reuse safety is proven by that slot's own submission fence rather than by a
        // free-running cursor unrelated to GPU completion. 0 falls back to image_count (or the
        // backend's own image-count default if that's also 0) purely so callers that genuinely have
        // no frames-in-flight concept (tests, tools) still get a sane, nonzero ring size.
        u32 frames_in_flight = 0;
        // Optional retiring swapchain for resize/recreation handoff. Backends may pass this to native
        // APIs such as Vulkan's oldSwapchain/DXGI resize path to reuse presentation resources. The old
        // handle remains caller-owned: destroy it after this creation succeeds; keep it on failure.
        SwapchainHandle old_swapchain{};
        // Engine intent for presenting from the compute queue instead of graphics when the device/
        // surface actually support it — see Core::PresentationSettings::allow_present_from_compute
        // (Core/Renderer.hpp), the layer that resolves into this field, for the full contract. The
        // backend resolves this against real per-family surface support at creation time, the same
        // "never assume, always ask the surface" discipline present_strategy above already follows.
        bool allow_present_from_compute = true;
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
        // Optional completion fence for the presentation operation itself, distinct from a graphics
        // submission fence. Backends attach it only when Feature::SwapchainMaintenance (or an
        // equivalent native capability) is enabled; it signals when this present no longer keeps the
        // swapchain alive, allowing a resize to retire only the old presentation resources.
        FenceHandle completion_fence{};
        const char *label = nullptr;
    };

    // Distinguishes the non-error outcomes a successful present can still report. Suboptimal and
    // OutOfDate both mean the present itself went through, but differ in urgency: Suboptimal means
    // the swapchain still works and can keep presenting, OutOfDate means it should stop being used
    // for new acquisitions until rebuilt. Real errors (device/surface/fullscreen-exclusive loss) are
    // reported through RhiError, not this enum -- see RhiErrorCode::SurfaceLost/
    // FullScreenExclusiveLost/DeviceLost.
    enum class PresentOutcome {
        Success,
        Suboptimal,
        OutOfDate,
    };

} // namespace SFT::RHI
