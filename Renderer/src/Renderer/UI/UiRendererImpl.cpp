#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <glm/geometric.hpp>
#include <limits>
#include <span>
#pragma endregion

#include <Renderer/UI/UiRenderer.hpp>

using std::unexpected;

namespace SFT::UI {

    namespace {
        std::atomic<u64> next_ui_renderer_generation{1};
    }

    /// Assigns a new value to this `UI`.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function does not throw exceptions.
    UiRenderer &UiRenderer::operator=(UiRenderer &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        text_pipeline_ = std::move(other.text_pipeline_);
        quad_pipeline_ = std::move(other.quad_pipeline_);
        stroke_pipeline_ = std::move(other.stroke_pipeline_);
        sector_pipeline_ = std::move(other.sector_pipeline_);
        custom_element_pipeline_ = std::move(other.custom_element_pipeline_);
        custom_stroke_element_pipeline_ = std::move(other.custom_stroke_element_pipeline_);
        surface_frame_resources_ = std::move(other.surface_frame_resources_);
        color_format_ = other.color_format_;
        generation_.store(other.generation_.load(std::memory_order_acquire), std::memory_order_release);
        enable_shader_disk_cache_ = other.enable_shader_disk_cache_;
        white_texture_ = other.white_texture_;
        operation_mutex_ = std::move(other.operation_mutex_);
        ready_ = other.ready_;
        return *this;
    }

    /// Creates a `UI` resource or value from the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param color_format Format used for the resource, render target, or conversion.
    /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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

        auto stroke_pipeline = UiStrokePipeline::create(device, color_format, enable_shader_disk_cache);
        if (!stroke_pipeline) {
            renderer.text_pipeline_.destroy(device);
            renderer.quad_pipeline_.destroy(device);
            return unexpected(stroke_pipeline.error());
        }
        renderer.stroke_pipeline_ = std::move(*stroke_pipeline);

        auto sector_pipeline = UiSectorPipeline::create(device, color_format, enable_shader_disk_cache);
        if (!sector_pipeline) {
            renderer.text_pipeline_.destroy(device);
            renderer.quad_pipeline_.destroy(device);
            renderer.stroke_pipeline_.destroy(device);
            return unexpected(sector_pipeline.error());
        }
        renderer.sector_pipeline_ = std::move(*sector_pipeline);
        renderer.color_format_ = color_format;
        renderer.enable_shader_disk_cache_ = enable_shader_disk_cache;

