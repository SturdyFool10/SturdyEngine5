#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <Core/Core.hpp>
#include <Renderer/Handles.hpp>
#include <Renderer/RendererModule.hpp>

/// 3D colour lookup tables for the colour grade (`RenderSettings::ColorGradeSettings::lut_texture`).
///
/// A `ColorLut` is `size` x `size` x `size` RGB values, red varying fastest (the `.cube` order). The grade reads it in a *log*
/// domain: input = log2(scene-linear / 0.18) mapped from -8..+8 stops to 0..1, output decoded the same way, so an identity LUT
/// (`make_identity_color_lut`) changes nothing and an authored one covers the whole dynamic range. `.cube` files are read as
/// they are (their `DOMAIN_MIN/MAX` are ignored: the values are taken to already be in that 0..1 log domain).
///
/// ```cpp
/// auto lut = Engine::load_cube_lut("Looks/bodycam.cube");
/// auto texture = Engine::create_color_lut(*engine.renderer(), *lut);
/// graph.color_grade().enabled = true;
/// graph.color_grade().lut_texture = texture->value;
/// ```
namespace SFT::Engine {

    struct ColorLut {
        u32 size = 0;
        /// size^3 RGB triples, red fastest, then green, then blue.
        std::vector<f32> rgb;

        [[nodiscard]] bool valid() const noexcept { return size >= 2 && rgb.size() == static_cast<usize>(size) * size * size * 3u; }
    };

    /// The LUT that changes nothing (each entry is its own normalised coordinate).
    [[nodiscard]] ColorLut make_identity_color_lut(u32 size);

    /// Parses an Adobe/Iridas `.cube` 3D LUT. Fails (with a message) on a missing or 1D `LUT_*_SIZE`, a size outside 2..256, or a
    /// value count that does not match.
    [[nodiscard]] std::expected<ColorLut, std::string> parse_cube_lut(std::string_view text);

    /// Reads and parses a `.cube` file.
    [[nodiscard]] std::expected<ColorLut, std::string> load_cube_lut(const std::string &path);

    /// The LUT as the texture the grade samples: an atlas `size * size` wide and `size` tall (one `size` x `size` slice per blue
    /// value, left to right, red along x and green along y inside a slice), RGBA16F, row-major. Pure CPU.
    [[nodiscard]] std::vector<u16> color_lut_to_half_atlas(const ColorLut &lut);

    /// Uploads a LUT and returns its texture handle (`.value` goes into `ColorGradeSettings::lut_texture`). The caller owns the
    /// texture and frees it with `Renderer::destroy_texture`.
    [[nodiscard]] Core::RendererExpected<Renderer::TextureHandle> create_color_lut(Renderer::Renderer &renderer, const ColorLut &lut);

} // namespace SFT::Engine
