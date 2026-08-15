#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <span>
#pragma endregion

#include "UiRenderer.hpp"

using std::unexpected;

namespace SFT::UI {

    namespace {
        std::atomic<u64> next_ui_renderer_generation{1};
    }

    Core::RendererExpected<UiRenderer> UiRenderer::create(
        RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache) {
        UiRenderer renderer;

        auto text_pipeline = Renderer::TextPipeline::create(device, color_format, enable_shader_disk_cache);
        if (!text_pipeline) {
            return unexpected(text_pipeline.error());
        }
        renderer.text_pipeline_ = std::move(*text_pipeline);

        auto quad_pipeline = UiQuadPipeline::create(device, color_format, enable_shader_disk_cache);
        if (!quad_pipeline) {
            renderer.text_pipeline_.destroy(device);
            return unexpected(quad_pipeline.error());
        }
        renderer.quad_pipeline_ = std::move(*quad_pipeline);
        renderer.color_format_ = color_format;
        renderer.enable_shader_disk_cache_ = enable_shader_disk_cache;

        renderer.generation_ = next_ui_renderer_generation.fetch_add(1, std::memory_order_relaxed);
        renderer.ready_ = true;
        return renderer;
    }

    namespace {

        // One draw item, spanning all three underlying kinds, carrying just enough to (a) sort
        // everything into one global paint order and (b) find its way back to the actual data
        // afterward. `index`/`count` are into FrameSnapshot's own arrays — for Text, `count` glyphs
        // starting at `index` (one run == one original Clay TEXT command, see PaintKey's own doc
        // comment on why every glyph in a run shares one PaintKey).
        struct PaintEntry {
            PaintKey paint;
            enum class Kind : u8 { Quad, Text, Custom } kind = Kind::Quad;
            usize index = 0;
            usize count = 1;
        };

    } // namespace

