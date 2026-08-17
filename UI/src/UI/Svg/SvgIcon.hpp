#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>
#pragma endregion

/// Thin wrapper over the vendored lunasvg library (Sturdy::LunaSvg) — parses and rasterizes an SVG
/// file to a straight-alpha RGBA8 bitmap. This is the only file that ever names a lunasvg type
/// (kept out of this header entirely, see SvgIconImpl.cpp): callers only ever see RasterizedSvg, a
/// plain engine-native struct. GPU upload/caching (Engine::UiSvgCache) is a separate, Engine-level
/// concern — UI:: itself stays engine-agnostic.
///
/// Previously this package hand-rolled its own SVG parser (single-tint geometry only, merged into
/// one Text::GlyphOutline and rasterized through the font-glyph SDF pipeline — no gradients, no
/// per-shape colors, no clip-paths, no <use>/<defs>, no CSS <style> blocks). lunasvg supports
/// essentially all of SVG 1.1 + SVG Tiny 1.2 (only animation/filters/scripts are out), at the cost
/// of rasterizing to a fixed-resolution bitmap rather than staying vector-sharp at any draw scale —
/// an accepted, deliberate tradeoff for being able to render real-world SVGs found in the wild.
namespace SFT::UI::Svg {

    struct RasterizedSvg {
        u32 width = 0;
        u32 height = 0;
        /// Straight-alpha RGBA8, row-major, tightly packed (width*height*4 bytes, no row padding) —
        /// matches Engine::TextureAssetDesc::rgba8's type exactly so Engine::UiSvgCache can
        /// std::move this straight into a TextureAssetDesc with no copy.
        std::vector<std::byte> rgba;
    };

    /// Loads and rasterizes the SVG at `path`, sized so the longer side of the document's own
    /// intrinsic (viewBox/width/height) dimensions comes out to `target_px` pixels — the same
    /// sizing contract the old hand-rolled rasterizer used. Returns nullopt if the file doesn't
    /// exist, isn't valid SVG, or has a zero-sized/degenerate intrinsic size.
    [[nodiscard]] std::optional<RasterizedSvg> rasterize_svg_file(const std::filesystem::path &path, f32 target_px);

} // namespace SFT::UI::Svg
