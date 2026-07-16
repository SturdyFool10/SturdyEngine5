#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <Async/ParIter.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <glm/vec2.hpp>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/TextAtlas.hpp>
#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] u32 bytes_per_texel(RHI::Format format) noexcept {
            switch (format) {
                case RHI::Format::R8Unorm: return 1;
                case RHI::Format::RGBA8Unorm: return 4;
                default: return 0;
            }
        }

        constexpr f32 color_padding_px = 2.0f;

        struct RasterPlan {
            u32 width = 1;
            u32 height = 1;
            u32 reference_ppem = 1;
        };

        [[nodiscard]] constexpr usize format_index(Text::RasterFormat format) noexcept {
            switch (format) {
                case Text::RasterFormat::SDF: return 0;
                case Text::RasterFormat::MSDF: return 1;
                case Text::RasterFormat::Color: return 2;
            }
            return 0;
        }

        [[nodiscard]] constexpr const char *atlas_texture_label(Text::RasterFormat format) noexcept {
            switch (format) {
                case Text::RasterFormat::SDF: return "text SDF atlas image";
                case Text::RasterFormat::MSDF: return "text MSDF atlas image";
                case Text::RasterFormat::Color: return "text color atlas image";
            }
            return "text atlas image";
        }

        [[nodiscard]] constexpr const char *atlas_view_label(Text::RasterFormat format) noexcept {
            switch (format) {
                case Text::RasterFormat::SDF: return "text SDF atlas image view";
                case Text::RasterFormat::MSDF: return "text MSDF atlas image view";
                case Text::RasterFormat::Color: return "text color atlas image view";
            }
            return "text atlas image view";
        }

        // Pick an actual rasterization size from this glyph's own ink bounds. The requested ppem is
        // preserved unless an unusually large glyph cannot fit even an entire atlas tile, in which
        // case distance fields let us down-rasterize it and scale it back up without clipping.
        [[nodiscard]] RasterPlan raster_plan_for(const GlyphRequest &request, f32 distance_padding_px,
                                                 u32 max_raster_extent) noexcept {
            const f32 padding = request.format == Text::RasterFormat::Color ? color_padding_px : distance_padding_px;
            const f32 units_per_em = static_cast<f32>(std::max(request.units_per_em, 1u));
            f32 width_units = 0.0f;
            f32 height_units = 0.0f;

            if (request.format == Text::RasterFormat::Color) {
                hb_glyph_extents_t extents{};
                if (request.font != nullptr &&
                    hb_font_get_glyph_extents(request.font->handle(), request.glyph_id, &extents)) {
                    width_units = std::abs(static_cast<f32>(extents.width));
                    height_units = std::abs(static_cast<f32>(extents.height));
                }
                if (!(width_units > 0.0f) || !(height_units > 0.0f)) {
                    width_units = units_per_em;
                    height_units = units_per_em;
                }
            } else if (request.outline != nullptr) {
                const Text::GlyphBounds bounds = Text::glyph_bounds(*request.outline);
                if (!bounds.empty) {
                    width_units = std::max(bounds.width(), 0.0f);
                    height_units = std::max(bounds.height(), 0.0f);
                }
            }

            const double rounded_ppem = std::round(static_cast<double>(std::max(request.pixel_size, 1.0f)));
            const u32 requested_ppem = rounded_ppem >= static_cast<double>(std::numeric_limits<u32>::max())
                                           ? std::numeric_limits<u32>::max()
                                           : std::max(1u, static_cast<u32>(rounded_ppem));
            u32 reference_ppem = requested_ppem;
            const f32 usable = std::max(1.0f, static_cast<f32>(max_raster_extent) - 2.0f * padding);
            if (width_units > 0.0f || height_units > 0.0f) {
                const f32 max_x = width_units > 0.0f ? usable * units_per_em / width_units
                                                     : std::numeric_limits<f32>::max();
                const f32 max_y = height_units > 0.0f ? usable * units_per_em / height_units
                                                      : std::numeric_limits<f32>::max();
                const u32 fitting_ppem = std::max(1u, static_cast<u32>(std::floor(std::min(max_x, max_y))));
                reference_ppem = std::min(reference_ppem, fitting_ppem);
            }

            const f32 scale = static_cast<f32>(reference_ppem) / units_per_em;
            const u32 width = width_units > 0.0f
                                  ? static_cast<u32>(std::ceil(width_units * scale + 2.0f * padding))
                                  : 1u;
            const u32 height = height_units > 0.0f
                                   ? static_cast<u32>(std::ceil(height_units * scale + 2.0f * padding))
                                   : 1u;
            return RasterPlan{
                .width = std::clamp(width, 1u, max_raster_extent),
                .height = std::clamp(height, 1u, max_raster_extent),
                .reference_ppem = reference_ppem,
            };
        }

        // The RHI only has raw texture-upload support for R8Unorm and RGBA8Unorm today (see
        // Renderer/RendererTextures.cpp) — msdfgen's MSDF output is 3-channel, so it's expanded
        // into RGBA here with a constant alpha (unused by the text shader, which reads MSDF
        // distances from RGB and takes their median).
        void expand_rgb_to_rgba(span<const u8> rgb, vector<std::byte> &out, usize write_offset) {
            const usize pixel_count = rgb.size() / 3;
            for (usize i = 0; i < pixel_count; ++i) {
                const usize src = i * 3;
                const usize dst = write_offset + i * 4;
                out[dst + 0] = static_cast<std::byte>(rgb[src + 0]);
                out[dst + 1] = static_cast<std::byte>(rgb[src + 1]);
                out[dst + 2] = static_cast<std::byte>(rgb[src + 2]);
                out[dst + 3] = std::byte{0xFF};
            }
        }

    } // namespace

    usize GlyphKeyHash::operator()(const GlyphKey &key) const noexcept {
        u64 hashed = key.font_id;
        hashed ^= static_cast<u64>(key.glyph_id) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
        hashed ^= static_cast<u64>(key.reference_ppem) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
        hashed ^= static_cast<u64>(key.format) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
        return static_cast<usize>(hashed);
    }

    GlyphSlot TextAtlas::slot_from_rect(Text::RasterFormat format, RectLocation rect) const noexcept {
        const f32 raster_width = static_cast<f32>(rect.raster_width);
        const f32 raster_height = static_cast<f32>(rect.raster_height);
        const FormatAtlas &atlas = format_atlas(format);
        const f32 tile_size = rect.tile_index < atlas.tiles.size()
                                  ? static_cast<f32>(atlas.tiles[rect.tile_index].size)
                                  : static_cast<f32>(max_tile_size_);
        // Rectangles are packed edge-to-edge. The raster's own padding is its filtering guard, and
        // a half-texel UV inset ensures bilinear filtering never crosses into its neighbor.
        const f32 half_texel = 0.5f / tile_size;
        const f32 u0 = static_cast<f32>(rect.x) / tile_size + half_texel;
        const f32 v0 = static_cast<f32>(rect.y) / tile_size + half_texel;
        const f32 u1 = static_cast<f32>(rect.x + rect.raster_width) / tile_size - half_texel;
        const f32 v1 = static_cast<f32>(rect.y + rect.raster_height) / tile_size - half_texel;
        return GlyphSlot{
            .tile_index = rect.tile_index,
            .uv_min = glm::vec2{u0, v0},
            .uv_max = glm::vec2{u1, v1},
            .raster_size_px = glm::vec2{raster_width, raster_height},
            .reference_ppem = rect.reference_ppem,
            .format = format,
            .bearing_x = rect.bearing_x,
            .bearing_top = rect.bearing_top,
        };
    }

    Core::RendererExpected<TextAtlas> TextAtlas::create(RHI::RhiDevice &device, const Config &config) {
        TextAtlas atlas;
        atlas.config_ = config;
        atlas.max_tile_size_ = clamp_tile_size(config.maximum_image_size, device.limits());
        atlas.initial_tile_size_ = std::min(config.initial_image_size, atlas.max_tile_size_);
        if (atlas.initial_tile_size_ == 0 || atlas.max_tile_size_ == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text atlas image sizes must be non-zero."});
        }
        if (!std::isfinite(config.pixel_range) || !std::isfinite(config.padding_px) ||
            config.pixel_range <= 0.0f || config.padding_px < config.pixel_range ||
            static_cast<f32>(atlas.initial_tile_size_) < 2.0f * std::max(config.padding_px, color_padding_px) + 1.0f) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Text atlas distance range/padding must be finite, padding must cover the distance range, "
                "and the initial image must leave at least one texel inside its padding."});
        }
        return atlas;
    }

    Core::RendererExpected<TextAtlas::Tile> TextAtlas::create_tile(RHI::RhiDevice &device,
                                                                   Text::RasterFormat format, u32 size) {
        if (size == 0 || size > max_tile_size_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text atlas growth requested an invalid image size."});
        }
        auto texture = device.create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = texture_format(format),
            .extent = RHI::Extent3D{.width = size, .height = size, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            // TransferSrc is required when a populated image later becomes the source of a
            // grow-only replacement copy. Every atlas image may eventually occupy that role.
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = atlas_texture_label(format),
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create text atlas image"));
        }
        auto view = device.create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = atlas_view_label(format),
        });
        if (!view) {
            device.destroy_texture(*texture);
            return unexpected(graphics_error_from_rhi(view.error(), "create text atlas image view"));
        }
        Tile tile{
            .texture = *texture,
            .view = *view,
            .current_layout = RHI::TextureLayout::Undefined,
            .size = size,
        };
        tile.free_rects.push_back(AtlasRect{.width = size, .height = size});
        return tile;
    }

    Core::RendererResult TextAtlas::append_tile(RHI::RhiDevice &device, Text::RasterFormat format, u32 size) {
        FormatAtlas &atlas = format_atlas(format);
        if (!atlas.tiles.empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Text atlas format already owns its grow-only image.");
        }
        auto tile = create_tile(device, format, size);
        if (!tile) {
            return unexpected(tile.error());
        }
        atlas.tiles.push_back(std::move(*tile));
        return {};
    }

    Core::RendererResult TextAtlas::grow_tile(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                               Text::RasterFormat format, u32 new_size,
                                               TextAtlasRetiredResources &out_retired_resources) {
        FormatAtlas &atlas = format_atlas(format);
        if (atlas.tiles.size() != 1 || new_size <= atlas.tiles.front().size || new_size > max_tile_size_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Text atlas requested an invalid grow-only image replacement.");
        }

        auto replacement_result = create_tile(device, format, new_size);
        if (!replacement_result) {
            return unexpected(replacement_result.error());
        }
        Tile replacement = std::move(*replacement_result);
        Tile previous = std::move(atlas.tiles.front());

        // Preserve the old allocator partition in the top-left square and add the newly exposed
        // area as two disjoint strips. Existing glyph coordinates therefore remain unchanged.
        replacement.free_rects = previous.free_rects;
        const u32 added = new_size - previous.size;
        replacement.free_rects.push_back(AtlasRect{
            .x = previous.size,
            .y = 0,
            .width = added,
            .height = new_size,
        });
        replacement.free_rects.push_back(AtlasRect{
            .x = 0,
            .y = previous.size,
            .width = previous.size,
            .height = added,
        });

        if (previous.current_layout != RHI::TextureLayout::Undefined) {
            const array barriers{
                RHI::TextureBarrier{
                    .texture = previous.texture,
                    .src_stage = RHI::PipelineStage::FragmentShader,
                    .src_access = RHI::AccessFlags::ShaderRead,
                    .dst_stage = RHI::PipelineStage::Transfer,
                    .dst_access = RHI::AccessFlags::TransferRead,
                    .old_layout = previous.current_layout,
                    .new_layout = RHI::TextureLayout::TransferSrc,
                },
                RHI::TextureBarrier{
                    .texture = replacement.texture,
                    .src_stage = RHI::PipelineStage::None,
                    .src_access = RHI::AccessFlags::None,
                    .dst_stage = RHI::PipelineStage::Transfer,
                    .dst_access = RHI::AccessFlags::TransferWrite,
                    .old_layout = RHI::TextureLayout::Undefined,
                    .new_layout = RHI::TextureLayout::TransferDst,
                },
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{barriers.data(), barriers.size()});
            encoder.copy_texture_to_texture(previous.texture, replacement.texture, RHI::TextureCopy{
                .extent = RHI::Extent3D{.width = previous.size, .height = previous.size, .depth_or_layers = 1},
            });
            const RHI::TextureBarrier to_sampled{
                .texture = replacement.texture,
                .src_stage = RHI::PipelineStage::Transfer,
                .src_access = RHI::AccessFlags::TransferWrite,
                .dst_stage = RHI::PipelineStage::FragmentShader,
                .dst_access = RHI::AccessFlags::ShaderRead,
                .old_layout = RHI::TextureLayout::TransferDst,
                .new_layout = RHI::TextureLayout::ShaderReadOnly,
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});
            replacement.current_layout = RHI::TextureLayout::ShaderReadOnly;
        }

        atlas.tiles.front() = std::move(replacement);
        out_retired_resources.texture_views.push_back(previous.view);
        out_retired_resources.textures.push_back(previous.texture);
        return {};
    }

    Core::RendererExpected<TextAtlas::RectLocation>
    TextAtlas::allocate_rect(RHI::RhiDevice &device, RHI::CommandEncoder &encoder, Text::RasterFormat format,
                             u32 width, u32 height, span<const GlyphKey> protected_keys,
                             TextAtlasRetiredResources &out_retired_resources) {
        if (width == 0 || height == 0 || width > max_tile_size_ || height > max_tile_size_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text atlas glyph rectangle does not fit in a tile."});
        }
        FormatAtlas &atlas = format_atlas(format);

        for (;;) {
            usize best_tile = 0;
            usize best_free = 0;
            u64 best_waste = std::numeric_limits<u64>::max();
            u32 best_short_side = std::numeric_limits<u32>::max();
            bool found = false;
            for (usize tile_index = 0; tile_index < atlas.tiles.size(); ++tile_index) {
                const vector<AtlasRect> &free_rects = atlas.tiles[tile_index].free_rects;
                for (usize free_index = 0; free_index < free_rects.size(); ++free_index) {
                    const AtlasRect &free = free_rects[free_index];
                    if (width > free.width || height > free.height) {
                        continue;
                    }
                    const u64 waste = static_cast<u64>(free.width) * free.height - static_cast<u64>(width) * height;
                    const u32 short_side = std::min(free.width - width, free.height - height);
                    if (!found || waste < best_waste || (waste == best_waste && short_side < best_short_side)) {
                        found = true;
                        best_tile = tile_index;
                        best_free = free_index;
                        best_waste = waste;
                        best_short_side = short_side;
                    }
                }
            }

            if (found) {
                Tile &tile = atlas.tiles[best_tile];
                const AtlasRect free = tile.free_rects[best_free];
                tile.free_rects[best_free] = tile.free_rects.back();
                tile.free_rects.pop_back();

                const u32 remaining_width = free.width - width;
                const u32 remaining_height = free.height - height;
                auto keep = [&](AtlasRect rect) {
                    if (rect.width > 0 && rect.height > 0) {
                        tile.free_rects.push_back(rect);
                    }
                };
                // Split along the axis with more room left. The two resulting free rectangles are
                // disjoint, and sorting batch misses by size makes this behave like dense shelves
                // without losing the ability to reuse irregular holes after eviction.
                if (remaining_width > remaining_height) {
                    keep(AtlasRect{.x = free.x + width, .y = free.y,
                                   .width = remaining_width, .height = free.height});
                    keep(AtlasRect{.x = free.x, .y = free.y + height,
                                   .width = width, .height = remaining_height});
                } else {
                    keep(AtlasRect{.x = free.x + width, .y = free.y,
                                   .width = remaining_width, .height = height});
                    keep(AtlasRect{.x = free.x, .y = free.y + height,
                                   .width = free.width, .height = remaining_height});
                }

                return RectLocation{
                    .tile_index = static_cast<u32>(best_tile),
                    .x = free.x,
                    .y = free.y,
                    .raster_width = width,
                    .raster_height = height,
                };
            }

            if (atlas.tiles.empty()) {
                const u32 required_size = std::max(width, height);
                u32 next_size = initial_tile_size_;
                while (next_size < required_size && next_size < max_tile_size_) {
                    next_size = next_size > max_tile_size_ / 2u ? max_tile_size_ : next_size * 2u;
                }
                if (Core::RendererResult added = append_tile(device, format, next_size); !added) {
                    return unexpected(added.error());
                }
                continue;
            }

            if (atlas.tiles.front().size < max_tile_size_) {
                const u32 old_size = atlas.tiles.front().size;
                const u32 new_size = old_size > max_tile_size_ / 2u ? max_tile_size_ : old_size * 2u;
                if (Core::RendererResult grown = grow_tile(device, encoder, format, new_size, out_retired_resources);
                    !grown) {
                    return unexpected(grown.error());
                }
                continue;
            }

            std::optional<GlyphKey> evicted = format_lru(format).evict_one_if([&](const GlyphKey &candidate) {
                return std::ranges::find(protected_keys, candidate) == protected_keys.end();
            });
            if (!evicted) {
                return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              "Text atlas is full and every remaining glyph is pinned by this batch."});
            }
            auto it = resident_.find(*evicted);
            if (it == resident_.end()) {
                continue;
            }
            const RectLocation reclaimed = it->second;
            resident_.erase(it);
            release_rect(format, reclaimed);
        }
    }

    void TextAtlas::release_rect(Text::RasterFormat format, RectLocation rect) noexcept {
        FormatAtlas &atlas = format_atlas(format);
        if (rect.tile_index >= atlas.tiles.size() || rect.raster_width == 0 || rect.raster_height == 0) {
            return;
        }
        vector<AtlasRect> &free_rects = atlas.tiles[rect.tile_index].free_rects;
        free_rects.push_back(AtlasRect{
            .x = rect.x,
            .y = rect.y,
            .width = rect.raster_width,
            .height = rect.raster_height,
        });

        // Undo compatible guillotine splits. Repeat because one merge can make a larger neighbor
        // eligible on the next pass; releasing every glyph therefore restores one full-tile rect.
        bool merged = true;
        while (merged) {
            merged = false;
            for (usize i = 0; i < free_rects.size() && !merged; ++i) {
                for (usize j = i + 1; j < free_rects.size(); ++j) {
                    AtlasRect combined{};
                    const AtlasRect &a = free_rects[i];
                    const AtlasRect &b = free_rects[j];
                    if (a.y == b.y && a.height == b.height &&
                        (a.x + a.width == b.x || b.x + b.width == a.x)) {
                        combined = AtlasRect{
                            .x = std::min(a.x, b.x),
                            .y = a.y,
                            .width = a.width + b.width,
                            .height = a.height,
                        };
                    } else if (a.x == b.x && a.width == b.width &&
                               (a.y + a.height == b.y || b.y + b.height == a.y)) {
                        combined = AtlasRect{
                            .x = a.x,
                            .y = std::min(a.y, b.y),
                            .width = a.width,
                            .height = a.height + b.height,
                        };
                    } else {
                        continue;
                    }
                    free_rects[i] = combined;
                    free_rects[j] = free_rects.back();
                    free_rects.pop_back();
                    merged = true;
                    break;
                }
            }
        }
    }

    Core::RendererResult TextAtlas::ensure_resident(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                     span<const GlyphRequest> requests, vector<GlyphSlot> &out_slots,
                                                     vector<RHI::BufferHandle> &out_transient_buffers,
                                                     TextAtlasRetiredResources &out_retired_resources) {
        out_slots.assign(requests.size(), GlyphSlot{});
        vector<RasterPlan> raster_plans(requests.size());
        vector<GlyphKey> request_keys(requests.size());
        vector<PendingUpload> misses;

        // Validate the whole batch before mutating residency, so one malformed request cannot
        // leave earlier requests from the same call occupying half-initialized cache rectangles.
        for (const GlyphRequest &request : requests) {
            if (!std::isfinite(request.pixel_size) || !(request.pixel_size > 0.0f) || request.units_per_em == 0) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text atlas request has an invalid pixel size or units-per-em.");
            }
            if (request.format == Text::RasterFormat::Color) {
                if (request.font == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Text atlas color glyph cache miss with no font to rasterize.");
                }
            } else if (request.outline == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text atlas cache miss with no outline to rasterize.");
            }
        }

        // The call must keep every distinct requested glyph resident through the draw that follows
        // it. Compute exact padded rectangles once, and reject batches whose aggregate area cannot
        // possibly fit even if all unpinned entries are evicted.
        array<vector<GlyphKey>, 3> unique_batch_keys;
        array<u64, 3> unique_batch_area{};
        for (usize i = 0; i < requests.size(); ++i) {
            const GlyphRequest &request = requests[i];
            const RasterPlan raster_plan = raster_plan_for(request, config_.padding_px, max_tile_size_);
            const GlyphKey key{
                .font_id = request.font_id,
                .glyph_id = request.glyph_id,
                .reference_ppem = raster_plan.reference_ppem,
                .format = request.format,
            };
            raster_plans[i] = raster_plan;
            request_keys[i] = key;
            const usize index = format_index(request.format);
            vector<GlyphKey> &keys = unique_batch_keys[index];
            if (std::ranges::find(keys, key) == keys.end()) {
                keys.push_back(key);
                unique_batch_area[index] += static_cast<u64>(raster_plan.width) * raster_plan.height;
            }
        }
        for (usize i = 0; i < unique_batch_keys.size(); ++i) {
            const u64 capacity = static_cast<u64>(max_tile_size_) * max_tile_size_;
            if (unique_batch_area[i] > capacity) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "One text batch has more glyph raster area than its atlas format can retain.");
            }
        }

        auto rollback_misses = [&]() noexcept {
            for (const PendingUpload &miss : misses) {
                if (!miss.allocated) {
                    continue;
                }
                resident_.erase(miss.key);
                const Text::RasterFormat format = requests[miss.request_index].format;
                format_lru(format).erase(miss.key);
                release_rect(format, miss.rect);
            }
        };

        // Gather unique misses before allocating. Cached members of this batch are touched now and
        // all batch keys are passed to allocate_rect as pins, so allocation can never evict a slot
        // that the imminent draw still references.
        for (usize i = 0; i < requests.size(); ++i) {
            const GlyphRequest &request = requests[i];
            const RasterPlan &raster_plan = raster_plans[i];
            const GlyphKey &key = request_keys[i];

            auto it = resident_.find(key);
            if (it != resident_.end()) {
                format_lru(request.format).touch(key);
                out_slots[i] = slot_from_rect(request.format, it->second);
                continue;
            }
            const auto already_pending = std::ranges::find(misses, key, &PendingUpload::key);
            if (already_pending == misses.end()) {
                misses.push_back(PendingUpload{
                    .request_index = i,
                    .key = key,
                    .rect = RectLocation{
                        .raster_width = raster_plan.width,
                        .raster_height = raster_plan.height,
                        .reference_ppem = static_cast<f32>(raster_plan.reference_ppem),
                    },
                });
            }
        }

        if (misses.empty()) {
            return {};
        }

        // Largest-first placement keeps the guillotine allocator's residual strips useful and
        // avoids small punctuation carving awkward holes before tall or wide glyphs arrive.
        std::ranges::sort(misses, [](const PendingUpload &a, const PendingUpload &b) {
            if (a.rect.raster_height != b.rect.raster_height) {
                return a.rect.raster_height > b.rect.raster_height;
            }
            if (a.rect.raster_width != b.rect.raster_width) {
                return a.rect.raster_width > b.rect.raster_width;
            }
            return static_cast<u64>(a.rect.raster_width) * a.rect.raster_height >
                   static_cast<u64>(b.rect.raster_width) * b.rect.raster_height;
        });

        for (PendingUpload &miss : misses) {
            const Text::RasterFormat format = requests[miss.request_index].format;
            const span<const GlyphKey> protected_keys{unique_batch_keys[format_index(format)]};
            auto allocated = allocate_rect(device, encoder, format, miss.rect.raster_width, miss.rect.raster_height,
                                           protected_keys, out_retired_resources);
            if (!allocated) {
                rollback_misses();
                return unexpected(allocated.error());
            }
            allocated->reference_ppem = miss.rect.reference_ppem;
            miss.rect = *allocated;
            miss.allocated = true;
            resident_[miss.key] = miss.rect;
            format_lru(format).touch(miss.key);
        }

        if (Core::RendererResult uploaded =
                upload_misses(device, encoder, requests, misses, out_slots, out_transient_buffers);
            !uploaded) {
            rollback_misses();
            return uploaded;
        }

        // A request repeated in the same batch sees the provisional resident_ entry created by
        // its first occurrence. Resolve every output again after upload so duplicates receive the
        // final bearing instead of the provisional zero-bearing slot.
        for (usize i = 0; i < requests.size(); ++i) {
            const GlyphRequest &request = requests[i];
            const GlyphKey &key = request_keys[i];
            const auto resident = resident_.find(key);
            if (resident == resident_.end()) {
                rollback_misses();
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text atlas lost a glyph immediately after uploading it.");
            }
            out_slots[i] = slot_from_rect(request.format, resident->second);
        }
        return {};
    }

    Core::RendererResult TextAtlas::upload_misses(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                  span<const GlyphRequest> requests, const vector<PendingUpload> &misses,
                                                  vector<GlyphSlot> &out_slots, vector<RHI::BufferHandle> &out_transient_buffers) {
        // Rasterize every miss off the calling thread in parallel — this is the engine's job
        // system's (Async::Scheduler) first real consumer in the renderer.
        vector<Text::TextExpected<Text::RasterizedGlyph>> rasterized(misses.size());
        Async::par_iter(std::views::iota(usize{0}, misses.size())).for_each([&](usize i) {
            const GlyphRequest &request = requests[misses[i].request_index];
            const RectLocation &rect = misses[i].rect;
            if (request.format == Text::RasterFormat::Color) {
                const Text::ColorRasterParams params{
                    .width = rect.raster_width,
                    .height = rect.raster_height,
                    .pixel_size = rect.reference_ppem,
                    .padding_px = color_padding_px,
                };
                rasterized[i] = Text::rasterize_color_glyph(*request.font, request.glyph_id, params);
                return;
            }
            const f32 scale = rect.reference_ppem / static_cast<f32>(request.units_per_em);
            const Text::RasterParams params{
                .width = rect.raster_width,
                .height = rect.raster_height,
                .scale = scale,
                .pixel_range = config_.pixel_range,
                .padding_px = config_.padding_px,
                .translation = std::nullopt,
            };
            rasterized[i] = Text::rasterize_glyph(*request.outline, request.format, params);
        });

        for (usize i = 0; i < misses.size(); ++i) {
            if (!rasterized[i]) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Text atlas glyph rasterization failed: " + rasterized[i].error().message.cpp_string());
            }
            // Bearing is only known now that rasterization has actually run. Fold it into both the
            // resident_ entry (so a future cache hit sees it too) and this miss's own slot.
            RectLocation rect = misses[i].rect;
            rect.bearing_x = rasterized[i]->bearing_x;
            rect.bearing_top = rasterized[i]->bearing_top;
            resident_[misses[i].key] = rect;
            out_slots[misses[i].request_index] = slot_from_rect(requests[misses[i].request_index].format, rect);
        }

        // Batch every miss's pixels into one staging buffer, then one command buffer + submit +
        // wait — the throughput win the plan calls for over one round trip per glyph.
        usize total_bytes = 0;
        vector<usize> byte_offsets(misses.size());
        for (usize i = 0; i < misses.size(); ++i) {
            const Text::RasterFormat format = requests[misses[i].request_index].format;
            const usize texel_bytes = bytes_per_texel(texture_format(format));
            total_bytes = (total_bytes + 3u) & ~usize{3u};
            byte_offsets[i] = total_bytes;
            total_bytes += static_cast<usize>(misses[i].rect.raster_width) * misses[i].rect.raster_height * texel_bytes;
        }

        vector<std::byte> staging_bytes(total_bytes);
        for (usize i = 0; i < misses.size(); ++i) {
            const Text::RasterFormat format = requests[misses[i].request_index].format;
            const Text::RasterizedGlyph &glyph = *rasterized[i];
            if (format == Text::RasterFormat::MSDF) {
                expand_rgb_to_rgba(glyph.pixels, staging_bytes, byte_offsets[i]);
            } else {
                // SDF (1 channel) and Color (already RGBA8) both copy straight through.
                std::ranges::transform(glyph.pixels, staging_bytes.begin() + static_cast<isize>(byte_offsets[i]),
                                       [](u8 value) { return static_cast<std::byte>(value); });
            }
        }

        auto staging = device.create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(total_bytes),
            .usage = RHI::BufferUsage::TransferSrc,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "text atlas staging",
        });
        if (!staging) {
            return unexpected(graphics_error_from_rhi(staging.error(), "create text atlas staging buffer"));
        }
        if (auto written = device.write_buffer(*staging, 0, span<const std::byte>{staging_bytes}); !written) {
            device.destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(written.error(), "write text atlas staging buffer"));
        }

        // Transition every touched tile to TransferDst exactly once, even if it receives several
        // glyphs in this batch.
        vector<std::pair<Text::RasterFormat, u32>> touched_tiles;
        auto mark_touched = [&](Text::RasterFormat format, u32 tile_index) {
            for (const auto &[existing_format, existing_index] : touched_tiles) {
                if (existing_format == format && existing_index == tile_index) {
                    return;
                }
            }
            touched_tiles.emplace_back(format, tile_index);
        };
        for (const PendingUpload &miss : misses) {
            mark_touched(requests[miss.request_index].format, miss.rect.tile_index);
        }

        for (const auto &[format, tile_index] : touched_tiles) {
            Tile &tile = format_atlas(format).tiles[tile_index];
            const RHI::TextureBarrier to_transfer{
                .texture = tile.texture,
                .src_stage = tile.current_layout == RHI::TextureLayout::Undefined ? RHI::PipelineStage::None : RHI::PipelineStage::FragmentShader,
                .src_access = tile.current_layout == RHI::TextureLayout::Undefined ? RHI::AccessFlags::None : RHI::AccessFlags::ShaderRead,
                .dst_stage = RHI::PipelineStage::Transfer,
                .dst_access = RHI::AccessFlags::TransferWrite,
                .old_layout = tile.current_layout,
                .new_layout = RHI::TextureLayout::TransferDst,
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&to_transfer, 1});
            tile.current_layout = RHI::TextureLayout::TransferDst;
        }

        for (usize i = 0; i < misses.size(); ++i) {
            const Text::RasterFormat format = requests[misses[i].request_index].format;
            const Tile &tile = format_atlas(format).tiles[misses[i].rect.tile_index];
            const RHI::BufferTextureCopy copy{
                .buffer_offset = byte_offsets[i],
                .mip_level = 0,
                .base_array_layer = 0,
                .array_layer_count = 1,
                .texture_offset = RHI::Offset3D{
                    static_cast<i32>(misses[i].rect.x),
                    static_cast<i32>(misses[i].rect.y),
                    0,
                },
                .texture_extent = RHI::Extent3D{
                    .width = misses[i].rect.raster_width,
                    .height = misses[i].rect.raster_height,
                    .depth_or_layers = 1,
                },
            };
            encoder.copy_buffer_to_texture(*staging, tile.texture, copy);
        }

        for (const auto &[format, tile_index] : touched_tiles) {
            Tile &tile = format_atlas(format).tiles[tile_index];
            const RHI::TextureBarrier to_sampled{
                .texture = tile.texture,
                .src_stage = RHI::PipelineStage::Transfer,
                .src_access = RHI::AccessFlags::TransferWrite,
                .dst_stage = RHI::PipelineStage::FragmentShader,
                .dst_access = RHI::AccessFlags::ShaderRead,
                .old_layout = RHI::TextureLayout::TransferDst,
                .new_layout = RHI::TextureLayout::ShaderReadOnly,
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});
            tile.current_layout = RHI::TextureLayout::ShaderReadOnly;
        }

        // No submit/wait here: `encoder` is the caller's shared per-frame encoder, already
        // recording the rest of the frame's work — this upload rides along in that one queue
        // submission. The staging buffer must outlive the GPU copy above, which hasn't necessarily
        // run yet, so it's handed to the caller for frame-fence-gated cleanup instead of being
        // destroyed here.
        out_transient_buffers.push_back(*staging);
        return {};
    }

    RHI::TextureViewHandle TextAtlas::tile_view(Text::RasterFormat format, u32 tile_index) const noexcept {
        const FormatAtlas &atlas = format_atlas(format);
        if (tile_index >= atlas.tiles.size()) {
            return {};
        }
        return atlas.tiles[tile_index].view;
    }

    u32 TextAtlas::tile_count(Text::RasterFormat format) const noexcept {
        return static_cast<u32>(format_atlas(format).tiles.size());
    }

    void TextAtlas::destroy(RHI::RhiDevice &device) noexcept {
        for (FormatAtlas *atlas : {&sdf_, &msdf_, &color_}) {
            for (Tile &tile : atlas->tiles) {
                if (tile.view) {
                    device.destroy_texture_view(tile.view);
                }
                if (tile.texture) {
                    device.destroy_texture(tile.texture);
                }
            }
            atlas->tiles.clear();
        }
        resident_.clear();
        sdf_lru_ = {};
        msdf_lru_ = {};
        color_lru_ = {};
    }

} // namespace SFT::Renderer
