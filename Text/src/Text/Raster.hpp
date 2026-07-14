#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <msdfgen.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Outline.hpp"

using std::vector;

namespace SFT::Text {

    // Which encoding a rasterized glyph uses. SDF/MSDF are distance fields (see below); `Color` is
    // a flat RGBA8 raster with no distance math at all — color emoji glyphs (Text/ColorGlyph.cppm)
    // don't benefit from distance-field rescaling the way monochrome ink does, and real color-emoji
    // fonts ship fixed-size strikes/layers anyway, so they're just sampled straight through by the
    // text shader instead of going through the SDF/MSDF coverage-antialiasing path.
    enum class RasterFormat {
        SDF,
        MSDF,
        Color,
    };

    // Chooses SDF vs MSDF from a glyph's final on-screen long-side size, in pixels: above 32px,
    // corner sharpness becomes visible, so switch to MSDF; at or below, SDF is indistinguishable
    // and cheaper. Applies **hysteresis** (down-shift only below 28px) so a glyph whose projected
    // size oscillates near the threshold — a 3D label as the camera dollies, an animated UI scale
    // — doesn't thrash between formats and constantly re-rasterize into the atlas. Pass the
    // glyph's current cached format as `previous` (default SDF for a glyph seen for the first
    // time); 2D/UI callers that compute size once at layout time can simply omit `previous`.
    [[nodiscard]] inline RasterFormat select_raster_format(f32 long_side_px, RasterFormat previous = RasterFormat::SDF) noexcept {
        constexpr f32 upshift_threshold = 32.0f;
        constexpr f32 downshift_threshold = 28.0f;
        if (previous == RasterFormat::MSDF) {
            return long_side_px < downshift_threshold ? RasterFormat::SDF : RasterFormat::MSDF;
        }
        return long_side_px > upshift_threshold ? RasterFormat::MSDF : RasterFormat::SDF;
    }

    // A rasterized glyph: `width * height * channel_count` bytes, row-major, top-to-bottom,
    // channel values normalized to [0, 255] the way generateSDF/generateMSDF's distance mapping
    // already produces — 0.5 (encoded ~128) sits on the true glyph edge. `channel_count` is 1 for
    // RasterFormat::SDF, 3 for RasterFormat::MSDF (matching RHI::Format::R8Unorm/RGBA8Unorm* atlas
    // storage — see Renderer/TextAtlas.cppm).
    struct RasterizedGlyph {
        u32 width = 0;
        u32 height = 0;
        u32 channel_count = 0;
        vector<u8> pixels;
    };

    // Placement of the glyph within the output raster. `scale` converts font design units (see
    // Font::units_per_em()) to output pixels; `pixel_range` is the width of the encoded distance
    // band either side of the true edge, in output pixels (a larger range gives smoother
    // antialiasing falloff at the cost of atlas precision — 4px is a reasonable default);
    // `padding_px` reserves space between the glyph's ink bounding box and the raster edge so the
    // distance band isn't clipped. The caller (the atlas — Renderer/TextAtlas.cppm) picks
    // `width`/`height`/`scale` to fit its cell grid.
    struct RasterParams {
        u32 width = 0;
        u32 height = 0;
        f32 scale = 1.0f;
        f32 pixel_range = 4.0f;
        f32 padding_px = 4.0f;
    };

    namespace Detail {

