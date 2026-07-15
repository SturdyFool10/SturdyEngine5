#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <Async/ParIter.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <glm/vec2.hpp>
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

        // How much of a (fixed-size) cache cell a glyph actually needs rasterized: enough to hold
        // the glyph at its requested display size plus the SDF/MSDF distance-field margin (color
        // glyphs fill their canvas directly with no such margin — see rasterize_color_glyph).
        // Rasterizing — and later sampling — only this sub-rect instead of always the full cell
        // keeps the on-screen quad's minification factor tied to how big the glyph is actually
        // drawn, rather than the cache's fixed storage granularity (a 16px on-screen glyph
        // sampling a fixed 64px cell is a 4x minification with no mipmapping — visibly broken).
        [[nodiscard]] u32 raster_size_for(Text::RasterFormat format, f32 pixel_size, f32 padding_px, u32 cell_size) noexcept {
            const f32 needed = format == Text::RasterFormat::Color ? pixel_size : pixel_size + 2.0f * padding_px;
            const u32 rounded = static_cast<u32>(std::ceil(needed));
            return std::clamp(rounded, 1u, cell_size);
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

    GlyphSlot TextAtlas::slot_from_cell(Text::RasterFormat format, CellLocation cell) const noexcept {
        // Only `raster_size` texels of this cell (top-left aligned) hold real rasterized content —
        // see upload_misses. Bounding the UV rect to that sub-region, rather than the full
        // (generally larger) fixed cell, keeps the displayed quad's minification factor tied to
        // how the glyph was actually rasterized instead of the cache's fixed storage granularity.
        const f32 raster_size = cell.raster_size > 0 ? static_cast<f32>(cell.raster_size) : static_cast<f32>(config_.cell_size);
        const f32 cell_size = static_cast<f32>(config_.cell_size);
        const f32 tile_size = static_cast<f32>(tile_size_);
        // Cells are packed edge-to-edge with no gutter between them, so a UV rect that reaches
        // all the way to the cell boundary lets bilinear filtering blend in the neighboring
        // cell's texel at the seam (visible as a faint ghost line along glyph edges). Insetting
        // by half a texel keeps every sample inside this cell's own texels.
        const f32 half_texel = 0.5f / tile_size;
        const f32 cell_u0 = static_cast<f32>(cell.cell_x) * cell_size / tile_size;
        const f32 cell_v0 = static_cast<f32>(cell.cell_y) * cell_size / tile_size;
        const f32 u0 = cell_u0 + half_texel;
        const f32 v0 = cell_v0 + half_texel;
        const f32 u1 = cell_u0 + raster_size / tile_size - half_texel;
        const f32 v1 = cell_v0 + raster_size / tile_size - half_texel;
        return GlyphSlot{
            .tile_index = cell.tile_index,
            .uv_min = glm::vec2{u0, v0},
            .uv_max = glm::vec2{u1, v1},
            .cell_size_px = raster_size,
            .format = format,
        };
    }

    Core::RendererExpected<TextAtlas> TextAtlas::create(RHI::RhiDevice &device, const Config &config) {
        TextAtlas atlas;
        atlas.config_ = config;
        atlas.tile_size_ = clamp_tile_size(config.desired_tile_size, device.limits());
        if (atlas.tile_size_ == 0 || config.cell_size == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text atlas tile/cell size must be non-zero."});
        }
        const u32 cells_per_row = atlas.tile_size_ / config.cell_size;
        if (cells_per_row == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text atlas cell size is larger than the device's max tile size."});
        }
        atlas.sdf_.cells_per_row = cells_per_row;
        atlas.msdf_.cells_per_row = cells_per_row;
        atlas.color_.cells_per_row = cells_per_row;
        return atlas;
    }

    Core::RendererExpected<TextAtlas::CellLocation> TextAtlas::allocate_cell(RHI::RhiDevice &device, Text::RasterFormat format) {
        FormatAtlas &atlas = format_atlas(format);
        const u32 cells_per_tile = atlas.cells_per_row * atlas.cells_per_row;

        u32 cell_index = 0;
        const u32 resident_capacity = static_cast<u32>(atlas.tiles.size()) * cells_per_tile;
        if (atlas.next_free_cell < resident_capacity) {
            cell_index = atlas.next_free_cell++;
        } else if (atlas.tiles.size() < config_.max_tiles_per_format) {
            auto texture = device.create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = texture_format(format),
                .extent = RHI::Extent3D{.width = tile_size_, .height = tile_size_, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst,
                .label = "text atlas tile",
            });
            if (!texture) {
                return unexpected(graphics_error_from_rhi(texture.error(), "create text atlas tile texture"));
            }
            auto view = device.create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .label = "text atlas tile view",
            });
            if (!view) {
                device.destroy_texture(*texture);
                return unexpected(graphics_error_from_rhi(view.error(), "create text atlas tile view"));
            }
            atlas.tiles.push_back(Tile{.texture = *texture, .view = *view, .current_layout = RHI::TextureLayout::Undefined});
            cell_index = atlas.next_free_cell++;
        } else {
            std::optional<GlyphKey> evicted = format_lru(format).evict_one();
            if (!evicted) {
                return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              "Text atlas is full and has no evictable glyph."});
            }
            auto it = resident_.find(*evicted);
            CellLocation reclaimed = it->second;
            resident_.erase(it);
            cell_index = reclaimed.tile_index * cells_per_tile + reclaimed.cell_y * atlas.cells_per_row + reclaimed.cell_x;
        }

        const u32 tile_index = cell_index / cells_per_tile;
        const u32 local = cell_index % cells_per_tile;
        return CellLocation{
            .tile_index = tile_index,
            .cell_x = local % atlas.cells_per_row,
            .cell_y = local / atlas.cells_per_row,
        };
    }

    Core::RendererResult TextAtlas::ensure_resident(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                     span<const GlyphRequest> requests, vector<GlyphSlot> &out_slots,
                                                     vector<RHI::BufferHandle> &out_transient_buffers) {
        out_slots.assign(requests.size(), GlyphSlot{});
        vector<PendingUpload> misses;

        for (usize i = 0; i < requests.size(); ++i) {
            const GlyphRequest &request = requests[i];
            const u32 reference_ppem = static_cast<u32>(request.pixel_size + 0.5f);
            const GlyphKey key{
                .font_id = request.font_id,
                .glyph_id = request.glyph_id,
                .reference_ppem = reference_ppem,
                .format = request.format,
            };

            auto it = resident_.find(key);
            if (it != resident_.end()) {
                format_lru(request.format).touch(key);
                out_slots[i] = slot_from_cell(request.format, it->second);
                continue;
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

            auto cell = allocate_cell(device, request.format);
            if (!cell) {
                return unexpected(cell.error());
            }
            cell->raster_size = raster_size_for(request.format, request.pixel_size, config_.padding_px, config_.cell_size);
            resident_[key] = *cell;
            format_lru(request.format).touch(key);
            misses.push_back(PendingUpload{.request_index = i, .cell = *cell});
        }

        if (misses.empty()) {
            return {};
        }
        return upload_misses(device, encoder, requests, misses, out_slots, out_transient_buffers);
    }

    Core::RendererResult TextAtlas::upload_misses(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                  span<const GlyphRequest> requests, const vector<PendingUpload> &misses,
                                                  vector<GlyphSlot> &out_slots, vector<RHI::BufferHandle> &out_transient_buffers) {
        // Rasterize every miss off the calling thread in parallel — this is the engine's job
        // system's (Async::Scheduler) first real consumer in the renderer.
        vector<Text::TextExpected<Text::RasterizedGlyph>> rasterized(misses.size());
        Async::par_iter(std::views::iota(usize{0}, misses.size())).for_each([&](usize i) {
            const GlyphRequest &request = requests[misses[i].request_index];
            const u32 raster_size = misses[i].cell.raster_size;
            if (request.format == Text::RasterFormat::Color) {
                const Text::ColorRasterParams params{
                    .width = raster_size,
                    .height = raster_size,
                    .pixel_size = request.pixel_size,
                };
                rasterized[i] = Text::rasterize_color_glyph(*request.font, request.glyph_id, params);
                return;
            }
            const f32 scale = request.pixel_size / static_cast<f32>(request.units_per_em);
            const Text::RasterParams params{
                .width = raster_size,
                .height = raster_size,
                .scale = scale,
                .pixel_range = config_.pixel_range,
                .padding_px = config_.padding_px,
            };
            rasterized[i] = Text::rasterize_glyph(*request.outline, request.format, params);
        });

        for (usize i = 0; i < misses.size(); ++i) {
            if (!rasterized[i]) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Text atlas glyph rasterization failed: " + rasterized[i].error().message);
            }
            out_slots[misses[i].request_index] = slot_from_cell(requests[misses[i].request_index].format, misses[i].cell);
        }

        // Batch every miss's pixels into one staging buffer, then one command buffer + submit +
        // wait — the throughput win the plan calls for over one round trip per glyph.
        usize total_bytes = 0;
        vector<usize> byte_offsets(misses.size());
        for (usize i = 0; i < misses.size(); ++i) {
            const Text::RasterFormat format = requests[misses[i].request_index].format;
            const usize texel_bytes = bytes_per_texel(texture_format(format));
            const usize raster_size = misses[i].cell.raster_size;
            byte_offsets[i] = total_bytes;
            total_bytes += raster_size * raster_size * texel_bytes;
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
            mark_touched(requests[miss.request_index].format, miss.cell.tile_index);
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
            const Tile &tile = format_atlas(format).tiles[misses[i].cell.tile_index];
            const RHI::BufferTextureCopy copy{
                .buffer_offset = byte_offsets[i],
                .mip_level = 0,
                .base_array_layer = 0,
                .array_layer_count = 1,
                .texture_offset = RHI::Offset3D{
                    static_cast<i32>(misses[i].cell.cell_x * config_.cell_size),
                    static_cast<i32>(misses[i].cell.cell_y * config_.cell_size),
                    0,
                },
                .texture_extent = RHI::Extent3D{.width = misses[i].cell.raster_size, .height = misses[i].cell.raster_size, .depth_or_layers = 1},
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
            atlas->next_free_cell = 0;
        }
        resident_.clear();
    }

} // namespace SFT::Renderer
