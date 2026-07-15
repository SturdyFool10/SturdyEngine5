#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <expected>
#include <glm/vec2.hpp>
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/TextCanvas.hpp>
#include <Renderer/TileGrid.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>
#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    Core::RendererExpected<TextCanvas> TextCanvas::create(RHI::RhiDevice &device, const Config &config, TextAtlas &atlas,
                                                           TextPipeline &pipeline) {
        TextCanvas canvas;
        canvas.config_ = config;
        canvas.tile_size_ = clamp_tile_size(config.desired_tile_size, device.limits());
        if (canvas.tile_size_ == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text canvas tile size must be non-zero."});
        }
        canvas.atlas_ = &atlas;
        canvas.pipeline_ = &pipeline;
        return canvas;
    }

    void TextCanvas::draw_run(span<const GlyphPlacement> glyphs) {
        for (const GlyphPlacement &glyph : glyphs) {
            const vector<TileCoord> overlapped = tiles_overlapping(
                static_cast<i32>(glyph.position.x), static_cast<i32>(glyph.position.y),
                static_cast<u32>(glyph.size.x), static_cast<u32>(glyph.size.y), tile_size_);
            for (TileCoord coord : overlapped) {
                tile_glyphs_[coord].push_back(glyph);
                if (auto it = resident_tiles_.find(coord); it != resident_tiles_.end()) {
                    it->second.dirty = true;
                }
            }
        }
    }

    Core::RendererResult TextCanvas::render_tile(RHI::RhiDevice &device, TileCoord coord, TileRecord &tile) {
        const glm::vec2 tile_origin{static_cast<f32>(coord.x) * static_cast<f32>(tile_size_),
                                    static_cast<f32>(coord.y) * static_cast<f32>(tile_size_)};

        vector<GlyphPlacement> empty_glyphs;
        const vector<GlyphPlacement> &glyphs = [&]() -> const vector<GlyphPlacement> & {
            auto it = tile_glyphs_.find(coord);
            return it != tile_glyphs_.end() ? it->second : empty_glyphs;
        }();

        // This tile render is a standalone one-shot (an on-demand offscreen re-raster of one dirty
        // tile, not part of any per-frame render graph), so — unlike the main frame's shared
        // encoder — it owns and submits/waits on its own command encoder for its whole lifetime,
        // atlas upload included.
        auto encoder = device.create_command_encoder(RHI::CommandEncoderDesc{.label = "text canvas tile render"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create text canvas tile encoder"));
        }
        vector<RHI::BufferHandle> transient_buffers;

        vector<GlyphSlot> slots;
        vector<GlyphInstance> instances;
        if (!glyphs.empty()) {
            vector<GlyphRequest> requests;
            requests.reserve(glyphs.size());
            for (const GlyphPlacement &glyph : glyphs) {
                requests.push_back(GlyphRequest{
                    .font_id = glyph.font_id,
                    .glyph_id = glyph.glyph_id,
                    .units_per_em = glyph.units_per_em,
                    .pixel_size = glyph.pixel_size,
                    .format = glyph.format,
                    .outline = glyph.outline,
                    .font = glyph.font,
                });
            }
            if (auto resident = atlas_->ensure_resident(device, **encoder, requests, slots, transient_buffers); !resident) {
                return unexpected(resident.error());
            }

            instances.reserve(glyphs.size());
            for (usize i = 0; i < glyphs.size(); ++i) {
                const GlyphSlot &slot = slots[i];
                const f32 scale = slot.cell_size_px > 0.0f ? glyphs[i].size.x / slot.cell_size_px : 1.0f;
                instances.push_back(GlyphInstance{
                    .position = glyphs[i].position - tile_origin,
                    .size = glyphs[i].size,
                    .uv_min = slot.uv_min,
                    .uv_max = slot.uv_max,
                    .color = glyphs[i].color,
                    .format_kind = format_kind_value(slot.format),
                    .screen_px_range = atlas_->pixel_range() * scale,
                });
            }
        }

        vector<TextDrawBatch> batches;
        if (auto prepared = pipeline_->prepare(device, instances, slots, batches, transient_buffers); !prepared) {
            return unexpected(prepared.error());
        }

        const RHI::TextureBarrier to_attachment{
            .texture = tile.texture,
            .src_stage = RHI::PipelineStage::None,
            .src_access = RHI::AccessFlags::None,
            .dst_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .dst_access = RHI::AccessFlags::ColorAttachmentWrite,
            .old_layout = RHI::TextureLayout::Undefined,
            .new_layout = RHI::TextureLayout::ColorAttachment,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_attachment, 1});

        const RHI::ColorAttachment color_attachment{
            .view = tile.view,
            .load_op = RHI::LoadOp::Clear,
            .store_op = RHI::StoreOp::Store,
            .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
        };
        const RHI::RenderPassDesc pass_desc{
            .color_attachments = span<const RHI::ColorAttachment>{&color_attachment, 1},
            .render_area = RHI::Rect2D{.x = 0, .y = 0, .width = tile_size_, .height = tile_size_},
            .label = "text canvas tile",
        };
        auto pass = (*encoder)->begin_render_pass(pass_desc);
        if (!pass) {
            return unexpected(graphics_error_from_rhi(pass.error(), "begin text canvas tile render pass"));
        }
        (*pass)->set_viewport(RHI::Viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<f32>(tile_size_), .height = static_cast<f32>(tile_size_)});
        (*pass)->set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = tile_size_, .height = tile_size_});

        vector<RHI::BindGroupHandle> transient_bind_groups;
        Core::RendererResult draw_result = pipeline_->draw(device, **pass, *atlas_, batches,
                                                            glm::vec2{static_cast<f32>(tile_size_), static_cast<f32>(tile_size_)},
                                                            transient_bind_groups);
        (*pass)->end();
        if (!draw_result) {
            return draw_result;
        }

        const RHI::TextureBarrier to_sampled{
            .texture = tile.texture,
            .src_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .src_access = RHI::AccessFlags::ColorAttachmentWrite,
            .dst_stage = RHI::PipelineStage::FragmentShader,
            .dst_access = RHI::AccessFlags::ShaderRead,
            .old_layout = RHI::TextureLayout::ColorAttachment,
            .new_layout = RHI::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish text canvas tile encoder"));
        }
        auto fence = device.create_fence(RHI::FenceDesc{.label = "text canvas tile fence"});
        if (!fence) {
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create text canvas tile fence"));
        }
        const array command_buffers{*command_buffer};
        const RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "text canvas tile submit",
        };
        if (auto submitted = device.submit(submit_desc); !submitted) {
            device.destroy_fence(*fence);
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit text canvas tile render"));
        }
        if (auto waited = device.wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true); !waited) {
            device.destroy_fence(*fence);
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(waited.error(), "wait text canvas tile fence"));
        }
        device.destroy_fence(*fence);
        device.destroy_command_buffer(*command_buffer);

        for (RHI::BindGroupHandle bind_group : transient_bind_groups) {
            device.destroy_bind_group(bind_group);
        }
        for (RHI::BufferHandle buffer : transient_buffers) {
            device.destroy_buffer(buffer);
        }

        tile.dirty = false;
        return {};
    }

    Core::RendererResult TextCanvas::evict_if_over_budget(RHI::RhiDevice &device, span<const TileCoord> keep) {
        while (resident_tiles_.size() > config_.max_resident_tiles) {
            std::optional<TileCoord> victim = lru_.evict_one();
            if (!victim) {
                break;
            }
            if (std::ranges::find(keep, *victim) != keep.end()) {
                // Still needed this call — put it back at the front and stop; nothing else is
                // evictable without violating the caller's own viewport request.
                lru_.touch(*victim);
                break;
            }
            auto it = resident_tiles_.find(*victim);
            if (it != resident_tiles_.end()) {
                if (it->second.view) {
                    device.destroy_texture_view(it->second.view);
                }
                if (it->second.texture) {
                    device.destroy_texture(it->second.texture);
                }
                resident_tiles_.erase(it);
            }
        }
        return {};
    }

    Core::RendererExpected<vector<ResidentCanvasTile>> TextCanvas::ensure_viewport_resident(RHI::RhiDevice &device,
                                                                                            RHI::Rect2D viewport) {
        const vector<TileCoord> coords = tiles_overlapping(viewport.x, viewport.y, viewport.width, viewport.height, tile_size_);
        vector<ResidentCanvasTile> result;
        result.reserve(coords.size());

        for (TileCoord coord : coords) {
            lru_.touch(coord);

            auto it = resident_tiles_.find(coord);
            if (it == resident_tiles_.end()) {
                auto texture = device.create_texture(RHI::TextureDesc{
                    .dimension = RHI::TextureDimension::Dim2D,
                    .format = RHI::Format::RGBA8Unorm,
                    .extent = RHI::Extent3D{.width = tile_size_, .height = tile_size_, .depth_or_layers = 1},
                    .mip_levels = 1,
                    .samples = RHI::SampleCount::X1,
                    .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::ColorAttachment,
                    .label = "text canvas tile",
                });
                if (!texture) {
                    return unexpected(graphics_error_from_rhi(texture.error(), "create text canvas tile texture"));
                }
                auto view = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = *texture,
                    .view_type = RHI::TextureViewType::View2D,
                    .label = "text canvas tile view",
                });
                if (!view) {
                    device.destroy_texture(*texture);
                    return unexpected(graphics_error_from_rhi(view.error(), "create text canvas tile view"));
                }
                it = resident_tiles_.emplace(coord, TileRecord{.texture = *texture, .view = *view, .dirty = true}).first;
            }

            if (it->second.dirty) {
                if (auto rendered = render_tile(device, coord, it->second); !rendered) {
                    return unexpected(rendered.error());
                }
            }

            result.push_back(ResidentCanvasTile{
                .coord = coord,
                .view = it->second.view,
                .logical_rect = RHI::Rect2D{
                    .x = coord.x * static_cast<i32>(tile_size_),
                    .y = coord.y * static_cast<i32>(tile_size_),
                    .width = tile_size_,
                    .height = tile_size_,
                },
            });
        }

        if (auto evicted = evict_if_over_budget(device, coords); !evicted) {
            return unexpected(evicted.error());
        }
        return result;
    }

    void TextCanvas::destroy(RHI::RhiDevice &device) noexcept {
        for (auto &[coord, tile] : resident_tiles_) {
            if (tile.view) {
                device.destroy_texture_view(tile.view);
            }
            if (tile.texture) {
                device.destroy_texture(tile.texture);
            }
        }
        resident_tiles_.clear();
        tile_glyphs_.clear();
        atlas_ = nullptr;
        pipeline_ = nullptr;
    }

} // namespace SFT::Renderer
