#include <Renderer/Text/Raster.hpp>

namespace SFT::Text {

/// Selects raster format that best satisfies the supplied requirements.
///
/// @param long_side_px `long_side_px` value used by the operation.
/// @param previous `previous` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
RasterFormat select_raster_format(f32 long_side_px, RasterFormat previous) noexcept {
        constexpr f32 upshift_threshold = 32.0f;
        constexpr f32 downshift_threshold = 28.0f;
        if (previous == RasterFormat::MSDF) {
            return long_side_px < downshift_threshold ? RasterFormat::SDF : RasterFormat::MSDF;
        }
        return long_side_px > upshift_threshold ? RasterFormat::MSDF : RasterFormat::SDF;
    }

/// Performs the glyph bounds operation for `Text` using the supplied arguments.
///
/// @param outline `outline` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GlyphBounds glyph_bounds(const GlyphOutline &outline) {
        if (outline.contours.empty()) {
            return {};
        }
        const msdfgen::Shape shape = Detail::to_msdfgen_shape(outline);
        const msdfgen::Shape::Bounds bounds = shape.getBounds();
        return GlyphBounds{
            .left = static_cast<f32>(bounds.l),
            .bottom = static_cast<f32>(bounds.b),
            .right = static_cast<f32>(bounds.r),
            .top = static_cast<f32>(bounds.t),
            .empty = false,
        };
    }

} // namespace SFT::Text

namespace SFT::Text::Detail {

/// Converts the value to msdfgen shape representation.
///
/// @param outline `outline` value used by the operation.
///
/// @return Returns the value converted to msdfgen shape representation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


                            break;
                    }
                    current = to;
                }
            }
            return shape;
        }

/// Converts the value to unorm byte representation.
///
/// @param value Value consumed by the operation.
///
/// @return Returns the value converted to unorm byte representation.
/// @note This function does not throw exceptions.
u8 to_unorm_byte(float value) noexcept {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<u8>(clamped * 255.0f + 0.5f);
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

/// Rasterizes glyph using the supplied arguments and current state.
///
/// @param outline `outline` value used by the operation.
/// @param format Format used for the resource, render target, or conversion.
/// @param params `params` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`.
TextExpected<RasterizedGlyph> rasterize_glyph(const GlyphOutline &outline, RasterFormat format,
                                                                       const RasterParams &params) {
        if (params.width == 0 || params.height == 0) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a glyph into a zero-sized raster.");
        }
        if (format == RasterFormat::Color) {
            return text_error(TextErrorCode::InvalidArgument,
                              "Color glyphs must be rasterized with rasterize_color_glyph().");
        }
        if (!(params.scale > 0.0f) || !(params.pixel_range > 0.0f) || params.padding_px < 0.0f) {
            return text_error(TextErrorCode::InvalidArgument,
                              "Glyph raster scale/range must be positive and padding must be non-negative.");
        }

        RasterizedGlyph result;
        result.width = params.width;
        result.height = params.height;
        result.channel_count = format == RasterFormat::MSDF ? 3 : 1;

        if (outline.contours.empty()) {


            result.pixels.assign(static_cast<usize>(params.width) * params.height * result.channel_count, 0);
            return result;
        }

        msdfgen::Shape shape = Detail::to_msdfgen_shape(outline);


        shape.orientContours();
        shape.normalize();
        if (format == RasterFormat::MSDF) {
            msdfgen::edgeColoringSimple(shape, 3.0);
        }

        const double scale = static_cast<double>(params.scale);
        const msdfgen::Shape::Bounds bounds = shape.getBounds();
        const double translate_x = params.translation
                                       ? static_cast<double>(params.translation->x)
                                       : static_cast<double>(params.padding_px) / scale - bounds.l;
        const double translate_y = params.translation
                                       ? static_cast<double>(params.translation->y)
                                       : static_cast<double>(params.padding_px) / scale - bounds.b;


        result.bearing_x = static_cast<f32>(-translate_x * scale);
        result.bearing_top = static_cast<f32>(static_cast<double>(params.height) - translate_y * scale);

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
