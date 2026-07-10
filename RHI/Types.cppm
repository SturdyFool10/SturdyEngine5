module;

#pragma region Imports
#include <glm/vec4.hpp>
#pragma endregion

export module Sturdy.RHI:Types;

import Sturdy.Foundation;

export namespace SFT::RHI {

    // ─── Formats ─────────────────────────────────────────────────────────────────

    // A practical cross-API texel/vertex format set — the common ground every desktop API (Vulkan,
    // D3D12, Metal) and WebGPU agree on. Not exhaustive (no BCn/ASTC/ETC block-compression, no
    // exotic packed formats yet); those get added when a real asset pipeline needs them rather than
    // enumerated speculatively. `Undefined` is the zero value so a default-constructed descriptor is
    // obviously unset.
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
    };

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
    struct Rect2D {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    // Floating-point viewport. `min_depth`/`max_depth` map the [0,1] clip-space depth range; a
    // flipped viewport (negative height) is expressed by the backend, not encoded here.
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
