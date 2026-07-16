#define STB_IMAGE_IMPLEMENTATION
#include "ColorGlyph.hpp"

#include <cmath>

namespace SFT::Text {

ColorGlyphFormat detect_color_format(const Font &font, u32 glyph_id) {
        if (!font) {
            return ColorGlyphFormat::None;
        }
        hb_face_t *face = font.face_handle();

        if (hb_ot_color_has_layers(face)) {
            unsigned int probe_capacity = 0;
            const unsigned int total_layers =
                hb_ot_color_glyph_get_layers(face, static_cast<hb_codepoint_t>(glyph_id), 0, &probe_capacity, nullptr);
            if (total_layers > 0) {
                return ColorGlyphFormat::Layered;
            }
        }

        if (hb_ot_color_has_png(face)) {
            // PNG presence is per-glyph (each glyph either has a strike or doesn't), so the only
            // reliable check is trying to reference it — hb_ot_color_has_png() only says the table
            // exists at all.
            hb_blob_t *blob = hb_ot_color_glyph_reference_png(font.handle(), static_cast<hb_codepoint_t>(glyph_id));
            const bool has_png = blob != nullptr && blob != hb_blob_get_empty();
            if (blob != nullptr) {
                hb_blob_destroy(blob);
            }
            if (has_png) {
                return ColorGlyphFormat::Bitmap;
            }
        }

        return ColorGlyphFormat::None;
    }

} // namespace SFT::Text

namespace SFT::Text::Detail {

TextExpected<RasterizedGlyph> rasterize_bitmap_glyph(const Font &font, u32 glyph_id,
                                                                                   const ColorRasterParams &params) {
            const auto ppem = static_cast<unsigned int>(params.pixel_size + 0.5f);
            // Atlas misses are rasterized in parallel. Never mutate the shared Font's ppem while
            // selecting a bitmap strike; a HarfBuzz sub-font gives this job private mutable state.
            hb_font_t *raster_font = hb_font_create_sub_font(font.handle());
            if (raster_font == nullptr || raster_font == hb_font_get_empty()) {
                if (raster_font != nullptr) {
                    hb_font_destroy(raster_font);
                }
                return text_error(TextErrorCode::RasterizationFailed,
                                  "Failed to create a HarfBuzz font for color bitmap rasterization.");
            }
            hb_font_set_ppem(raster_font, ppem, ppem);

            hb_blob_t *blob = hb_ot_color_glyph_reference_png(raster_font, static_cast<hb_codepoint_t>(glyph_id));
            if (blob == nullptr || blob == hb_blob_get_empty()) {
                if (blob != nullptr) {
                    hb_blob_destroy(blob);
                }
                hb_font_destroy(raster_font);
                return text_error(TextErrorCode::RasterizationFailed, "Glyph has no PNG bitmap.");
            }

            unsigned int length = 0;
            const char *data = hb_blob_get_data(blob, &length);
            int decoded_width = 0;
            int decoded_height = 0;
            int decoded_channels = 0;
            stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(data), static_cast<int>(length),
                                                    &decoded_width, &decoded_height, &decoded_channels, 4);
            hb_blob_destroy(blob);
            if (pixels == nullptr) {
                hb_font_destroy(raster_font);
                return text_error(TextErrorCode::RasterizationFailed, "Failed to decode glyph PNG bitmap.");
            }

            RasterizedGlyph result;
            result.width = params.width;
            result.height = params.height;
            result.channel_count = 4;
            result.pixels.assign(static_cast<usize>(params.width) * params.height * 4, 0);

            const u32 padding = std::min(
                static_cast<u32>(std::ceil(std::max(params.padding_px, 0.0f))),
                std::min(params.width / 2u, params.height / 2u));
            const u32 target_width = std::max(1u, params.width - 2u * padding);
            const u32 target_height = std::max(1u, params.height - 2u * padding);

            // Bilinear resampling in premultiplied-alpha space avoids both the jagged nearest-
            // neighbor result and dark RGB fringes around translucent strike pixels.
            for (u32 y = 0; y < target_height; ++y) {
                const f32 source_y = (static_cast<f32>(y) + 0.5f) * static_cast<f32>(decoded_height) /
                                         static_cast<f32>(target_height) -
                                     0.5f;
                const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, decoded_height - 1);
                const int y1 = std::min(y0 + 1, decoded_height - 1);
                const f32 fy = std::clamp(source_y - static_cast<f32>(y0), 0.0f, 1.0f);
                for (u32 x = 0; x < target_width; ++x) {
                    const f32 source_x = (static_cast<f32>(x) + 0.5f) * static_cast<f32>(decoded_width) /
                                             static_cast<f32>(target_width) -
                                         0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, decoded_width - 1);
                    const int x1 = std::min(x0 + 1, decoded_width - 1);
                    const f32 fx = std::clamp(source_x - static_cast<f32>(x0), 0.0f, 1.0f);
                    const array<f32, 4> weights{
                        (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy),
                        (1.0f - fx) * fy, fx * fy,
                    };
                    const array<usize, 4> offsets{
                        (static_cast<usize>(y0) * decoded_width + x0) * 4,
                        (static_cast<usize>(y0) * decoded_width + x1) * 4,
                        (static_cast<usize>(y1) * decoded_width + x0) * 4,
                        (static_cast<usize>(y1) * decoded_width + x1) * 4,
                    };