    Core::RendererResult UiRenderer::prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                             const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                             Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                             vector<RHI::BufferHandle> &out_transient_buffers,
                                             Renderer::TextAtlasRetiredResources &out_retired_atlas_resources) {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        if (!ready_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "UiRenderer was not created.");
        }

        if (texture_resolver == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiRenderer::prepare requires a texture_resolver.");
        }
        if (!surface.is_valid()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiRenderer::prepare requires a valid render surface.");
        }
        auto surface_resources = std::ranges::find(
            surface_frame_resources_, surface, &SurfaceFrameResources::surface);
        if (surface_resources == surface_frame_resources_.end()) {
            auto text_atlas = Renderer::TextAtlas::create(device, Renderer::TextAtlas::Config{});
            if (!text_atlas) {
                return unexpected(text_atlas.error());
            }
            surface_frame_resources_.push_back(SurfaceFrameResources{
                .surface = surface,
                .text_atlas = std::move(*text_atlas),
            });
            surface_resources = std::prev(surface_frame_resources_.end());
        }
        if (frame_resource_index >= surface_resources->frames.size()) {
            surface_resources->frames.resize(static_cast<usize>(frame_resource_index) + 1u);
        }
        FrameResources &frame_resources = surface_resources->frames[frame_resource_index];
        frame_resources.text_batches.clear();
        frame_resources.quad_batches.clear();
        frame_resources.custom_draws.clear();
        frame_resources.custom_group_ids.clear();
        if (!white_texture_) {
            // Renderer::ensure_default_white_texture() is private (Renderer's own material-fallback
            // internal, not part of its public surface) — UiRenderer creates its own 1x1 white
            // texture the same way any other texture-owning consumer would, through the public
            // create_texture() path, and caches the resulting handle for the pipeline's lifetime.
            const std::array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
            auto white_handle = texture_resolver->create_texture(1, 1, RHI::Format::RGBA8Unorm,
                                                                  span<const std::byte>{white.data(), white.size()},
                                                                  "ui quad white");
            if (!white_handle) {
                return unexpected(white_handle.error());
            }
            white_texture_ = *white_handle;
        }
        RHI::TextureViewHandle white_view{};
        if (Renderer::TextureResource *resource = texture_resolver->texture(white_texture_)) {
            white_view = resource->view;
        }
        if (!white_view) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiRenderer::prepare: the default white texture is no longer valid.");
        }

        // Merge every draw item from the snapshot into one global paint order (see PaintKey's own
        // doc comment, Style.hpp, and this class's own doc comment) — sorted by (z, paint_index),
        // never reordering items that share both.
        vector<PaintEntry> entries;
        entries.reserve(snapshot.quads_.size() + snapshot.custom_draws_.size() + 8);
        for (usize i = 0; i < snapshot.quads_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.quads_[i].paint, .kind = PaintEntry::Kind::Quad, .index = i});
        }
        {
            usize i = 0;
            while (i < snapshot.glyphs_.size()) {
                usize j = i + 1;
                while (j < snapshot.glyphs_.size() && snapshot.glyph_paint_[j].order == snapshot.glyph_paint_[i].order) {
                    ++j;
                }
                entries.push_back(
                    PaintEntry{.paint = snapshot.glyph_paint_[i], .kind = PaintEntry::Kind::Text, .index = i, .count = j - i});
                i = j;
            }
        }
        for (usize i = 0; i < snapshot.custom_draws_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.custom_draws_[i].paint, .kind = PaintEntry::Kind::Custom, .index = i});
        }
        std::sort(entries.begin(), entries.end(),
                 [](const PaintEntry &a, const PaintEntry &b) noexcept { return a.paint < b.paint; });

        // Split the sorted merge back into per-kind arrays, each carrying a `group id` that
        // increments every time the *kind* changes along the sorted sequence — the extra batching
        // key UiQuadPipeline::prepare()/Renderer::TextPipeline::prepare() need so their own
        // (texture/format-tile, scissor) merging never bridges a gap that used to hold a
        // different-kind item (see their own doc comments). draw() below walks all three back out
        // in lockstep by ascending group id.
        vector<UiQuadInstance> quad_instances;
        vector<RHI::TextureViewHandle> quad_texture_views;
        vector<RHI::Rect2D> quad_scissors;
        vector<u32> quad_groups;
        vector<Renderer::GlyphPlacement> ordered_glyphs;
        vector<RHI::Rect2D> ordered_glyph_scissors;
        vector<u32> glyph_groups;

        u32 group_id = 0;
        bool has_previous_kind = false;
        PaintEntry::Kind previous_kind{};
        for (const PaintEntry &entry : entries) {
            if (has_previous_kind && entry.kind != previous_kind) {
                ++group_id;
            }
            has_previous_kind = true;
            previous_kind = entry.kind;

            switch (entry.kind) {
                case PaintEntry::Kind::Quad: {
                    const QuadDraw &quad = snapshot.quads_[entry.index];
                    RHI::TextureViewHandle view = white_view;
                    if (quad.image_ref != nullptr) {
                        if (Renderer::TextureResource *resource = texture_resolver->texture(quad.image_ref->texture)) {
                            view = resource->view;
                        }
                    }
                    quad_instances.push_back(quad.instance);
                    quad_texture_views.push_back(view);
                    quad_scissors.push_back(quad.scissor);
                    quad_groups.push_back(group_id);
                    break;
                }
                case PaintEntry::Kind::Text: {
                    for (usize k = 0; k < entry.count; ++k) {
                        ordered_glyphs.push_back(snapshot.glyphs_[entry.index + k]);
                        ordered_glyph_scissors.push_back(snapshot.glyph_scissors_[entry.index + k]);
                        glyph_groups.push_back(group_id);
                    }
                    break;
                }
                case PaintEntry::Kind::Custom: {
                    frame_resources.custom_draws.push_back(snapshot.custom_draws_[entry.index]);
                    frame_resources.custom_group_ids.push_back(group_id);
                    break;
                }
            }
        }

        if (Core::RendererResult quad_prepared = quad_pipeline_.prepare(
                device, quad_instances, quad_texture_views, quad_scissors, quad_groups,
                frame_resources.quads, frame_resources.quad_batches);
            !quad_prepared) {
            return quad_prepared;
        }

        if (Core::RendererResult custom_prepared =
                custom_element_pipeline_.prepare(
                    device, color_format_, frame_resources.custom_draws, enable_shader_disk_cache_);
            !custom_prepared) {
            return custom_prepared;
        }

        if (!ordered_glyphs.empty()) {
            vector<Renderer::GlyphRequest> requests;
            requests.reserve(ordered_glyphs.size());
            for (const Renderer::GlyphPlacement &placement : ordered_glyphs) {
                requests.push_back(Renderer::GlyphRequest{
                    .font_id = placement.font_id,
                    .glyph_id = placement.glyph_id,
                    .units_per_em = placement.units_per_em,
                    .pixel_size = placement.pixel_size,
                    .format = placement.format,
                    .outline = placement.outline,
                    .font = placement.font,
                });
            }
            vector<Renderer::GlyphSlot> slots;
            if (auto resident = surface_resources->text_atlas.ensure_resident(
                    device, encoder, requests, slots, out_transient_buffers, out_retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }

            vector<Renderer::GlyphInstance> instances;
            instances.reserve(ordered_glyphs.size());
            for (usize i = 0; i < ordered_glyphs.size(); ++i) {
                instances.push_back(
                    Renderer::make_glyph_instance(ordered_glyphs[i].position, ordered_glyphs[i], slots[i],
                                                  surface_resources->text_atlas.pixel_range()));
            }
            if (Core::RendererResult text_prepared =
                    text_pipeline_.prepare(device, surface_resources->text_atlas, instances, slots,
                                           ordered_glyph_scissors, glyph_groups,
                                           frame_resources.text, frame_resources.text_batches);
                !text_prepared) {
                return text_prepared;
            }
        }

        return {};
    }

    Core::RendererResult UiRenderer::draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                          Core::RenderSurfaceHandle surface, u32 frame_resource_index) {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        auto surface_resources = std::ranges::find(
            surface_frame_resources_, surface, &SurfaceFrameResources::surface);
        if (surface_resources == surface_frame_resources_.end() ||
            frame_resource_index >= surface_resources->frames.size()) {
            // A backend reconstruction intentionally drops frames assembled against the old device.
            // Their overlay draw hook may still execute after the replacement renderer is installed,
            // but there is correctly no prepared UI work for that new generation/slot. Drawing an
            // empty overlay is the valid result; malformed resources that do exist still fail below.
            return {};
        }
        FrameResources &frame_resources = surface_resources->frames[frame_resource_index];

        // Walk quad/text/custom batches back out in ascending paint-order-group lockstep. Each
        // pipeline sets its batch's scissor internally, so only pipeline switches need coordinating.
        usize quad_cursor = 0;
        usize text_cursor = 0;
        usize custom_cursor = 0;
        while (quad_cursor < frame_resources.quad_batches.size() ||
               text_cursor < frame_resources.text_batches.size() ||
               custom_cursor < frame_resources.custom_draws.size()) {
            u32 next_group = std::numeric_limits<u32>::max();
            if (quad_cursor < frame_resources.quad_batches.size()) {
                next_group = std::min(next_group, frame_resources.quad_batches[quad_cursor].paint_group);
            }
            if (text_cursor < frame_resources.text_batches.size()) {
                next_group = std::min(next_group, frame_resources.text_batches[text_cursor].paint_group);
            }
            if (custom_cursor < frame_resources.custom_draws.size()) {
                next_group = std::min(next_group, frame_resources.custom_group_ids[custom_cursor]);
            }

            while (quad_cursor < frame_resources.quad_batches.size() &&
                   frame_resources.quad_batches[quad_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = quad_pipeline_.draw(
                        pass, span<const UiQuadDrawBatch>{&frame_resources.quad_batches[quad_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++quad_cursor;
            }
            while (text_cursor < frame_resources.text_batches.size() &&
                   frame_resources.text_batches[text_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = text_pipeline_.draw(
                        pass, span<const Renderer::TextDrawBatch>{&frame_resources.text_batches[text_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++text_cursor;
            }
            while (custom_cursor < frame_resources.custom_draws.size() &&
                   frame_resources.custom_group_ids[custom_cursor] == next_group) {
                if (Core::RendererResult drawn = custom_element_pipeline_.draw(
                        pass, color_format_, span<const CustomDraw>{&frame_resources.custom_draws[custom_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++custom_cursor;
            }
        }
        return {};
    }

    void UiRenderer::destroy(RHI::RhiDevice &device) noexcept {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        for (SurfaceFrameResources &surface_resources : surface_frame_resources_) {
            for (FrameResources &resources : surface_resources.frames) {
                destroy_ui_quad_frame_resources(device, resources.quads);
                destroy_text_frame_resources(device, resources.text);
            }
            surface_resources.text_atlas.destroy(device);
        }
        surface_frame_resources_.clear();
        quad_pipeline_.destroy(device);
        custom_element_pipeline_.destroy(device);
        text_pipeline_.destroy(device);
        ready_ = false;
    }

} // namespace SFT::UI
