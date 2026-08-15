#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::RHI {

    // ─── Formats ─────────────────────────────────────────────────────────────────

    // A practical cross-API texel/vertex format set — the common ground every desktop API (Vulkan,
    // D3D12, Metal) and WebGPU agree on. Not exhaustive (no ASTC/ETC block-compression, no exotic
    // packed formats yet); those get added when a real asset pipeline needs them rather than
    // enumerated speculatively — BC7/BC5/BC4 landed for exactly that reason (Engine::AssetManager's
    // texture-compression pipeline). `Undefined` is the zero value so a default-constructed
    // descriptor is obviously unset.
    enum class Format : u32 {
        Undefined = 0,

        // 8-bit
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

        // Packed 32-bit
        RGB10A2Unorm,
        RG11B10Float,

        // 16-bit
        R16Uint,
        R16Sint,
        R16Float,
        RG16Uint,
        RG16Sint,
        RG16Float,
        RGBA16Uint,
        RGBA16Sint,
        RGBA16Float,

        // 32-bit
        R32Uint,
        R32Sint,
        R32Float,
        RG32Uint,
        RG32Sint,
        RG32Float,
        RGBA32Uint,
        RGBA32Sint,
        RGBA32Float,

        // Depth / stencil
        D16Unorm,
        D24UnormS8Uint,
        D32Float,
        D32FloatS8Uint,

        // Block-compressed (4x4 texel blocks) — lossy. BC7 stores full RGBA and is a drop-in
        // replacement for RGBA8Unorm/RGBA8UnormSrgb in any sampling shader (no shader changes
        // needed). BC5 (2-channel, e.g. tangent-space normal maps with a shader-side Z
        // reconstruction) and BC4 (1-channel, e.g. a standalone mask/occlusion texture) are wired
        // into Engine::AssetManager::create_texture's kind-aware compression policy
        // (Engine::TextureKind / Engine::Detail::choose_bc_format). BC1 (opaque RGB, half BC7's
        // size — used only for confirmed-alpha-irrelevant color textures) and BC3 (RGBA, smooth
        // interpolated alpha) round out the same policy. BC6H (HDR) and BC2 are not wired into any
        // pipeline yet — added when a real asset pipeline needs them, same as every format above.
        BC1Unorm,
        BC1UnormSrgb,
        BC3Unorm,
        BC3UnormSrgb,
        BC4Unorm,
        BC5Unorm,
        BC7Unorm,
        BC7UnormSrgb,
    };

    // True for any block-compressed format (4x4 texel blocks, byte size computed per-block rather
    // than per-texel — see Renderer::texture_data_bytes).
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

    // True for any format usable as a depth attachment (with or without a packed stencil).
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

    // True for any format carrying a stencil aspect.
    [[nodiscard]] constexpr bool format_has_stencil(Format format) noexcept {
        return format == Format::D24UnormS8Uint || format == Format::D32FloatS8Uint;
    }

    [[nodiscard]] constexpr bool format_is_depth_stencil(Format format) noexcept {
        return format_has_depth(format) || format_has_stencil(format);
    }

    // ─── Sampling / indexing ─────────────────────────────────────────────────────

    // MSAA sample count for an attachment/texture. `X1` means no multisampling.
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

    // ─── Geometry / clear PODs ───────────────────────────────────────────────────

    // A texel-space size. `depth`/`array` default to 1 so 2D is the no-extra-fields common case.
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

    // Integer pixel rectangle — scissor rects, copy/blit regions, render areas.
    //
    // Always framebuffer pixels with a top-left origin and +Y running down, on every backend. This
    // matches D3D12 and Metal natively, so unlike Viewport (below) these rects are never flipped in
    // translation — only clip space differs between APIs, not pixel space.
    struct Rect2D {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    // Floating-point viewport, in the same top-left-origin pixel space as Rect2D.
    // `min_depth`/`max_depth` map the [0,1] clip-space depth range.
    //
    // ─── Clip-space convention ──────────────────────────────────────────────────
    // The RHI standardizes on Vulkan's clip space, and each backend translates into its own native
    // convention. Shaders are written once against these rules and behave identically everywhere:
    //
    //   * NDC X ∈ [-1, 1], +X right.
    //   * NDC Y ∈ [-1, 1], **+Y down** — y = -1 is the TOP of the viewport.
    //   * NDC Z ∈ [0, 1], near plane at 0.
    //   * Face winding is evaluated in framebuffer space (+Y down), so FrontFace means the same
    //     thing on every backend and needs no per-backend inversion.
    //   * Texture/UV origin is top-left, matching the pixel spaces above.
    //
    // How each backend reaches that convention:
    //   * Vulkan  — native; viewports are passed through verbatim.
    //   * D3D12   — native NDC is +Y up. D3D12 exposes no legal API-level way to invert it: viewport,
    //               rasterizer and pipeline state all lack the knob, and D3D12_VIEWPORT::Height is
    //               documented as non-negative, so a negative-height viewport is out of spec even
    //               though hardware accepts it. The reconciliation is therefore a shader-side
    //               negation, applied by sturdy_clip_position() in Shaders/sturdy_common.slang. The
    //               sign is injected as STURDY_CLIP_Y_SIGN by the shader compiler (Core::Slang) from
    //               the target format, so no call site or shader author can forget it. Depth needs
    //               no adjustment (D3D12 is already [0, 1]).
    //
    // Consequences worth knowing:
    //   * Viewports and scissor rects are never translated on any backend — only clip space differs.
    //   * FrontFace IS inverted by the D3D12 backend: negating clip Y is a mirror, so it reverses
    //     the rasterizer's winding classification. See FrontFace in Pipeline.hpp. Get this wrong and
    //     geometry lands in the right place with the wrong half of it culled away.
    //   * Every vertex-stage entry point MUST emit SV_Position through sturdy_clip_position() (or a
    //     shared helper that already does, like fullscreenTrianglePosition/uiQuadClipPosition).
    //     A shader that writes SV_Position directly renders vertically mirrored on D3D12.
    //
    // Do NOT compensate for any of this in matrix code — projection matrices are built once, for
    // Vulkan's convention, and are correct on every backend.
    struct Viewport {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 min_depth = 0.0f;
        f32 max_depth = 1.0f;
    };

    // RGBA clear color, linear-space float components (glm to match the engine's geometry/math
    // basis, which already standardizes on glm vectors).
    using ClearColor = glm::vec4;

    struct ClearDepthStencil {
        f32 depth = 1.0f;
        u32 stencil = 0;
    };

} // namespace SFT::RHI