                    f32 alpha = 0.0f;
                    array<f32, 3> premultiplied{};
                    for (usize sample = 0; sample < offsets.size(); ++sample) {
                        const f32 sample_alpha = static_cast<f32>(pixels[offsets[sample] + 3]) / 255.0f;
                        alpha += weights[sample] * sample_alpha;
                        for (usize channel = 0; channel < premultiplied.size(); ++channel) {
                            premultiplied[channel] += weights[sample] * sample_alpha *
                                                      static_cast<f32>(pixels[offsets[sample] + channel]);
                        }
                    }

                    const usize dst = (static_cast<usize>(y + padding) * params.width + x + padding) * 4;
                    for (usize channel = 0; channel < premultiplied.size(); ++channel) {
                        result.pixels[dst + channel] = alpha > 0.0f
                                                          ? static_cast<u8>(std::clamp(premultiplied[channel] / alpha,
                                                                                       0.0f, 255.0f) +
                                                                            0.5f)
                                                          : 0;
                    }
                    result.pixels[dst + 3] = static_cast<u8>(std::clamp(alpha * 255.0f, 0.0f, 255.0f) + 0.5f);
                }
            }
            stbi_image_free(pixels);

            hb_glyph_extents_t extents{};
            const f32 scale = params.pixel_size / static_cast<f32>(std::max(font.units_per_em(), 1u));
            if (hb_font_get_glyph_extents(raster_font, glyph_id, &extents)) {
                result.bearing_x = static_cast<f32>(extents.x_bearing) * scale - static_cast<f32>(padding);
                result.bearing_top = static_cast<f32>(extents.y_bearing) * scale + static_cast<f32>(padding);
            } else {
                result.bearing_x = -static_cast<f32>(padding);
                result.bearing_top = params.pixel_size + static_cast<f32>(padding);
            }
            hb_font_destroy(raster_font);
            return result;
        }

