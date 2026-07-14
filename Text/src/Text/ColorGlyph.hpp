#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <hb.h>
#include <hb-ot.h>
// stb_image is a single-header library — exactly one translation unit in the whole program must
// define STB_IMAGE_IMPLEMENTATION before including it to get the actual decoder definitions, and
// this is that one TU (nothing else in Text uses stb_image).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"
#include "Outline.hpp"
#include "Raster.hpp"

using std::vector;

namespace SFT::Text {

    // Which color-glyph table (if any) a glyph is stored in. A font can carry both — `Bitmap`
    // (CBDT or Apple `sbix`, fixed-size PNG strikes) and `Layered` (COLR/CPAL, vector layers with
    // per-layer palette colors) are checked independently; a glyph with neither is an ordinary
    // monochrome outline glyph (Text::glyph_outline / the SDF/MSDF path).
    enum class ColorGlyphFormat {
        None,
        Bitmap,
        Layered,
    };

    [[nodiscard]] inline ColorGlyphFormat detect_color_format(const Font &font, u32 glyph_id) {
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

    struct ColorRasterParams {
        u32 width = 0;
        u32 height = 0;
        // Desired em size in pixels — selects the nearest embedded PNG strike (CBDT/sbix ship
        // several fixed sizes; HarfBuzz picks the closest to this) and scales COLR layer outlines.
        f32 pixel_size = 32.0f;
    };

    namespace Detail {

        [[nodiscard]] inline TextExpected<RasterizedGlyph> rasterize_bitmap_glyph(const Font &font, u32 glyph_id,
                                                                                   const ColorRasterParams &params) {
            const auto ppem = static_cast<unsigned int>(params.pixel_size + 0.5f);
            hb_font_set_ppem(font.handle(), ppem, ppem);

            hb_blob_t *blob = hb_ot_color_glyph_reference_png(font.handle(), static_cast<hb_codepoint_t>(glyph_id));
            if (blob == nullptr || blob == hb_blob_get_empty()) {
                if (blob != nullptr) {
                    hb_blob_destroy(blob);
                }
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
                return text_error(TextErrorCode::RasterizationFailed, "Failed to decode glyph PNG bitmap.");
            }

            RasterizedGlyph result;
            result.width = params.width;
            result.height = params.height;
            result.channel_count = 4;
            result.pixels.resize(static_cast<usize>(params.width) * params.height * 4);

            // Nearest-neighbor resample into the target cell — embedded strikes are typically
            // already close to the requested size, this just guarantees an exact-size cell.
            const u32 safe_width = params.width > 0 ? params.width : 1;
            const u32 safe_height = params.height > 0 ? params.height : 1;
            for (u32 y = 0; y < params.height; ++y) {
                const int src_y = std::min(decoded_height - 1, static_cast<int>(static_cast<u64>(y) * static_cast<u32>(decoded_height) / safe_height));
                for (u32 x = 0; x < params.width; ++x) {
                    const int src_x = std::min(decoded_width - 1, static_cast<int>(static_cast<u64>(x) * static_cast<u32>(decoded_width) / safe_width));
                    const usize src_offset = (static_cast<usize>(src_y) * static_cast<usize>(decoded_width) + static_cast<usize>(src_x)) * 4;
                    const usize dst_offset = (static_cast<usize>(y) * params.width + x) * 4;
                    result.pixels[dst_offset + 0] = pixels[src_offset + 0];
                    result.pixels[dst_offset + 1] = pixels[src_offset + 1];
                    result.pixels[dst_offset + 2] = pixels[src_offset + 2];
                    result.pixels[dst_offset + 3] = pixels[src_offset + 3];
                }
            }
            stbi_image_free(pixels);
            return result;
        }

        [[nodiscard]] inline TextExpected<RasterizedGlyph> rasterize_layered_glyph(const Font &font, u32 glyph_id,
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

            const u32 units_per_em = font.units_per_em() > 0 ? font.units_per_em() : 1000;
            const RasterParams layer_raster_params{
                .width = params.width,
                .height = params.height,
                .scale = params.pixel_size / static_cast<f32>(units_per_em),
                .pixel_range = 4.0f,
                .padding_px = 2.0f,
            };

            // hb_ot_color_glyph_get_layers already returns layers back-to-front (paint order), so
            // a plain forward "source over destination" composite reproduces the glyph correctly.
            for (const hb_ot_color_layer_t &layer : layers) {
                auto outline = glyph_outline(font, layer.glyph);
                if (!outline) {
                    continue;
                }
                // A color index of 0xFFFF means "use the caller's foreground/text color" rather
                // than a palette entry — leave it white so the shader's color tint (GlyphInstance's
                // straight-through-sampled color) supplies it.
                u8 r = 255;
                u8 g = 255;
                u8 b = 255;
                u8 a = 255;
                if (layer.color_index != 0xFFFFu && layer.color_index < palette.size()) {
                    const hb_color_t color = palette[layer.color_index];
                    r = hb_color_get_red(color);
                    g = hb_color_get_green(color);
                    b = hb_color_get_blue(color);
                    a = hb_color_get_alpha(color);
                }

                auto mask = rasterize_glyph(*outline, RasterFormat::SDF, layer_raster_params);
                if (!mask) {
                    continue;
                }

                for (usize i = 0; i < mask->pixels.size(); ++i) {
                    // The SDF byte is already ~0.5-centered on the true edge (see Raster.cppm), so
                    // using it directly as a soft coverage value gives this layer antialiased edges
                    // for free instead of a hard-thresholded fill.
                    const f32 coverage = static_cast<f32>(mask->pixels[i]) / 255.0f;
                    const f32 layer_alpha = coverage * (static_cast<f32>(a) / 255.0f);
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
                    result.pixels[dst + 0] = blend_channel(r, result.pixels[dst + 0]);
                    result.pixels[dst + 1] = blend_channel(g, result.pixels[dst + 1]);
                    result.pixels[dst + 2] = blend_channel(b, result.pixels[dst + 2]);
                    result.pixels[dst + 3] = static_cast<u8>(std::clamp(out_alpha * 255.0f, 0.0f, 255.0f));
                }
            }

            return result;
        }

    } // namespace Detail

    // Rasterizes a color glyph (bitmap or layered — see detect_color_format()) into a flat RGBA8
    // cell, ready for Renderer::TextAtlas's Color sub-atlas. Fails if the glyph has neither color
    // table — callers should check detect_color_format() first (or fall back to
    // Text::rasterize_glyph for an ordinary monochrome outline glyph).
    [[nodiscard]] inline TextExpected<RasterizedGlyph> rasterize_color_glyph(const Font &font, u32 glyph_id,
                                                                             const ColorRasterParams &params) {
        if (!font) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a color glyph from an invalid font.");
        }
        if (params.width == 0 || params.height == 0) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot rasterize a color glyph into a zero-sized raster.");
        }

        switch (detect_color_format(font, glyph_id)) {
            case ColorGlyphFormat::Bitmap: return Detail::rasterize_bitmap_glyph(font, glyph_id, params);
            case ColorGlyphFormat::Layered: return Detail::rasterize_layered_glyph(font, glyph_id, params);
            case ColorGlyphFormat::None: break;
        }
        return text_error(TextErrorCode::RasterizationFailed, "Glyph has no color format (not COLR/CPAL or PNG).");
    }

} // namespace SFT::Text
