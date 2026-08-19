#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::RHI {


    enum class Format : u32 {
        Undefined = 0,


        R8Unorm,
        R8Snorm,
        R8Uint,
        R8Sint,
        RG8Unorm,
        RG8Snorm,
        RG8Uint,
        RG8Sint,
        RGBA8Unorm,
        RGBA8UnormSrgb,
        RGBA8Snorm,
        RGBA8Uint,
        RGBA8Sint,
        BGRA8Unorm,
        BGRA8UnormSrgb,


        RGB10A2Unorm,
        RG11B10Float,


        R16Uint,
        R16Sint,
        R16Float,
        RG16Uint,
        RG16Sint,
        RG16Float,
        RGBA16Uint,
        RGBA16Sint,
        RGBA16Float,


        R32Uint,
        R32Sint,
        R32Float,
        RG32Uint,
        RG32Sint,
        RG32Float,
        RGBA32Uint,
        RGBA32Sint,
        RGBA32Float,


        D16Unorm,
        D24UnormS8Uint,
        D32Float,
        D32FloatS8Uint,


        BC1Unorm,
        BC1UnormSrgb,
        BC3Unorm,
        BC3UnormSrgb,
        BC4Unorm,
        BC5Unorm,
        BC7Unorm,
        BC7UnormSrgb,
    };


    /// Reports whether format is block compressed.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool format_is_block_compressed(Format format) noexcept {
        switch (format) {
            case Format::BC1Unorm:
            case Format::BC1UnormSrgb:
            case Format::BC3Unorm:
            case Format::BC3UnormSrgb:
            case Format::BC4Unorm:
            case Format::BC5Unorm:
            case Format::BC7Unorm:
            case Format::BC7UnormSrgb:
                return true;
            default:
                return false;
        }
    }


    /// Reports whether format has depth.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool format_has_depth(Format format) noexcept {
        switch (format) {
            case Format::D16Unorm:
            case Format::D24UnormS8Uint:
            case Format::D32Float:
            case Format::D32FloatS8Uint:
                return true;
            default:
                return false;
        }
    }


    /// Reports whether format has stencil.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool format_has_stencil(Format format) noexcept {
        return format == Format::D24UnormS8Uint || format == Format::D32FloatS8Uint;
    }

    /// Reports whether format is depth stencil.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool format_is_depth_stencil(Format format) noexcept {
        return format_has_depth(format) || format_has_stencil(format);
    }


    enum class SampleCount : u32 {
        X1 = 1,
        X2 = 2,
        X4 = 4,
        X8 = 8,
        X16 = 16,
    };

    enum class ResolveMode : u32 {
        SampleZero,
        Average,
        Minimum,
        Maximum,
    };

    enum class IndexFormat : u32 {
        Uint16,
        Uint32,
    };

    // The axis-aligned fragment sizes both D3D12 VRS Tier 2 and Vulkan VK_KHR_fragment_shading_rate
    // guarantee as a conformant baseline (1x4/4x1 and other non-power-of-2-aligned combinations are
    // deliberately excluded -- neither API guarantees those universally).
    enum class ShadingRate : u32 {
        X1x1,
        X1x2,
        X2x1,
        X2x2,
        X2x4,
        X4x2,
        X4x4,
    };

    // How a shading rate combines with the next stage's rate (pipeline -> primitive -> attachment).
    // `Combine` maps to D3D12_SHADING_RATE_COMBINER_SUM and Vulkan's
    // VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR -- these are believed analogous (both extend
    // coarseness rather than just picking one side) but this hasn't been empirically verified against
    // either spec's exact combine formula, so treat any visual difference between backends when using
    // `Combine` as a known unknown rather than a bug in one specific backend.
    enum class ShadingRateCombiner : u32 {
        Passthrough,
        Override,
        Min,
        Max,
        Combine,
    };


    struct Extent3D {
        u32 width = 1;
        u32 height = 1;
        u32 depth_or_layers = 1;
    };

    struct Extent2D {
        u32 width = 1;
        u32 height = 1;
    };

    struct Offset3D {
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
    };


    struct Rect2D {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    // A programmable MSAA sample position within a pixel, normalized to [0, 1) with (0,0) at the
    // pixel's top-left corner and (1,1) one full pixel step away -- Vulkan's native
    // VkSampleLocationEXT representation. D3D12's SetSamplePositions uses signed 1/16-pixel offsets
    // from pixel center instead (D3D12_SAMPLE_POSITION, range [-8,7]); the D3D12 backend converts.
    struct SampleLocation {
        f32 x = 0.5f;
        f32 y = 0.5f;
    };

    // Floating-point viewport, in the same top-left-origin pixel space as Rect2D.
    // `min_depth`/`max_depth` map the [0,1] clip-space depth range.
    //
    // ─── Clip-space convention ──────────────────────────────────────────────────
    // The RHI standardizes on the "traditional" +Y-up NDC every common 3D math library produces by
    // default (glm::perspectiveRH_ZO/orthoRH_ZO, D3D12's native convention) — not Vulkan's native
    // +Y-down NDC. A shader is written once, against this one convention, with zero backend-specific
    // code — no macro, no per-vertex sign multiply, no C++-supplied "which backend am I" scalar:
    //
    //   * NDC X ∈ [-1, 1], +X right.
    //   * NDC Y ∈ [-1, 1], +Y up — y = 1 is the TOP of the viewport.
    //   * NDC Z ∈ [0, 1], near plane at 0.
    //   * Texture/UV origin is top-left, matching the pixel spaces above.
    //
    // How each backend reaches that convention:
    //   * D3D12   — native; nothing is touched. Camera::projection_matrix(), every matrix multiply,
    //               every SV_Position write is used exactly as authored.
    //   * Vulkan  — native NDC is +Y down, the opposite of the RHI convention above. Rather than
    //               touch a single matrix or shader, the Vulkan backend flips its *viewport*: negative
    //               height (VkViewport.height = -height, y += height — core Vulkan since 1.1 via
    //               VK_KHR_maintenance1, explicitly meant for this D3D-interop case; this engine
    //               requires Vulkan 1.4 unconditionally, so no feature check is needed). See
    //               VulkanRhiBridgeCommands.cpp's to_vk_viewport doc comment. This is the *only* place
    //               in the engine that reconciles the two conventions. Depth needs no adjustment
    //               (Vulkan is already [0, 1]).
    //
    // Consequences worth knowing:
    //   * Viewports and scissor rects are never translated on any backend — only the Vulkan viewport's
    //     height/origin above.
    //   * Raster FrontFace is not another coordinate conversion. It is mapped 1:1 by each backend;
    //     applying an additional Vulkan-only inversion here double-corrects winding and culls the
    //     visible side of ordinary meshes.
    //   * A shader never needs to think about any of this: write SV_Position from a plain
    //     matrix-vector multiply (or, for screen-space passes, plain `uv * 2 - 1` NDC math) and it
    //     renders identically on every backend.
    //
    // Do NOT compensate for any of this in matrix or shader code — that defeats the point. If a
    // draw looks vertically mirrored on one backend, the bug is in RHI backend plumbing (viewport
    // setup or a FrontFace conversion), not in application/shader code.
    struct Viewport {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 min_depth = 0.0f;
        f32 max_depth = 1.0f;
    };


    using ClearColor = glm::vec4;

    struct ClearDepthStencil {
        f32 depth = 1.0f;
        u32 stencil = 0;
    };

} // namespace SFT::RHI