TextExpected<RasterizedGlyph> rasterize_layered_glyph(const Font &font, u32 glyph_id,
                                                                                    const ColorRasterParams &params) {
            hb_face_t *face = font.face_handle();

            unsigned int layers_probe = 0;
            const unsigned int total_layers =
                hb_ot_color_glyph_get_layers(face, static_cast<hb_codepoint_t>(glyph_id), 0, &layers_probe, nullptr);
            if (total_layers == 0) {
                return text_error(TextErrorCode::RasterizationFailed, "Glyph has no COLR layers.");
            }
            vector<hb_ot_color_layer_t> layers(total_layers);
            unsigned int layers_written = total_layers;
            hb_ot_color_glyph_get_layers(face, static_cast<hb_codepoint_t>(glyph_id), 0, &layers_written, layers.data());
            layers.resize(layers_written);

            unsigned int palette_probe = 0;
            const unsigned int total_colors = hb_ot_color_palette_get_colors(face, 0, 0, &palette_probe, nullptr);
            vector<hb_color_t> palette;
            if (total_colors > 0) {
                palette.resize(total_colors);
                unsigned int palette_written = total_colors;
                hb_ot_color_palette_get_colors(face, 0, 0, &palette_written, palette.data());
                palette.resize(palette_written);
            }

            RasterizedGlyph result;
            result.width = params.width;
            result.height = params.height;
            result.channel_count = 4;
            result.pixels.assign(static_cast<usize>(params.width) * params.height * 4, 0);

            struct ResolvedLayer {
                GlyphOutline outline;
                u8 r = 255;
                u8 g = 255;
                u8 b = 255;
                u8 a = 255;
            };
            vector<ResolvedLayer> resolved_layers;
            resolved_layers.reserve(layers.size());

            Text::GlyphBounds union_bounds{};
            for (const hb_ot_color_layer_t &layer : layers) {
                auto outline = glyph_outline(font, layer.glyph);
                if (!outline) {
                    continue;
                }
                const Text::GlyphBounds bounds = glyph_bounds(*outline);
                if (bounds.empty) {
                    continue;
                }

                ResolvedLayer resolved{.outline = std::move(*outline)};
                if (layer.color_index != 0xFFFFu && layer.color_index < palette.size()) {
                    const hb_color_t color = palette[layer.color_index];
                    resolved.r = hb_color_get_red(color);
                    resolved.g = hb_color_get_green(color);
                    resolved.b = hb_color_get_blue(color);
                    resolved.a = hb_color_get_alpha(color);
                }
                if (union_bounds.empty) {
                    union_bounds = bounds;
                } else {
                    union_bounds.left = std::min(union_bounds.left, bounds.left);
                    union_bounds.bottom = std::min(union_bounds.bottom, bounds.bottom);
                    union_bounds.right = std::max(union_bounds.right, bounds.right);
                    union_bounds.top = std::max(union_bounds.top, bounds.top);
                }
                resolved_layers.push_back(std::move(resolved));
            }
            if (resolved_layers.empty() || union_bounds.empty) {
                return text_error(TextErrorCode::RasterizationFailed,
                                  "COLR glyph contains no rasterizable outline layers.");
            }

            const u32 units_per_em = font.units_per_em() > 0 ? font.units_per_em() : 1000;
            const f32 scale = params.pixel_size / static_cast<f32>(units_per_em);
            const f32 padding = std::max(params.padding_px, 0.0f);
            const RasterParams layer_raster_params{
                .width = params.width,
                .height = params.height,
                .scale = scale,
                .pixel_range = 1.0f,
                .padding_px = padding,
                .translation = RasterTranslation{
                    .x = padding / scale - union_bounds.left,
                    .y = padding / scale - union_bounds.bottom,
                },
            };

            // hb_ot_color_glyph_get_layers already returns layers back-to-front (paint order), so
            // a plain forward "source over destination" composite reproduces the glyph correctly.
            bool have_bearing = false;
            for (const ResolvedLayer &layer : resolved_layers) {
                // A color index of 0xFFFF means the caller's foreground color. The reusable color
                // atlas has no per-instance foreground channel, so its neutral cached value is
                // white; fully colored emoji layers retain their palette values unchanged.
                auto mask = rasterize_glyph(layer.outline, RasterFormat::SDF, layer_raster_params);
                if (!mask) {
                    continue;
                }
                if (!have_bearing) {
                    result.bearing_x = mask->bearing_x;
                    result.bearing_top = mask->bearing_top;
                    have_bearing = true;
                }

                for (usize i = 0; i < mask->pixels.size(); ++i) {
                    // The SDF byte is already ~0.5-centered on the true edge (see Raster.cppm), so
                    // using it directly as a soft coverage value gives this layer antialiased edges
                    // for free instead of a hard-thresholded fill.
                    const f32 coverage = static_cast<f32>(mask->pixels[i]) / 255.0f;
                    const f32 layer_alpha = coverage * (static_cast<f32>(layer.a) / 255.0f);
                    if (layer_alpha <= 0.0f) {
                        continue;
                    }
                    const usize dst = i * 4;
                    const f32 dst_alpha = static_cast<f32>(result.pixels[dst + 3]) / 255.0f;
                    const f32 out_alpha = layer_alpha + dst_alpha * (1.0f - layer_alpha);
                    if (out_alpha <= 0.0f) {
                        continue;
                    }
                    auto blend_channel = [&](u8 src_channel, u8 dst_channel) noexcept -> u8 {
                        const f32 blended = (static_cast<f32>(src_channel) * layer_alpha +
                                             static_cast<f32>(dst_channel) * dst_alpha * (1.0f - layer_alpha)) /
                                            out_alpha;
                        return static_cast<u8>(std::clamp(blended, 0.0f, 255.0f));
                    };
                    result.pixels[dst + 0] = blend_channel(layer.r, result.pixels[dst + 0]);
                    result.pixels[dst + 1] = blend_channel(layer.g, result.pixels[dst + 1]);
                    result.pixels[dst + 2] = blend_channel(layer.b, result.pixels[dst + 2]);
                    result.pixels[dst + 3] = static_cast<u8>(std::clamp(out_alpha * 255.0f, 0.0f, 255.0f));
                }
            }

            return result;
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

TextExpected<RasterizedGlyph> rasterize_color_glyph(const Font &font, u32 glyph_id,
                                                                             const ColorRasterParams &params) {
        if (!font) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a color glyph from an invalid font.");
        }
        if (params.width == 0 || params.height == 0) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a color glyph into a zero-sized raster.");
        }

        // Prefer scalable COLR layers. If none exist, the bitmap path selects the closest strike
        // on a private sub-font at the requested ppem (and reports a useful error if absent).
        if (detect_color_format(font, glyph_id) == ColorGlyphFormat::Layered) {
            return Detail::rasterize_layered_glyph(font, glyph_id, params);
        }
        return Detail::rasterize_bitmap_glyph(font, glyph_id, params);
    }

} // namespace SFT::Text