        [[nodiscard]] inline msdfgen::Shape to_msdfgen_shape(const GlyphOutline &outline) {
            msdfgen::Shape shape;
            for (const Contour &contour : outline.contours) {
                if (contour.empty()) {
                    continue;
                }
                msdfgen::Contour &msdf_contour = shape.addContour();
                msdfgen::Point2 current(contour.front().to.x, contour.front().to.y);
                for (usize i = 1; i < contour.size(); ++i) {
                    const OutlineSegment &segment = contour[i];
                    const msdfgen::Point2 to(segment.to.x, segment.to.y);
                    switch (segment.kind) {
                        case OutlineSegmentKind::LineTo:
                            msdf_contour.addEdge(msdfgen::EdgeHolder(current, to));
                            break;
                        case OutlineSegmentKind::QuadTo:
                            msdf_contour.addEdge(msdfgen::EdgeHolder(
                                current, msdfgen::Point2(segment.control1.x, segment.control1.y), to));
                            break;
                        case OutlineSegmentKind::CubicTo:
                            msdf_contour.addEdge(msdfgen::EdgeHolder(
                                current, msdfgen::Point2(segment.control1.x, segment.control1.y),
                                msdfgen::Point2(segment.control2.x, segment.control2.y), to));
                            break;
                        case OutlineSegmentKind::MoveTo:
                            // Only meaningful as a contour's first segment (handled above); a
                            // stray one mid-contour would indicate a malformed outline, ignore it.
                            break;
                    }
                    current = to;
                }
            }
            return shape;
        }

        [[nodiscard]] inline u8 to_unorm_byte(float value) noexcept {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<u8>(clamped * 255.0f + 0.5f);
        }

    } // namespace Detail

    // Rasterizes `outline` into a signed distance field, single-channel (SDF) or multi-channel
    // (MSDF) per `format`. Built on msdfgen (Chlumsky) — `outline`'s contours map directly onto
    // msdfgen's own edge-segment representation (line/quadratic/cubic), so no flattening or
    // reparsing is needed between HarfBuzz's hb-draw output and msdfgen's shape input.
    [[nodiscard]] inline TextExpected<RasterizedGlyph> rasterize_glyph(const GlyphOutline &outline, RasterFormat format,
                                                                       const RasterParams &params) {
        if (params.width == 0 || params.height == 0) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a glyph into a zero-sized raster.");
        }

        RasterizedGlyph result;
        result.width = params.width;
        result.height = params.height;
        result.channel_count = format == RasterFormat::MSDF ? 3 : 1;

        if (outline.contours.empty()) {
            // A blank glyph (space, control characters, ...) — no ink, so every pixel encodes
            // "far outside" (0), which the text shader's coverage AA renders as fully transparent.
            result.pixels.assign(static_cast<usize>(params.width) * params.height * result.channel_count, 0);
            return result;
        }

        msdfgen::Shape shape = Detail::to_msdfgen_shape(outline);
        shape.normalize();
        if (format == RasterFormat::MSDF) {
            msdfgen::edgeColoringSimple(shape, 3.0);
        }

        const double scale = static_cast<double>(params.scale);
        const msdfgen::Shape::Bounds bounds = shape.getBounds();
        const double translate_x = static_cast<double>(params.padding_px) / scale - bounds.l;
        const double translate_y = static_cast<double>(params.padding_px) / scale - bounds.b;
        const msdfgen::Range range(static_cast<double>(params.pixel_range) / scale);
        const msdfgen::SDFTransformation transformation(
            msdfgen::Projection(msdfgen::Vector2(scale, scale), msdfgen::Vector2(translate_x, translate_y)), range);

        const int width = static_cast<int>(params.width);
        const int height = static_cast<int>(params.height);

        if (format == RasterFormat::SDF) {
            msdfgen::Bitmap<float, 1> bitmap(width, height);
            msdfgen::generateSDF(bitmap, shape, transformation);
            result.pixels.resize(static_cast<usize>(params.width) * params.height);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    result.pixels[static_cast<usize>(y) * params.width + x] = Detail::to_unorm_byte(bitmap(x, y)[0]);
                }
            }
        } else {
            msdfgen::Bitmap<float, 3> bitmap(width, height);
            msdfgen::generateMSDF(bitmap, shape, transformation);
            result.pixels.resize(static_cast<usize>(params.width) * params.height * 3);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const float *pixel = bitmap(x, y);
                    const usize offset = (static_cast<usize>(y) * params.width + x) * 3;
                    result.pixels[offset + 0] = Detail::to_unorm_byte(pixel[0]);
                    result.pixels[offset + 1] = Detail::to_unorm_byte(pixel[1]);
                    result.pixels[offset + 2] = Detail::to_unorm_byte(pixel[2]);
                }
            }
        }

        return result;
    }

} // namespace SFT::Text
