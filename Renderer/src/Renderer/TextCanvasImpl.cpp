#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <glm/vec2.hpp>
#include <limits>
#include <optional>
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

    namespace {

        struct PlacementBounds {
            f32 left = 0.0f;
            f32 top = 0.0f;
            f32 right = 0.0f;
            f32 bottom = 0.0f;
        };

        [[nodiscard]] std::optional<PlacementBounds> placement_bounds(const GlyphPlacement &glyph) noexcept {
            const f32 inverse_em = 1.0f / static_cast<f32>(std::max(glyph.units_per_em, 1u));
            const glm::vec2 scale = glyph.size * inverse_em;
            f32 left_units = 0.0f;
            f32 bottom_units = 0.0f;
            f32 right_units = static_cast<f32>(glyph.units_per_em);
            f32 top_units = static_cast<f32>(glyph.units_per_em);

            if (glyph.format == Text::RasterFormat::Color && glyph.font != nullptr) {
                hb_glyph_extents_t extents{};
                if (hb_font_get_glyph_extents(glyph.font->handle(), glyph.glyph_id, &extents)) {
                    left_units = static_cast<f32>(extents.x_bearing);
                    right_units = static_cast<f32>(extents.x_bearing + extents.width);
                    top_units = static_cast<f32>(extents.y_bearing);
                    bottom_units = static_cast<f32>(extents.y_bearing + extents.height);
                }
            } else if (glyph.outline != nullptr) {
                const Text::GlyphBounds bounds = Text::glyph_bounds(*glyph.outline);
                if (bounds.empty) {
                    return std::nullopt; // spaces/control glyphs have no visible tile coverage
                }
                left_units = bounds.left;
                bottom_units = bounds.bottom;
                right_units = bounds.right;
                top_units = bounds.top;
            }

            const array<glm::vec2, 4> local_corners{
                glm::vec2{left_units * scale.x, -top_units * scale.y},
                glm::vec2{right_units * scale.x, -top_units * scale.y},
                glm::vec2{right_units * scale.x, -bottom_units * scale.y},
                glm::vec2{left_units * scale.x, -bottom_units * scale.y},
            };
            const f32 cosine = std::cos(glyph.rotation);
            const f32 sine = std::sin(glyph.rotation);
            PlacementBounds result{
                .left = std::numeric_limits<f32>::max(),
                .top = std::numeric_limits<f32>::max(),
                .right = std::numeric_limits<f32>::lowest(),
                .bottom = std::numeric_limits<f32>::lowest(),
            };
            for (const glm::vec2 corner : local_corners) {
                const glm::vec2 rotated{
                    corner.x * cosine - corner.y * sine,
                    corner.x * sine + corner.y * cosine,
                };
                const glm::vec2 point = glyph.position + rotated;
                result.left = std::min(result.left, point.x);
                result.top = std::min(result.top, point.y);
                result.right = std::max(result.right, point.x);
                result.bottom = std::max(result.bottom, point.y);
            }
            if (!std::isfinite(result.left) || !std::isfinite(result.top) ||
                !std::isfinite(result.right) || !std::isfinite(result.bottom)) {
                return std::nullopt;
            }
            return result;
        }

    } // namespace

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
            const std::optional<PlacementBounds> bounds = placement_bounds(glyph);
            if (!bounds) {
                continue;
            }
            // Distance-field reconstruction and stem darkening affect at most a fraction of a
            // screen pixel beyond the vector edge. One pixel keeps that antialiasing fringe in
            // every overlapped tile without treating the atlas's transparent guard band as ink.
            constexpr f32 fringe = 1.0f;
            const f32 left = std::floor(bounds->left - fringe);
            const f32 top = std::floor(bounds->top - fringe);
            const f32 right = std::ceil(bounds->right + fringe);
            const f32 bottom = std::ceil(bounds->bottom + fringe);
            constexpr f32 i32_min = static_cast<f32>(std::numeric_limits<i32>::min());
            constexpr f32 i32_max = static_cast<f32>(std::numeric_limits<i32>::max());
            if (left < i32_min || top < i32_min || right > i32_max || bottom > i32_max ||
                right <= left || bottom <= top) {
                continue;
            }
            const vector<TileCoord> overlapped = tiles_overlapping(
                static_cast<i32>(left), static_cast<i32>(top),
                static_cast<u32>(right - left), static_cast<u32>(bottom - top), tile_size_);
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
        TextAtlasRetiredResources retired_atlas_resources;

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
            if (auto resident = atlas_->ensure_resident(device, **encoder, requests, slots, transient_buffers,
                                                        retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }

            instances.reserve(glyphs.size());
            for (usize i = 0; i < glyphs.size(); ++i) {
                instances.push_back(make_glyph_instance(glyphs[i].position - tile_origin, glyphs[i], slots[i], atlas_->pixel_range()));
            }
        }

        vector<TextDrawBatch> batches;
        if (auto prepared = pipeline_->prepare(device, *atlas_, instances, slots, tile.text_resources, batches); !prepared) {
            return unexpected(prepared.error());
        }

        const RHI::TextureBarrier to_attachment{
            .texture = tile.texture,
            .src_stage = tile.current_layout == RHI::TextureLayout::Undefined
                             ? RHI::PipelineStage::None
                             : RHI::PipelineStage::FragmentShader,
            .src_access = tile.current_layout == RHI::TextureLayout::Undefined
                              ? RHI::AccessFlags::None
                              : RHI::AccessFlags::ShaderRead,
            .dst_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .dst_access = RHI::AccessFlags::ColorAttachmentWrite,
            .old_layout = tile.current_layout,
            .new_layout = RHI::TextureLayout::ColorAttachment,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_attachment, 1});
        tile.current_layout = RHI::TextureLayout::ColorAttachment;

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

        Core::RendererResult draw_result = pipeline_->draw(
            **pass, batches, glm::vec2{static_cast<f32>(tile_size_), static_cast<f32>(tile_size_)});
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
        tile.current_layout = RHI::TextureLayout::ShaderReadOnly;

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

        for (RHI::BufferHandle buffer : transient_buffers) {
            device.destroy_buffer(buffer);
        }
        for (RHI::TextureViewHandle view : retired_atlas_resources.texture_views) {
            device.destroy_texture_view(view);
        }
        for (RHI::TextureHandle texture : retired_atlas_resources.textures) {
            device.destroy_texture(texture);
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
                destroy_text_frame_resources(device, it->second.text_resources);
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
            destroy_text_frame_resources(device, tile.text_resources);
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
