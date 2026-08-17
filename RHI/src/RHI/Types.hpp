#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::RHI {



    /// A practical cross-API texel/vertex format set — the common ground every desktop API (Vulkan,
    /// D3D12, Metal) and WebGPU agree on. Not exhaustive (no ASTC/ETC block-compression, no exotic
    /// packed formats yet); those get added when a real asset pipeline needs them rather than
    /// enumerated speculatively — BC7/BC5/BC4 landed for exactly that reason (Engine::AssetManager's
    /// texture-compression pipeline). `Undefined` is the zero value so a default-constructed
    /// descriptor is obviously unset.
    enum class Format : u32 {
        Undefined = 0,

        /// 8-bit
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

        /// Packed 32-bit
        RGB10A2Unorm,
        RG11B10Float,

        /// 16-bit
        R16Uint,
        R16Sint,
        R16Float,
        RG16Uint,
        RG16Sint,
        RG16Float,
        RGBA16Uint,
        RGBA16Sint,
        RGBA16Float,

        /// 32-bit
        R32Uint,
        R32Sint,
        R32Float,
        RG32Uint,
        RG32Sint,
        RG32Float,
        RGBA32Uint,
        RGBA32Sint,
        RGBA32Float,

        /// Depth / stencil
        D16Unorm,
        D24UnormS8Uint,
        D32Float,
        D32FloatS8Uint,

        /// Block-compressed (4x4 texel blocks) — lossy. BC7 stores full RGBA and is a drop-in
        /// replacement for RGBA8Unorm/RGBA8UnormSrgb in any sampling shader (no shader changes
        /// needed). BC5 (2-channel, e.g. tangent-space normal maps with a shader-side Z
        /// reconstruction) and BC4 (1-channel, e.g. a standalone mask/occlusion texture) are wired
        /// into Engine::AssetManager::create_texture's kind-aware compression policy
        /// (Engine::TextureKind / Engine::Detail::choose_bc_format). BC1 (opaque RGB, half BC7's
        /// size — used only for confirmed-alpha-irrelevant color textures) and BC3 (RGBA, smooth
        /// interpolated alpha) round out the same policy. BC6H (HDR) and BC2 are not wired into any
        /// pipeline yet — added when a real asset pipeline needs them, same as every format above.
        BC1Unorm,
        BC1UnormSrgb,
        BC3Unorm,
        BC3UnormSrgb,
        BC4Unorm,
        BC5Unorm,
        BC7Unorm,
        BC7UnormSrgb,
    };

    /// True for any block-compressed format (4x4 texel blocks, byte size computed per-block rather
    /// than per-texel — see Renderer::texture_data_bytes).
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

    /// True for any format usable as a depth attachment (with or without a packed stencil).
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

    /// True for any format carrying a stencil aspect.
    [[nodiscard]] constexpr bool format_has_stencil(Format format) noexcept {
        return format == Format::D24UnormS8Uint || format == Format::D32FloatS8Uint;
    }

    [[nodiscard]] constexpr bool format_is_depth_stencil(Format format) noexcept {
        return format_has_depth(format) || format_has_stencil(format);
    }



    /// MSAA sample count for an attachment/texture. `X1` means no multisampling.
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



    /// A texel-space size. `depth`/`array` default to 1 so 2D is the no-extra-fields common case.
    struct Extent3D {
        u32 width = 1;
        u32 height = 1;
        u32 depth_or_layers = 1;
    };

    struct Offset3D {
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
    };

    /// Integer pixel rectangle — scissor rects, copy/blit regions, render areas.
    ///
    /// Always framebuffer pixels with a top-left origin and +Y running down, on every backend. This
    /// matches D3D12 and Metal natively, so unlike Viewport (below) these rects are never flipped in
    /// translation — only clip space differs between APIs, not pixel space.
    struct Rect2D {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    /// Floating-point viewport, in the same top-left-origin pixel space as Rect2D.
    /// `min_depth`/`max_depth` map the [0,1] clip-space depth range.
    ///
    /// ─── Clip-space convention ──────────────────────────────────────────────────
    /// The RHI standardizes on the "traditional" +Y-up NDC every common 3D math library produces by
    /// default (glm::perspectiveRH_ZO/orthoRH_ZO, D3D12's native convention) — not Vulkan's native
    /// +Y-down NDC. A shader is written once, against this one convention, with zero backend-specific
    /// code — no macro, no per-vertex sign multiply, no C++-supplied "which backend am I" scalar:
    ///
    ///   * NDC X ∈ [-1, 1], +X right.
    ///   * NDC Y ∈ [-1, 1], +Y up — y = 1 is the TOP of the viewport.
    ///   * NDC Z ∈ [0, 1], near plane at 0.
    ///   * Texture/UV origin is top-left, matching the pixel spaces above.
    ///
    /// How each backend reaches that convention:
    ///   * D3D12   — native; nothing is touched. Camera::projection_matrix(), every matrix multiply,
    ///               every SV_Position write is used exactly as authored.
    ///   * Vulkan  — native NDC is +Y down, the opposite of the RHI convention above. Rather than
    ///               touch a single matrix or shader, the Vulkan backend flips its *viewport*: negative
    ///               height (VkViewport.height = -height, y += height — core Vulkan since 1.1 via
    ///               VK_KHR_maintenance1, explicitly meant for this D3D-interop case; this engine
    ///               requires Vulkan 1.4 unconditionally, so no feature check is needed). See
    ///               VulkanRhiBridgeCommands.cpp's to_vk_viewport doc comment. This is the *only* place
    ///               in the engine that reconciles the two conventions. Depth needs no adjustment
    ///               (Vulkan is already [0, 1]).
    ///
    /// Consequences worth knowing:
    ///   * Viewports and scissor rects are never translated on any backend — only the Vulkan viewport's
    ///     height/origin above.
    ///   * FrontFace IS inverted by the Vulkan backend (VulkanRhiConvert.hpp's to_vk(rhi::FrontFace)):
    ///     the negative-height viewport is a mirror, so it reverses the rasterizer's winding
    ///     classification there. D3D12's equivalent conversion is a plain 1:1 map — its viewport is
    ///     never touched. See FrontFace in Pipeline.hpp. Get this backwards and geometry lands in the
    ///     right place with the wrong half of it culled away. This applies equally to shadow-atlas
    ///     depth draws (Renderer::record_shadow_view_chunk) — they reuse the same material pipelines as
    ///     the camera pass.
    ///   * A shader never needs to think about any of this: write SV_Position from a plain
    ///     matrix-vector multiply (or, for screen-space passes, plain `uv * 2 - 1` NDC math) and it
    ///     renders identically on every backend.
    ///
    /// Do NOT compensate for any of this in matrix or shader code — that defeats the point. If a
    /// draw looks vertically mirrored on one backend, the bug is in RHI backend plumbing (viewport
    /// setup or a FrontFace conversion), not in application/shader code.
    struct Viewport {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 min_depth = 0.0f;
        f32 max_depth = 1.0f;
    };

    /// RGBA clear color, linear-space float components (glm to match the engine's geometry/math
    /// basis, which already standardizes on glm vectors).
    using ClearColor = glm::vec4;

    struct ClearDepthStencil {
        f32 depth = 1.0f;
        u32 stencil = 0;
    };

} // namespace SFT::RHI