        renderer.generation_.store(next_ui_renderer_generation.fetch_add(1, std::memory_order_relaxed),
                                   std::memory_order_release);
        renderer.ready_ = true;
        return renderer;
    }

    namespace {


        struct PaintEntry {
            PaintKey paint;
            enum class Kind : u8 { Quad, Text, Custom, Stroke, Fill, Sector, CustomStroke } kind = Kind::Quad;
            usize index = 0;
            usize count = 1;
        };

        /// Cheap CPU-side visibility cull: an axis-aligned overlap test between an item's own bounding
        /// box and its resolved scissor rect. `false` means the item is either fully outside its own
        /// clip region (a scrolled-away panel, a hidden tab's stale content, ...) or its scissor
        /// collapsed to zero area entirely — either way, nothing of it would end up on screen, so it's
        /// dropped before ever reaching an instance buffer instead of being uploaded to the GPU and
        /// scissor-discarded per-pixel there. Conservative by construction (an AABB test never culls
        /// something that's even partially visible), so this only ever removes work, never geometry.
        ///
        /// @param scissor Resolved (already ancestor-intersected) scissor rect for this item.
        /// @param position Top-left of the item's own bounding box, same space as `scissor`.
        /// @param size Extent of the item's own bounding box.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool overlaps_scissor(const RHI::Rect2D &scissor, glm::vec2 position, glm::vec2 size) noexcept {
            if (scissor.width == 0 || scissor.height == 0) {
                return false;
            }
            const f32 scissor_x0 = static_cast<f32>(scissor.x);
            const f32 scissor_y0 = static_cast<f32>(scissor.y);
            const f32 scissor_x1 = scissor_x0 + static_cast<f32>(scissor.width);
            const f32 scissor_y1 = scissor_y0 + static_cast<f32>(scissor.height);
            return position.x < scissor_x1 && position.y < scissor_y1 &&
                   position.x + size.x > scissor_x0 && position.y + size.y > scissor_y0;
        }

        /// Converts one FillQuad into the UiQuadInstance UiQuadPipeline actually draws (fills are
        /// batched through the same pipeline as ordinary rect/border/image quads — see
        /// PaintEntry::Kind::Fill's own handling below).
        ///
        /// @param quad `quad` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiQuadInstance fill_quad_to_instance(const FillQuad &quad) noexcept {
            const Foundation::Color::Linear linear = quad.color.to_linear();
            return UiQuadInstance{
                .position = quad.position,
                .size = quad.size,
                .corner_radius = {quad.corner_radius.top_left, quad.corner_radius.top_right,
                                  quad.corner_radius.bottom_left, quad.corner_radius.bottom_right},
                .fill_color = {static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                              static_cast<f32>(linear.a)},
                .uv_min = {0.0f, 0.0f},
                .uv_max = {1.0f, 1.0f},
                .kind = static_cast<f32>(UiQuadKind::Rect),
            };
        }

        /// Converts one Sector into the UiSectorInstance UiSectorPipeline draws.
        ///
        /// @param sector `sector` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UiSectorInstance sector_to_instance(const Sector &sector) noexcept {
            const Foundation::Color::Linear linear = sector.style.color.to_linear();
            return UiSectorInstance{
                .center = sector.center,
                .inner_radius = sector.inner_radius,
                .outer_radius = sector.outer_radius,
                .start_angle = sector.start_angle,
                .end_angle = sector.end_angle,
                .feather_px = sector.style.feather_px,
                .color = {static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                         static_cast<f32>(linear.a)},
            };
        }

        /// Expands one resolved polyline path into per-segment stroke instances, appended to
        /// `out_instances`. Dash arc-length is measured independently per path, since each path is
        /// its own logical line even though several share one StrokeDraw (see StrokePolylineData's
        /// own doc comment for why).
        ///
        /// @param path `path` value used by the operation.
        /// @param out_instances `out_instances` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void expand_stroke_path_instances(const StrokePath &path, vector<UiStrokeInstance> &out_instances) noexcept {
            if (path.points.size() < 2) {
                return;
            }
            const Foundation::Color::Linear linear = path.style.color.to_linear();
            const glm::vec4 color{static_cast<f32>(linear.r), static_cast<f32>(linear.g), static_cast<f32>(linear.b),
                                  static_cast<f32>(linear.a)};
            const f32 half_width = std::max(path.style.width * 0.5f, 0.0f);
            const f32 snap = path.style.snap_to_pixel_grid ? 1.0f : 0.0f;

            f32 arc_length = 0.0f;
            for (usize i = 0; i + 1 < path.points.size(); ++i) {
                const glm::vec2 p0 = path.points[i];
                const glm::vec2 p1 = path.points[i + 1];
                out_instances.push_back(UiStrokeInstance{
                    .p0 = p0,
                    .p1 = p1,
                    .color = color,
                    .half_width = half_width,
                    .feather_px = path.style.feather_px,
                    .dash_length = path.style.dash_length,
                    .dash_gap = path.style.dash_gap,
                    .dash_phase = arc_length,
                    .snap_to_pixel_grid = snap,
                });
                arc_length += glm::length(p1 - p0);
            }
        }

    } // namespace

    /// Prepares the required state or resources for a later operation.
    ///
    /// @param device Device used or affected by the operation.
    /// @param encoder `encoder` value used by the operation.
    /// @param snapshot `snapshot` value used by the operation.
    /// @param texture_resolver Texture used or affected by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param frame_resource_index Zero-based index of the target element or entry.
    /// @param out_transient_buffers Buffer used or affected by the operation.
    /// @param out_retired_atlas_resources `out_retired_atlas_resources` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult UiRenderer::prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                             Renderer::RenderGraph &render_graph,
                                             const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                             Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                             vector<RHI::BufferHandle> &out_transient_buffers,
                                             Renderer::TextAtlasRetiredResources &out_retired_atlas_resources,
                                             vector<RHI::BindGroupHandle> &out_transient_bind_groups,
                                             vector<Renderer::RenderGraphTextureHandle> &out_glow_bloom_outputs) {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        if (!ready_) {


            return {};
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
        frame_resources.stroke_batches.clear();
        frame_resources.sector_batches.clear();
        frame_resources.custom_draws.clear();
        frame_resources.custom_group_ids.clear();
        frame_resources.custom_strokes.clear();
        frame_resources.custom_stroke_group_ids.clear();
        // Torn down here rather than immediately after use: the (surface, frame_resource_index) slot
        // being reused is itself the engine's existing signal that last time's GPU work on this slot
        // has completed (every other per-slot resource above is reused/rewritten the same way), so
        // it's safe to destroy the previous frame's owned glow mask/bloom textures now.
        if (frame_resources.glow_mask_view) device.destroy_texture_view(frame_resources.glow_mask_view);
        if (frame_resources.glow_mask_texture) device.destroy_texture(frame_resources.glow_mask_texture);
        if (frame_resources.glow_bloom_view) device.destroy_texture_view(frame_resources.glow_bloom_view);
        if (frame_resources.glow_bloom_texture) device.destroy_texture(frame_resources.glow_bloom_texture);
        frame_resources.glow_mask_texture = {};
        frame_resources.glow_mask_view = {};
        frame_resources.glow_bloom_texture = {};
        frame_resources.glow_bloom_view = {};
        frame_resources.glow_mask_stroke_batches.clear();
        // destroy_ui_quad_frame_resources (not a plain .clear()) is required here: glow_bloom_view is a
        // brand-new RHI::TextureViewHandle every frame (the texture above it was just destroyed and is
        // about to be recreated), so quad_pipeline_'s per-texture-view binding cache can never find a
        // matching entry to reuse — every frame would otherwise push one more *persistent* bind group
        // into glow_composite_quad.binding_cache that's never evicted, since nothing else ever removes
        // an entry from that cache. That's a real, unbounded leak of persistent-descriptor-pool-backed
        // bind groups (visible as ever-growing descriptor pool chunk counts), not just wasted memory.
        destroy_ui_quad_frame_resources(device, frame_resources.glow_composite_quad);
        frame_resources.glow_composite_quad_batches.clear();
        if (!white_texture_) {


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


        // A glow composite quad deliberately ignores its path's own (possibly parent-clipped) scissor
        // and uses this instead — real bloom halos spread past the geometry that produced them, so
        // clipping one to its own container would just crop the glow at an arbitrary box edge instead
        // of letting it flow over neighboring UI like an actual glow does.
        const Core::Extent2D viewport_extent = snapshot.viewport_extent();
        const RHI::Rect2D full_viewport_scissor{.x = 0, .y = 0, .width = viewport_extent.x, .height = viewport_extent.y};

        vector<PaintEntry> entries;
        entries.reserve(snapshot.quads_.size() + snapshot.strokes_.size() + snapshot.custom_draws_.size() + 8);
        for (usize i = 0; i < snapshot.quads_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.quads_[i].paint, .kind = PaintEntry::Kind::Quad, .index = i});
        }
        for (usize i = 0; i < snapshot.strokes_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.strokes_[i].paint, .kind = PaintEntry::Kind::Stroke, .index = i});
        }
        for (usize i = 0; i < snapshot.fills_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.fills_[i].paint, .kind = PaintEntry::Kind::Fill, .index = i});
        }
        for (usize i = 0; i < snapshot.sectors_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.sectors_[i].paint, .kind = PaintEntry::Kind::Sector, .index = i});
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
        for (usize i = 0; i < snapshot.custom_strokes_.size(); ++i) {
            entries.push_back(PaintEntry{.paint = snapshot.custom_strokes_[i].paint, .kind = PaintEntry::Kind::CustomStroke, .index = i});
        }
        std::sort(entries.begin(), entries.end(),
                 [](const PaintEntry &a, const PaintEntry &b) noexcept { return a.paint < b.paint; });


        vector<UiQuadInstance> quad_instances;
        vector<RHI::TextureViewHandle> quad_texture_views;
        vector<RHI::Rect2D> quad_scissors;
        vector<u32> quad_groups;
        vector<UiStrokeInstance> stroke_instances;
        vector<RHI::Rect2D> stroke_scissors;
        vector<u32> stroke_groups;
        vector<UiStrokeInstance> mask_instances;
        vector<RHI::Rect2D> mask_scissors;
        vector<u32> mask_groups;
        vector<UiSectorInstance> sector_instances;
        vector<RHI::Rect2D> sector_scissors;
        vector<u32> sector_groups;
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
                    if (!overlaps_scissor(quad.scissor, quad.instance.position, quad.instance.size)) {
                        break;
                    }
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
                case PaintEntry::Kind::Fill: {
                    // Batched through the same quad_pipeline_/quad_instances stream as ordinary
                    // Kind::Quad entries — a fill is just an untextured UiQuadInstance, so there's no
                    // reason to give it a separate pipeline.
                    const FillQuadDraw &fill = snapshot.fills_[entry.index];
                    for (const FillQuad &quad : fill.quads) {
                        if (!overlaps_scissor(fill.scissor, quad.position, quad.size)) {
                            continue;
                        }
                        quad_instances.push_back(fill_quad_to_instance(quad));
                        quad_texture_views.push_back(white_view);
                        quad_scissors.push_back(fill.scissor);
                        quad_groups.push_back(group_id);
                    }
                    break;
                }
                case PaintEntry::Kind::Stroke: {
                    const StrokeDraw &stroke = snapshot.strokes_[entry.index];
                    for (const StrokePath &path : stroke.paths) {
                        if (path.points.empty()) {
                            continue;
                        }
                        glm::vec2 bounds_min{std::numeric_limits<f32>::max()};
                        glm::vec2 bounds_max{std::numeric_limits<f32>::lowest()};
                        for (const glm::vec2 &point : path.points) {
                            bounds_min = glm::min(bounds_min, point);
                            bounds_max = glm::max(bounds_max, point);
                        }
                        const f32 pad = path.style.width * 0.5f + path.style.feather_px;
                        bounds_min -= glm::vec2{pad};
                        bounds_max += glm::vec2{pad};
                        if (!overlaps_scissor(stroke.scissor, bounds_min, bounds_max - bounds_min)) {
                            continue;
                        }
                        if (path.style.glow_intensity > 0.0f) {
                            // Drawn only into the shared glow mask (at this path's own screen position
                            // and scissor, brightness pre-scaled by its own glow_intensity), not into
                            // the normal crisp stroke stream — the mask's own bloom composite (built
                            // once for the whole frame below) already reproduces this path's crisp core
                            // via the source+bloom combine inside add_ui_glow_bloom_passes, so drawing
                            // it a second time here would double it up.
                            const usize before = mask_instances.size();
                            expand_stroke_path_instances(path, mask_instances);
                            for (usize k = before; k < mask_instances.size(); ++k) {
                                mask_instances[k].color.r *= path.style.glow_intensity;
                                mask_instances[k].color.g *= path.style.glow_intensity;
                                mask_instances[k].color.b *= path.style.glow_intensity;
                                mask_scissors.push_back(stroke.scissor);
                                mask_groups.push_back(0u);
                            }
                            continue;
                        }
                        const usize before = stroke_instances.size();
                        expand_stroke_path_instances(path, stroke_instances);
                        for (usize k = before; k < stroke_instances.size(); ++k) {
                            stroke_scissors.push_back(stroke.scissor);
                            stroke_groups.push_back(group_id);
                        }
                    }
                    break;
                }
                case PaintEntry::Kind::Sector: {
                    const SectorDraw &sector_draw = snapshot.sectors_[entry.index];
                    for (const Sector &sector : sector_draw.sectors) {
                        // Conservative: the full outer-radius bounding square, not the actual angular
                        // wedge — never culls a visible sector, just occasionally fails to cull an
                        // invisible one, which is the correct direction to be wrong in.
                        const glm::vec2 bounds_min = sector.center - glm::vec2{sector.outer_radius};
                        const glm::vec2 bounds_size{sector.outer_radius * 2.0f};
                        if (!overlaps_scissor(sector_draw.scissor, bounds_min, bounds_size)) {
                            continue;
                        }
                        sector_instances.push_back(sector_to_instance(sector));
                        sector_scissors.push_back(sector_draw.scissor);
                        sector_groups.push_back(group_id);
                    }
                    break;
                }
                case PaintEntry::Kind::Text: {
                    for (usize k = 0; k < entry.count; ++k) {
                        const Renderer::GlyphPlacement &glyph = snapshot.glyphs_[entry.index + k];
                        const RHI::Rect2D &glyph_scissor = snapshot.glyph_scissors_[entry.index + k];
                        // Padded generously: `glyph.position`/`size` are a pen-anchored placeholder box
                        // (size == {font_size, font_size}), not the glyph's exact rasterized bounds — the
                        // real quad TextInstance.cpp builds can be offset from this box by the font's own
                        // bearing metrics. A tight test here risks culling a genuinely visible glyph right
                        // at a clip edge; the padding trades a little precision for never doing that.
                        const f32 glyph_pad = glyph.pixel_size * 0.5f;
                        if (!overlaps_scissor(glyph_scissor, glyph.position - glm::vec2{glyph_pad},
                                              glyph.size + glm::vec2{glyph_pad * 2.0f})) {
                            continue;
                        }
                        ordered_glyphs.push_back(glyph);
                        ordered_glyph_scissors.push_back(glyph_scissor);
                        glyph_groups.push_back(group_id);
                    }
                    break;
                }
                case PaintEntry::Kind::Custom: {
                    frame_resources.custom_draws.push_back(snapshot.custom_draws_[entry.index]);
                    frame_resources.custom_group_ids.push_back(group_id);
                    break;
                }
                case PaintEntry::Kind::CustomStroke: {
                    frame_resources.custom_strokes.push_back(snapshot.custom_strokes_[entry.index]);
                    frame_resources.custom_stroke_group_ids.push_back(group_id);
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

        if (Core::RendererResult stroke_prepared = stroke_pipeline_.prepare(
                device, stroke_instances, stroke_scissors, stroke_groups,
                frame_resources.strokes, frame_resources.stroke_batches);
            !stroke_prepared) {
            return stroke_prepared;
        }

        if (Core::RendererResult sector_prepared = sector_pipeline_.prepare(
                device, sector_instances, sector_scissors, sector_groups,
                frame_resources.sectors, frame_resources.sector_batches);
            !sector_prepared) {
            return sector_prepared;
        }

        if (Core::RendererResult custom_prepared =
                custom_element_pipeline_.prepare(
                    device, color_format_, frame_resources.custom_draws, enable_shader_disk_cache_);
            !custom_prepared) {
            return custom_prepared;
        }

        if (Core::RendererResult custom_stroke_prepared = custom_stroke_element_pipeline_.prepare(
                device, color_format_, frame_resources.custom_strokes, enable_shader_disk_cache_);
            !custom_stroke_prepared) {
            return custom_stroke_prepared;
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

        if (!mask_instances.empty()) {
            if (Core::RendererResult mask_prepared = stroke_pipeline_.prepare(
                    device, mask_instances, mask_scissors, mask_groups,
                    frame_resources.glow_mask_stroke, frame_resources.glow_mask_stroke_batches);
                !mask_prepared) {
                return mask_prepared;
            }

            auto mask_texture = device.create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = color_format_,
                .extent = RHI::Extent3D{.width = viewport_extent.x, .height = viewport_extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .label = "ui glow mask",
            });
            if (!mask_texture) {
                return unexpected(Renderer::graphics_error_from_rhi(mask_texture.error(), "create ui glow mask texture"));
            }
            frame_resources.glow_mask_texture = *mask_texture;
            auto mask_view = device.create_texture_view(RHI::TextureViewDesc{
                .texture = frame_resources.glow_mask_texture, .view_type = RHI::TextureViewType::View2D,
                .format = color_format_, .label = "ui glow mask view"});
            if (!mask_view) {
                return unexpected(Renderer::graphics_error_from_rhi(mask_view.error(), "create ui glow mask texture view"));
            }
            frame_resources.glow_mask_view = *mask_view;

            auto bloom_texture = device.create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = color_format_,
                .extent = RHI::Extent3D{.width = viewport_extent.x, .height = viewport_extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .label = "ui glow bloom",
            });
            if (!bloom_texture) {
                return unexpected(Renderer::graphics_error_from_rhi(bloom_texture.error(), "create ui glow bloom texture"));
            }
            frame_resources.glow_bloom_texture = *bloom_texture;
            auto bloom_view = device.create_texture_view(RHI::TextureViewDesc{
                .texture = frame_resources.glow_bloom_texture, .view_type = RHI::TextureViewType::View2D,
                .format = color_format_, .label = "ui glow bloom view"});
            if (!bloom_view) {
                return unexpected(Renderer::graphics_error_from_rhi(bloom_view.error(), "create ui glow bloom texture view"));
            }
            frame_resources.glow_bloom_view = *bloom_view;

            const Renderer::RenderGraphTextureHandle mask_handle = render_graph.import_texture(Renderer::RenderGraphImportedTextureDesc{
                .texture = frame_resources.glow_mask_texture,
                .default_view = frame_resources.glow_mask_view,
                .format = color_format_,
                .extent = RHI::Extent3D{.width = viewport_extent.x, .height = viewport_extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
            });
            const Renderer::RenderGraphTextureHandle bloom_handle = render_graph.import_texture(Renderer::RenderGraphImportedTextureDesc{
                .texture = frame_resources.glow_bloom_texture,
                .default_view = frame_resources.glow_bloom_view,
                .format = color_format_,
                .extent = RHI::Extent3D{.width = viewport_extent.x, .height = viewport_extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
            });

            UiStrokePipeline *stroke_pipeline = &stroke_pipeline_;
            const vector<UiStrokeDrawBatch> mask_batches = frame_resources.glow_mask_stroke_batches;
            const glm::vec2 viewport_f{viewport_extent};
            render_graph.add_render_pass("ui glow mask"_ustr)
                .add_color_attachment(Renderer::RenderGraphColorAttachmentDesc{
                    .texture = mask_handle,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = viewport_extent.x, .height = viewport_extent.y})
                .set_execute([stroke_pipeline, mask_batches, viewport_f](
                                 Renderer::RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = viewport_f.x, .height = viewport_f.y, .min_depth = 0.0f, .max_depth = 1.0f});
                    return stroke_pipeline->draw(pass, mask_batches, viewport_f);
                });

            // Per-path intensity is already baked into the mask's own brightness above, so this is a
            // flat pass-through multiplier on the shared blur/composite chain, not a second per-element
            // control.
            Renderer::RenderGraphSettings settings{};
            settings.bloom_intensity = 1.0f;
            if (Core::RendererResult bloom_added = texture_resolver->add_ui_glow_bloom_passes(
                    render_graph, mask_handle, viewport_extent, bloom_handle, color_format_, settings,
                    out_transient_bind_groups);
                !bloom_added) {
                return bloom_added;
            }
            out_glow_bloom_outputs.push_back(bloom_handle);

            const UiQuadInstance composite_instance{
                .position = {0.0f, 0.0f},
                .size = viewport_f,
                .corner_radius = {0.0f, 0.0f, 0.0f, 0.0f},
                .fill_color = {1.0f, 1.0f, 1.0f, 1.0f},
                .uv_min = {0.0f, 0.0f},
                .uv_max = {1.0f, 1.0f},
                .kind = static_cast<f32>(UiQuadKind::Image),
            };
            const vector<UiQuadInstance> composite_instances{composite_instance};
            const vector<RHI::TextureViewHandle> composite_views{frame_resources.glow_bloom_view};
            const vector<RHI::Rect2D> composite_scissors{full_viewport_scissor};
            const vector<u32> composite_groups{0u};
            if (Core::RendererResult composite_prepared = quad_pipeline_.prepare(
                    device, composite_instances, composite_views, composite_scissors, composite_groups,
                    frame_resources.glow_composite_quad, frame_resources.glow_composite_quad_batches);
                !composite_prepared) {
                return composite_prepared;
            }
            // Additive blend, not a normal batched quad — see UiQuadDrawBatch::additive's own doc
            // comment (UiQuadPipeline.hpp).
            for (UiQuadDrawBatch &batch : frame_resources.glow_composite_quad_batches) {
                batch.additive = true;
            }
        }

        return {};
    }

    /// Draws the requested content using the current rendering state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param viewport_size Requested or available size for the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param frame_resource_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult UiRenderer::draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                          Core::RenderSurfaceHandle surface, u32 frame_resource_index) {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        auto surface_resources = std::ranges::find(
            surface_frame_resources_, surface, &SurfaceFrameResources::surface);
        if (surface_resources == surface_frame_resources_.end() ||
            frame_resource_index >= surface_resources->frames.size()) {


            return {};
        }
        FrameResources &frame_resources = surface_resources->frames[frame_resource_index];


        usize quad_cursor = 0;
        usize stroke_cursor = 0;
        usize sector_cursor = 0;
        usize text_cursor = 0;
        usize custom_cursor = 0;
        usize custom_stroke_cursor = 0;
        while (quad_cursor < frame_resources.quad_batches.size() ||
               stroke_cursor < frame_resources.stroke_batches.size() ||
               sector_cursor < frame_resources.sector_batches.size() ||
               text_cursor < frame_resources.text_batches.size() ||
               custom_cursor < frame_resources.custom_draws.size() ||
               custom_stroke_cursor < frame_resources.custom_strokes.size()) {
            u32 next_group = std::numeric_limits<u32>::max();
            if (quad_cursor < frame_resources.quad_batches.size()) {
                next_group = std::min(next_group, frame_resources.quad_batches[quad_cursor].paint_group);
            }
            if (stroke_cursor < frame_resources.stroke_batches.size()) {
                next_group = std::min(next_group, frame_resources.stroke_batches[stroke_cursor].paint_group);
            }
            if (sector_cursor < frame_resources.sector_batches.size()) {
                next_group = std::min(next_group, frame_resources.sector_batches[sector_cursor].paint_group);
            }
            if (text_cursor < frame_resources.text_batches.size()) {
                next_group = std::min(next_group, frame_resources.text_batches[text_cursor].paint_group);
            }
            if (custom_cursor < frame_resources.custom_draws.size()) {
                next_group = std::min(next_group, frame_resources.custom_group_ids[custom_cursor]);
            }
            if (custom_stroke_cursor < frame_resources.custom_strokes.size()) {
                next_group = std::min(next_group, frame_resources.custom_stroke_group_ids[custom_stroke_cursor]);
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
            while (stroke_cursor < frame_resources.stroke_batches.size() &&
                   frame_resources.stroke_batches[stroke_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = stroke_pipeline_.draw(
                        pass, span<const UiStrokeDrawBatch>{&frame_resources.stroke_batches[stroke_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++stroke_cursor;
            }
            while (sector_cursor < frame_resources.sector_batches.size() &&
                   frame_resources.sector_batches[sector_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = sector_pipeline_.draw(
                        pass, span<const UiSectorDrawBatch>{&frame_resources.sector_batches[sector_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++sector_cursor;
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
            while (custom_stroke_cursor < frame_resources.custom_strokes.size() &&
                   frame_resources.custom_stroke_group_ids[custom_stroke_cursor] == next_group) {
                if (Core::RendererResult drawn = custom_stroke_element_pipeline_.draw(
                        pass, color_format_, span<const CustomStrokeDraw>{&frame_resources.custom_strokes[custom_stroke_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++custom_stroke_cursor;
            }
        }

        // Composited last, on top of everything above, with additive blending and no scissor at all —
        // see FrameResources::glow_composite_quad_batches' own doc comment for why.
        if (!frame_resources.glow_composite_quad_batches.empty()) {
            if (Core::RendererResult drawn = quad_pipeline_.draw(pass, frame_resources.glow_composite_quad_batches, viewport_size);
                !drawn) {
                return drawn;
            }
        }
        return {};
    }

    /// Destroys or releases the `UI` resource represented by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiRenderer::destroy(RHI::RhiDevice &device) noexcept {
        auto operation_guard = operation_mutex_->lock();
        (void)operation_guard;
        for (SurfaceFrameResources &surface_resources : surface_frame_resources_) {
            for (FrameResources &resources : surface_resources.frames) {
                destroy_ui_quad_frame_resources(device, resources.quads);
                destroy_ui_stroke_frame_resources(device, resources.strokes);
                destroy_ui_sector_frame_resources(device, resources.sectors);
                destroy_text_frame_resources(device, resources.text);
                destroy_ui_stroke_frame_resources(device, resources.glow_mask_stroke);
                destroy_ui_quad_frame_resources(device, resources.glow_composite_quad);
                if (resources.glow_mask_view) device.destroy_texture_view(resources.glow_mask_view);
                if (resources.glow_mask_texture) device.destroy_texture(resources.glow_mask_texture);
                if (resources.glow_bloom_view) device.destroy_texture_view(resources.glow_bloom_view);
                if (resources.glow_bloom_texture) device.destroy_texture(resources.glow_bloom_texture);
            }
            surface_resources.text_atlas.destroy(device);
        }
        surface_frame_resources_.clear();
        quad_pipeline_.destroy(device);
        sector_pipeline_.destroy(device);
        stroke_pipeline_.destroy(device);
        custom_element_pipeline_.destroy(device);
        custom_stroke_element_pipeline_.destroy(device);
        text_pipeline_.destroy(device);
        ready_ = false;


        generation_.store(next_ui_renderer_generation.fetch_add(1, std::memory_order_relaxed),
                          std::memory_order_release);
    }

} // namespace SFT::UI
