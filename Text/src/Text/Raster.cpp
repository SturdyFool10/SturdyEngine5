#include "Raster.hpp"

namespace SFT::Text {

RasterFormat select_raster_format(f32 long_side_px, RasterFormat previous) noexcept {
        constexpr f32 upshift_threshold = 32.0f;
        constexpr f32 downshift_threshold = 28.0f;
        if (previous == RasterFormat::MSDF) {
            return long_side_px < downshift_threshold ? RasterFormat::SDF : RasterFormat::MSDF;
        }
        return long_side_px > upshift_threshold ? RasterFormat::MSDF : RasterFormat::SDF;
    }

} // namespace SFT::Text

namespace SFT::Text::Detail {

msdfgen::Shape to_msdfgen_shape(const GlyphOutline &outline) {
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

u8 to_unorm_byte(float value) noexcept {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<u8>(clamped * 255.0f + 0.5f);
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

TextExpected<RasterizedGlyph> rasterize_glyph(const GlyphOutline &outline, RasterFormat format,
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

        // Where this specific raster sits relative to the pen (baseline origin) — see
        // RasterizedGlyph's doc comment. Derived directly from the same translate_x/translate_y
        // this glyph's own ink was anchored by: bounds.l maps to pixel column padding_px (by
        // construction of translate_x above), so pixel column of the pen (shape x=0) is
        // `translate_x * scale` from the raster's left edge, i.e. `padding_px - bounds.l * scale`
        // — bearing_x below is that quantity's sign flipped to an additive pen-relative offset.
        // Symmetric for Y, then re-based from msdfgen's bottom-up bitmap row convention (row 0 =
        // bottom) to this function's top-to-bottom output (row 0 = top) by subtracting from height.
        result.bearing_x = static_cast<f32>(bounds.l * scale - static_cast<double>(params.padding_px));
        result.bearing_top =
            static_cast<f32>(static_cast<double>(params.height) - static_cast<double>(params.padding_px) + bounds.b * scale);

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
                // msdfgen's bitmap is bottom-left origin (row 0 = bottom scanline); the atlas
                // image is top-left origin, so the row order must be flipped on the way out.
                const int src_y = height - 1 - y;
                for (int x = 0; x < width; ++x) {
                    result.pixels[static_cast<usize>(y) * params.width + x] =
                        Detail::to_unorm_byte(bitmap(x, src_y)[0]);
                }
            }
        } else {
            msdfgen::Bitmap<float, 3> bitmap(width, height);
            msdfgen::generateMSDF(bitmap, shape, transformation);
            result.pixels.resize(static_cast<usize>(params.width) * params.height * 3);
            for (int y = 0; y < height; ++y) {
                const int src_y = height - 1 - y;
                for (int x = 0; x < width; ++x) {
                    const float *pixel = bitmap(x, src_y);
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
