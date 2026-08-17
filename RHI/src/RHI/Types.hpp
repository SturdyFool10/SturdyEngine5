#pragma once

#include <Foundation/src/Foundation.hpp>

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


    struct Rect2D {
        i32 x = 0;
        i32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };


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
