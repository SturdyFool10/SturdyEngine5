#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <glm/geometric.hpp>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#pragma endregion

#include <Renderer/UI/UiRenderer.hpp>
#include <Async/ParIter.hpp>

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
        effect_children_ = std::move(other.effect_children_);
        next_effect_revision_ = other.next_effect_revision_;
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
        // The four pipelines compiled through the one shared Slang session; nothing is compiling now, so give its memory back.
        Core::Slang::ShaderCompiler::release_shared_session();
        renderer.color_format_ = color_format;
        renderer.enable_shader_disk_cache_ = enable_shader_disk_cache;

        renderer.generation_.store(next_ui_renderer_generation.fetch_add(1, std::memory_order_relaxed),
                                   std::memory_order_release);
        renderer.ready_ = true;
        return renderer;
    }

    namespace {


        /// Frames in a row the glow targets may go unused before their memory is given back.
        constexpr u32 kGlowIdleFramesBeforeRelease = 120;

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

    void UiRenderer::assign_paint_groups(const FrameSnapshot &snapshot, Core::Extent2D viewport, vector<PaintEntry> &entries) {
        // The order entries were sorted into (paint order) is what has to look unchanged wherever two things overlap.
        // Work out, per entry, the rectangle it can draw into, then place each entry in the earliest group of its kind
        // that comes after every group already touching that rectangle. A coarse grid remembers the latest group
        // that touched each cell, so this costs a few cells per entry, not a comparison against every other entry.
        static const bool reorder = [] {
            const char *forced = std::getenv("STURDY_UI_REORDER");
            return forced == nullptr || std::string_view{forced} != "0";
        }();
        constexpr f32 kEdgePad = 2.0f; // anti-aliased edges reach a little past the geometry
        constexpr i32 kCell = 8;
        const i32 columns = std::max<i32>(1, (static_cast<i32>(viewport.x) + kCell - 1) / kCell);
        const i32 rows = std::max<i32>(1, (static_cast<i32>(viewport.y) + kCell - 1) / kCell);
        vector<i32> latest(static_cast<usize>(columns) * static_cast<usize>(rows), -1);
        constexpr usize kKinds = 7;
        std::array<vector<u32>, kKinds> groups_of_kind;
        u32 group_count = 0;
        // Plain fills go through the quad pipeline, so they group with quads.
        const auto kind_slot = [](PaintEntry::Kind kind) {
            return static_cast<usize>(kind == PaintEntry::Kind::Fill ? PaintEntry::Kind::Quad : kind);
        };

        for (PaintEntry &entry : entries) {
            // Everything but the batched kinds (custom shader elements) is a barrier: it can neither move nor be moved past.
            bool barrier = false;
            glm::vec2 lo{std::numeric_limits<f32>::max()};
            glm::vec2 hi{std::numeric_limits<f32>::lowest()};
            const auto grow = [&](glm::vec2 a, glm::vec2 size, f32 pad) {
                lo = glm::min(lo, a - glm::vec2{pad});
                hi = glm::max(hi, a + size + glm::vec2{pad});
            };
            switch (entry.kind) {
                case PaintEntry::Kind::Quad: {
                    const UiQuadInstance &quad = snapshot.quads_[entry.index].instance;
                    grow(quad.position, quad.size, kEdgePad);
                    // A blend is a pipeline state, so a quad with one cannot share a draw with the quads around it.
                    if (snapshot.quads_[entry.index].blend != UiQuadBlend::Normal) {
                        barrier = true;
                    }
                    break;
                }
                case PaintEntry::Kind::Fill: {
                    for (const FillQuad &quad : snapshot.fills_[entry.index].quads) {
                        grow(quad.position, quad.size, kEdgePad);
                    }
                    break;
                }
                case PaintEntry::Kind::Stroke: {
                    for (const StrokePath &path : snapshot.strokes_[entry.index].paths) {
                        const f32 pad = path.style.width * 0.5f + path.style.feather_px + kEdgePad;
                        for (const glm::vec2 &point : path.points) {
                            grow(point, glm::vec2{0.0f}, pad);
                        }
                        // A glowing path also draws into the shared glow mask, which spreads across the surface.
                        if (path.style.glow_intensity > 0.0f) {
                            barrier = true;
                        }
                    }
                    break;
                }
                case PaintEntry::Kind::Sector: {
                    for (const Sector &sector : snapshot.sectors_[entry.index].sectors) {
                        grow(sector.center - glm::vec2{sector.outer_radius}, glm::vec2{sector.outer_radius * 2.0f},
                             kEdgePad + sector.style.feather_px);
                    }
                    break;
                }
                case PaintEntry::Kind::Text: {
                    for (usize k = 0; k < entry.count; ++k) {
                        const Renderer::GlyphPlacement &glyph = snapshot.glyphs_[entry.index + k];
                        // A glyph's quad can sit anywhere near its pen position (bearings, outlines, SDF spread).
                        const f32 pad = glyph.pixel_size + kEdgePad;
                        grow(glyph.position, glyph.size, pad);
                    }
                    break;
                }
                case PaintEntry::Kind::Custom:
                case PaintEntry::Kind::CustomStroke:
                    barrier = true;
                    break;
            }
            if (barrier || !reorder) {
                lo = glm::vec2{0.0f};
                hi = glm::vec2{static_cast<f32>(viewport.x), static_cast<f32>(viewport.y)};
            }
            // Cells the rectangle touches, clamped to the surface (what is off it draws nothing and constrains nothing).
            const i32 x0 = std::clamp(static_cast<i32>(std::floor(lo.x / kCell)), 0, columns - 1);
            const i32 x1 = std::clamp(static_cast<i32>(std::floor(hi.x / kCell)), 0, columns - 1);
            const i32 y0 = std::clamp(static_cast<i32>(std::floor(lo.y / kCell)), 0, rows - 1);
            const i32 y1 = std::clamp(static_cast<i32>(std::floor(hi.y / kCell)), 0, rows - 1);
            const bool on_surface = hi.x >= 0.0f && hi.y >= 0.0f && lo.x <= static_cast<f32>(viewport.x) &&
                                    lo.y <= static_cast<f32>(viewport.y) && lo.x <= hi.x && lo.y <= hi.y;
            i32 after = -1;
            if (on_surface) {
                for (i32 y = y0; y <= y1; ++y) {
                    for (i32 x = x0; x <= x1; ++x) {
                        after = std::max(after, latest[static_cast<usize>(y) * static_cast<usize>(columns) + static_cast<usize>(x)]);
                    }
                }
            }
            // The earliest group of this kind that is not before `after`; failing that, a new one at the end.
            vector<u32> &same_kind = groups_of_kind[kind_slot(entry.kind)];
            const auto found = std::ranges::lower_bound(same_kind, static_cast<u32>(std::max<i32>(after, 0)));
            u32 group = 0;
            if (barrier || found == same_kind.end()) {
                group = group_count++;
                same_kind.push_back(group);
            } else {
                group = *found;
            }
            entry.group = group;
            if (on_surface) {
                for (i32 y = y0; y <= y1; ++y) {
                    for (i32 x = x0; x <= x1; ++x) {
                        i32 &cell = latest[static_cast<usize>(y) * static_cast<usize>(columns) + static_cast<usize>(x)];
                        cell = std::max(cell, static_cast<i32>(group));
                    }
                }
            }
        }
        std::ranges::stable_sort(entries, {}, &PaintEntry::group);
    }

    void UiRenderer::destroy_effect_target(RHI::RhiDevice &device, FrameResources::EffectTarget &target) noexcept {
        for (const RHI::TextureViewHandle view : target.mip_views) {
            if (view) device.destroy_texture_view(view);
        }
        for (const auto &[key, view] : target.range_views) {
            if (view) device.destroy_texture_view(view);
        }
        if (target.view) device.destroy_texture_view(target.view);
        if (target.texture) device.destroy_texture(target.texture);
        target = {};
    }

    void UiRenderer::release_effect_targets(RHI::RhiDevice &device, FrameResources &resources) noexcept {
        for (auto &[key, target] : resources.effect_targets) {
            destroy_effect_target(device, target);
        }
        resources.effect_targets.clear();
    }

    namespace {

        /// Format a group's content is drawn in: half float, so a bright value survives to the effect's passes instead of clipping at 1.
        constexpr RHI::Format kEffectContentFormat = RHI::Format::RGBA16Float;
        /// Frames an effect target may go unused before its memory is given back.
        constexpr u64 kEffectTargetIdleFrames = 120;

        [[nodiscard]] Renderer::FullscreenSampler to_fullscreen_sampler(const UiEffectSampler &sampler) noexcept {
            const auto filter = [](UiEffectFilter value) noexcept {
                return value == UiEffectFilter::Nearest ? RHI::Filter::Nearest : RHI::Filter::Linear;
            };
            const auto address = [](UiEffectAddress value) noexcept {
                switch (value) {
                    case UiEffectAddress::Repeat: return RHI::AddressMode::Repeat;
                    case UiEffectAddress::Mirror: return RHI::AddressMode::MirroredRepeat;
                    case UiEffectAddress::Border: return RHI::AddressMode::ClampToBorder;
                    case UiEffectAddress::Clamp: break;
                }
                return RHI::AddressMode::ClampToEdge;
            };
            return Renderer::FullscreenSampler{
                .min_filter = filter(sampler.min_filter),
                .mag_filter = filter(sampler.mag_filter),
                .address_u = address(sampler.address_u),
                .address_v = address(sampler.address_v),
                .mip_lod_bias = sampler.lod_bias,
                .max_anisotropy = sampler.max_anisotropy > 1.0f ? sampler.max_anisotropy : 0.0f,
                .mipmap_mode = sampler.mip_filter == UiEffectFilter::Nearest ? RHI::MipmapMode::Nearest : RHI::MipmapMode::Linear,
                .min_lod = sampler.min_lod,
                .max_lod = sampler.max_lod,
            };
        }

    } // namespace

    Core::RendererResult UiRenderer::build_effect_groups(
        RHI::RhiDevice &device, RHI::CommandEncoder &encoder, Renderer::RenderGraph &render_graph, const FrameSnapshot &input,
        Renderer::Renderer *texture_resolver, Core::RenderSurfaceHandle surface, u32 frame_resource_index,
        FrameResources &frame_resources, vector<RHI::BufferHandle> &out_transient_buffers,
        Renderer::TextAtlasRetiredResources &out_retired_atlas_resources, vector<RHI::BindGroupHandle> &out_transient_bind_groups,
        vector<Renderer::RenderGraphTextureHandle> &out_glow_bloom_outputs, FrameSnapshot &out_effective,
        Renderer::RenderGraphTextureHandle backdrop_target, vector<RHI::CommandBufferHandle> *extra_command_buffers) {
        parallel_group_prepares_ = 0;
        // Layer markers (effects without passes) have nothing to render here.
        const vector<EffectGroupDraw> flat_groups = flatten_effect_groups(input.effect_groups());
        const std::span<const EffectGroupDraw> groups = flat_groups;
        const Core::Extent2D viewport = input.viewport_extent();
        if (viewport.x == 0 || viewport.y == 0) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "the UI surface has no area, so there is nothing for an effect to render into");
        }
        const glm::vec2 viewport_f{viewport};
        const auto error = [](string message) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, std::move(message));
        };

        // Top-level groups, in paint order. Nested groups belong to their top-level ancestor's child renderer.
        vector<usize> tops;
        for (usize i = 0; i < groups.size(); ++i) {
            if (groups[i].parent < 0 && groups[i].effect != nullptr) {
                tops.push_back(i);
            }
        }
        if (tops.empty()) {
            return error("an effect group has no effect");
        }
        // Which top-level group (index into `tops`) claims a draw with this paint order, or -1.
        const auto owner_of = [&](u32 order) -> i32 {
            auto it = std::upper_bound(tops.begin(), tops.end(), order,
                                       [&](u32 value, usize group) { return value < groups[group].paint.order; });
            if (it == tops.begin()) {
                return -1;
            }
            --it;
            const EffectGroupDraw &candidate = groups[*it];
            return order > candidate.paint.order && order < candidate.last_order ? static_cast<i32>(it - tops.begin()) : -1;
        };

        // Split every draw between the frame itself and the group that claims it.
        vector<FrameSnapshot> subs(tops.size());
        out_effective.full_viewport_scissor_ = input.full_viewport_scissor_;
        for (FrameSnapshot &sub : subs) {
            sub.full_viewport_scissor_ = input.full_viewport_scissor_;
        }
        const auto target_of = [&](u32 order) -> FrameSnapshot & {
            const i32 owner = owner_of(order);
            return owner < 0 ? out_effective : subs[static_cast<usize>(owner)];
        };
        for (const QuadDraw &draw : input.quads_) target_of(draw.paint.order).quads_.push_back(draw);
        for (const StrokeDraw &draw : input.strokes_) target_of(draw.paint.order).strokes_.push_back(draw);
        for (const FillQuadDraw &draw : input.fills_) target_of(draw.paint.order).fills_.push_back(draw);
        for (const SectorDraw &draw : input.sectors_) target_of(draw.paint.order).sectors_.push_back(draw);
        for (const CustomDraw &draw : input.custom_draws_) target_of(draw.paint.order).custom_draws_.push_back(draw);
        for (const CustomStrokeDraw &draw : input.custom_strokes_) target_of(draw.paint.order).custom_strokes_.push_back(draw);
        for (usize i = 0; i < input.glyphs_.size(); ++i) {
            FrameSnapshot &target = target_of(input.glyph_paint_[i].order);
            target.glyphs_.push_back(input.glyphs_[i]);
            target.glyph_scissors_.push_back(input.glyph_scissors_[i]);
            target.glyph_paint_.push_back(input.glyph_paint_[i]);
        }
        // Groups nested inside a top-level one go with it, re-parented so the child sees them as its own top level.
        for (usize t = 0; t < tops.size(); ++t) {
            const EffectGroupDraw &top = groups[tops[t]];
            vector<i32> remapped(groups.size(), -1);
            for (usize g = 0; g < groups.size(); ++g) {
                const EffectGroupDraw &nested = groups[g];
                if (nested.parent < 0 || nested.paint.order <= top.paint.order || nested.last_order >= top.last_order) {
                    continue;
                }
                EffectGroupDraw copy = nested;
                copy.parent = static_cast<usize>(nested.parent) == tops[t] ? -1 : remapped[static_cast<usize>(nested.parent)];
                remapped[g] = static_cast<i32>(subs[t].effect_groups_.size());
                subs[t].effect_groups_.push_back(copy);
            }
        }

        ++frame_resources.effect_frame;
        // Levels in a full chain for `extent`, and the number a request for `requested` (0 = full chain) resolves to.
        const auto full_chain = [](Core::Extent2D extent) {
            u32 levels = 1;
            for (u32 size = std::max(extent.x, extent.y); size > 1; size >>= 1u) {
                ++levels;
            }
            return levels;
        };
        const auto resolve_levels = [&](u32 requested, Core::Extent2D extent) {
            const u32 full = full_chain(extent);
            return requested == 0 ? full : std::min(requested, full);
        };
        const auto ensure_target = [&](u64 key, RHI::Format format, Core::Extent2D extent, u32 mip_request,
                                       const char *label) -> Core::RendererExpected<FrameResources::EffectTarget *> {
            FrameResources::EffectTarget &target = frame_resources.effect_targets[key];
            const u32 levels = resolve_levels(mip_request, extent);
            if (!target.texture || target.format != format || target.extent.x != extent.x || target.extent.y != extent.y ||
                target.mip_levels != levels) {
                destroy_effect_target(device, target);
                auto texture = device.create_texture(RHI::TextureDesc{
                    .dimension = RHI::TextureDimension::Dim2D,
                    .format = format,
                    .extent = RHI::Extent3D{.width = extent.x, .height = extent.y, .depth_or_layers = 1},
                    .mip_levels = levels,
                    .samples = RHI::SampleCount::X1,
                    .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                    .label = label,
                });
                if (!texture) {
                    return unexpected(Renderer::graphics_error_from_rhi(texture.error(), "create a UI effect target texture"));
                }
                target.texture = *texture;
                auto view = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = target.texture, .view_type = RHI::TextureViewType::View2D, .format = format, .label = label});
                if (!view) {
                    destroy_effect_target(device, target);
                    return unexpected(Renderer::graphics_error_from_rhi(view.error(), "create a UI effect target texture view"));
                }
                target.view = *view;
                if (levels > 1) {
                    for (u32 level = 0; level < levels; ++level) {
                        auto mip_view = device.create_texture_view(RHI::TextureViewDesc{.texture = target.texture,
                                                                                         .view_type = RHI::TextureViewType::View2D,
                                                                                         .format = format,
                                                                                         .base_mip_level = level,
                                                                                         .mip_level_count = 1,
                                                                                         .label = label});
                        if (!mip_view) {
                            destroy_effect_target(device, target);
                            return unexpected(Renderer::graphics_error_from_rhi(mip_view.error(), "create a UI effect mip view"));
                        }
                        target.mip_views.push_back(*mip_view);
                    }
                }
                target.format = format;
                target.extent = extent;
                target.mip_levels = levels;
                target.revision = next_effect_revision_++;
            }
            target.last_used_frame = frame_resources.effect_frame;
            return &target;
        };
        const auto import_target = [&](const FrameResources::EffectTarget &target, const char *label) {
            return render_graph.import_texture(Renderer::RenderGraphImportedTextureDesc{
                .texture = target.texture,
                .default_view = target.view,
                .format = target.format,
                .extent = RHI::Extent3D{.width = target.extent.x, .height = target.extent.y, .depth_or_layers = 1},
                .mip_levels = target.mip_levels,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = label,
            });
        };
        // The view a shader sees for levels [base, base + count) of `target` (count 0 = all the rest): the whole-chain view when that is
        // everything, a cached view otherwise.
        const auto view_for = [&](FrameResources::EffectTarget &target, u32 base, u32 count) -> Core::RendererExpected<RHI::TextureViewHandle> {
            const u32 first = std::min(base, target.mip_levels - 1);
            const u32 available = target.mip_levels - first;
            const u32 span = count == 0 ? available : std::min(count, available);
            if (first == 0 && span == target.mip_levels) {
                return target.view;
            }
            if (span == 1 && !target.mip_views.empty()) {
                return target.mip_views[first];
            }
            const u32 key = (first << 8u) | span;
            if (const auto found = target.range_views.find(key); found != target.range_views.end()) {
                return found->second;
            }
            auto view = device.create_texture_view(RHI::TextureViewDesc{.texture = target.texture,
                                                                         .view_type = RHI::TextureViewType::View2D,
                                                                         .format = target.format,
                                                                         .base_mip_level = first,
                                                                         .mip_level_count = span,
                                                                         .label = "ui effect mip range"});
            if (!view) {
                return unexpected(Renderer::graphics_error_from_rhi(view.error(), "create a UI effect mip-range view"));
            }
            target.range_views.emplace(key, *view);
            return *view;
        };
        // What a pass that draws level 0 of `target` attaches: for a chain, the level-0 view and subresource, for a plain image the whole thing.
        const auto attachment_for = [&](Renderer::RenderGraphTextureHandle handle, const FrameResources::EffectTarget &target) {
            Renderer::RenderGraphColorAttachmentDesc attachment{
                .texture = handle,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            };
            if (target.mip_levels > 1) {
                attachment.view = target.mip_views[0];
                attachment.subresources = RHI::TextureSubresourceRange{.base_mip_level = 0, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1};
            }
            return attachment;
        };
        // Makes every level after the first from the one above it: one fullscreen pass per level, level N sampled and level N+1 attached.
        const auto add_mip_passes = [&](Renderer::RenderGraphTextureHandle handle, FrameResources::EffectTarget &target, UiMipFilter filter,
                                        const std::string &name) -> Core::RendererResult {
            for (u32 level = 1; level < target.mip_levels; ++level) {
                const Core::Extent2D source_extent{std::max(1u, target.extent.x >> (level - 1u)), std::max(1u, target.extent.y >> (level - 1u))};
                const Core::Extent2D level_extent{std::max(1u, target.extent.x >> level), std::max(1u, target.extent.y >> level)};
                const UiEffectConstants constants{
                    .target_size = glm::vec2{level_extent},
                    .content_size = glm::vec2{target.extent},
                    .pass_index = level,
                };
                Renderer::CustomPostProcessEffect fullscreen;
                fullscreen.shader_path = "Shaders/ui_effect_downsample.slang";
                fullscreen.module_name = "ui_effect_downsample";
                fullscreen.label = UString{"ui effect mip " + std::to_string(level)};
                fullscreen.push_constants.resize(sizeof(UiEffectConstants) + sizeof(glm::vec4));
                std::memcpy(fullscreen.push_constants.data(), &constants, sizeof(UiEffectConstants));
                const glm::vec4 mode{filter == UiMipFilter::Tent ? 1.0f : 0.0f, 1.0f / static_cast<f32>(source_extent.x),
                                     1.0f / static_cast<f32>(source_extent.y), 0.0f};
                std::memcpy(fullscreen.push_constants.data() + sizeof(UiEffectConstants), &mode, sizeof(glm::vec4));
                if (Core::RendererResult ready = texture_resolver->prepare_fullscreen_effect(fullscreen, target.format); !ready) {
                    return unexpected(Core::GraphicsBackendError{ready.error().code, "mip generation for " + name + ": " + ready.error().message});
                }
                const UString pass_label{"ui effect mip " + std::to_string(level) + ": " + name};
                auto &builder = render_graph.add_render_pass(pass_label);
                builder.add_color_attachment(Renderer::RenderGraphColorAttachmentDesc{
                    .texture = handle,
                    .view = target.mip_views[level],
                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = level, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                });
                builder.add_sampled_texture(Renderer::RenderGraphSampledTextureReadDesc{
                    .texture = handle,
                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = level - 1, .mip_level_count = 1, .base_array_layer = 0, .array_layer_count = 1},
                });
                builder.set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = level_extent.x, .height = level_extent.y})
                    .set_execute([resolver = texture_resolver, &out_transient_bind_groups, source_view = target.mip_views[level - 1], level_extent,
                                  format = target.format, fullscreen = std::move(fullscreen)](Renderer::RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass_encoder = context.render_pass();
                        pass_encoder.set_viewport(RHI::Viewport{.width = static_cast<f32>(level_extent.x),
                                                                .height = static_cast<f32>(level_extent.y),
                                                                .min_depth = 0.0f,
                                                                .max_depth = 1.0f});
                        pass_encoder.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = level_extent.x, .height = level_extent.y});
                        return resolver->record_fullscreen_effect(pass_encoder, source_view, format, fullscreen, out_transient_bind_groups);
                    });
            }
            return {};
        };
        // An application texture read by several passes is imported into the graph once.
        std::unordered_map<u64, Renderer::RenderGraphTextureHandle> imported_app_textures;

        struct DeferredPrepare {
            UiRenderer *child = nullptr;
            const FrameSnapshot *sub = nullptr;
            std::string name;
        };
        vector<DeferredPrepare> deferred_prepares;

        for (usize t = 0; t < tops.size(); ++t) {
            const EffectGroupDraw &group = groups[tops[t]];
            const UiEffect &effect = *group.effect;

            // 0. The region this group renders into: its box plus the effect's outset (the whole surface when the effect is not clipped to
            // its bounds). Targets are that size, not the surface's, so a small widget's effect costs a small widget's memory; the group's
            // draws are moved into the region's own coordinates, and `region_origin` puts the result back where it belongs.
            const f32 scale = group.pixel_scale;
            glm::vec2 region_lo = group.position - glm::vec2{effect.outset.left, effect.outset.top} * scale;
            glm::vec2 region_hi = group.position + group.size + glm::vec2{effect.outset.right, effect.outset.bottom} * scale;
            if (!effect.clip_to_bounds) {
                region_lo = glm::vec2{0.0f};
                region_hi = viewport_f;
            }
            region_lo = glm::clamp(region_lo, glm::vec2{0.0f}, viewport_f);
            region_hi = glm::clamp(region_hi, glm::vec2{0.0f}, viewport_f);
            const i32 region_x0 = static_cast<i32>(std::floor(region_lo.x));
            const i32 region_y0 = static_cast<i32>(std::floor(region_lo.y));
            const i32 region_x1 = static_cast<i32>(std::ceil(region_hi.x));
            const i32 region_y1 = static_cast<i32>(std::ceil(region_hi.y));
            if (region_x1 <= region_x0 || region_y1 <= region_y0) {
                continue; // nothing of the group can be on the surface; its draws were claimed, so it draws nothing
            }
            const Core::Extent2D region{static_cast<u32>(region_x1 - region_x0), static_cast<u32>(region_y1 - region_y0)};
            const glm::vec2 region_f{region};
            const glm::vec2 region_origin{static_cast<f32>(region_x0), static_cast<f32>(region_y0)};
            {
                FrameSnapshot &sub = subs[t];
                const auto shift_scissor = [&](RHI::Rect2D &rect) {
                    const i32 x0 = std::max<i32>(rect.x - region_x0, 0);
                    const i32 y0 = std::max<i32>(rect.y - region_y0, 0);
                    const i32 x1 = std::min<i32>(rect.x - region_x0 + static_cast<i32>(rect.width), static_cast<i32>(region.x));
                    const i32 y1 = std::min<i32>(rect.y - region_y0 + static_cast<i32>(rect.height), static_cast<i32>(region.y));
                    rect = RHI::Rect2D{.x = x0, .y = y0, .width = static_cast<u32>(std::max(x1 - x0, 0)), .height = static_cast<u32>(std::max(y1 - y0, 0))};
                };
                for (QuadDraw &draw : sub.quads_) {
                    draw.instance.position -= region_origin;
                    shift_scissor(draw.scissor);
                }
                for (StrokeDraw &draw : sub.strokes_) {
                    for (StrokePath &path : draw.paths) {
                        for (glm::vec2 &point : path.points) point -= region_origin;
                    }
                    shift_scissor(draw.scissor);
                }
                for (FillQuadDraw &draw : sub.fills_) {
                    for (FillQuad &quad : draw.quads) quad.position -= region_origin;
                    shift_scissor(draw.scissor);
                }
                for (SectorDraw &draw : sub.sectors_) {
                    for (Sector &sector : draw.sectors) sector.center -= region_origin;
                    shift_scissor(draw.scissor);
                }
                for (CustomDraw &draw : sub.custom_draws_) {
                    draw.position -= region_origin;
                    shift_scissor(draw.scissor);
                }
                for (CustomStrokeDraw &draw : sub.custom_strokes_) {
                    for (glm::vec2 &point : draw.points) point -= region_origin;
                    shift_scissor(draw.scissor);
                }
                for (Renderer::GlyphPlacement &glyph : sub.glyphs_) glyph.position -= region_origin;
                for (RHI::Rect2D &rect : sub.glyph_scissors_) shift_scissor(rect);
                for (EffectGroupDraw &nested : sub.effect_groups_) {
                    nested.position -= region_origin;
                    shift_scissor(nested.scissor);
                }
                sub.full_viewport_scissor_ = RHI::Rect2D{.x = 0, .y = 0, .width = region.x, .height = region.y};
            }

            // 1. The group's content, drawn by a child renderer.
            if (effect_children_.size() <= t) {
                effect_children_.resize(t + 1);
            }
            if (!effect_children_[t]) {
                auto child = UiRenderer::create(device, kEffectContentFormat, enable_shader_disk_cache_);
                if (!child) {
                    return unexpected(child.error());
                }
                effect_children_[t] = std::make_unique<UiRenderer>(std::move(*child));
            }
            UiRenderer *child = effect_children_[t].get();
            // A group whose content needs nothing from the render graph (no nested groups, no glow) can be prepared on a worker with an
            // encoder of its own; those are collected and run together after the loop. Anything else is prepared right here.
            const auto needs_graph = [](const FrameSnapshot &sub) {
                if (!sub.effect_groups_.empty()) return true;
                for (const StrokeDraw &draw : sub.strokes_) {
                    for (const StrokePath &path : draw.paths) {
                        if (path.style.glow_intensity > 0.0f) return true;
                    }
                }
                return false;
            };
            if (extra_command_buffers != nullptr && !needs_graph(subs[t])) {
                deferred_prepares.push_back(DeferredPrepare{.child = child, .sub = &subs[t], .name = effect.name.cpp_string()});
            } else if (Core::RendererResult prepared = child->prepare(device, encoder, render_graph, subs[t], texture_resolver, surface,
                                                                      frame_resource_index, out_transient_buffers, out_retired_atlas_resources,
                                                                      out_transient_bind_groups, out_glow_bloom_outputs);
                       !prepared) {
                return unexpected(Core::GraphicsBackendError{prepared.error().code,
                                                             "effect '" + effect.name.cpp_string() + "' content: " + prepared.error().message});
            }
            auto content_target = ensure_target(effect_target_key(t, 0), kEffectContentFormat, region, effect.content.mip_levels, "ui effect content");
            if (!content_target) {
                return unexpected(content_target.error());
            }
            FrameResources::EffectTarget &content_image = **content_target;
            const Renderer::RenderGraphTextureHandle content = import_target(content_image, "ui effect content");
            const UString content_label{"ui effect content: " + effect.name.cpp_string()};
            render_graph.add_render_pass(content_label)
                .add_color_attachment(attachment_for(content, content_image))
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = region.x, .height = region.y})
                .set_execute([child, region_f, surface, frame_resource_index](Renderer::RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{.width = region_f.x, .height = region_f.y, .min_depth = 0.0f, .max_depth = 1.0f});
                    return child->draw(pass, region_f, surface, frame_resource_index);
                });
            if (content_image.mip_levels > 1) {
                if (Core::RendererResult mips = add_mip_passes(content, content_image, effect.content.mip_filter, "content of " + effect.name.cpp_string());
                    !mips) {
                    return mips;
                }
            }

            // The backdrop, captured on first use by a pass of this group.
            FrameResources::EffectTarget *backdrop_image = nullptr;
            Renderer::RenderGraphTextureHandle backdrop_copy{};
            const auto ensure_backdrop = [&]() -> Core::RendererResult {
                if (backdrop_image != nullptr) {
                    return {};
                }
                if (!static_cast<bool>(backdrop_target)) {
                    return error("effect '" + effect.name.cpp_string() + "' reads the backdrop, but this UI has none (nested group or no target)");
                }
                auto image = ensure_target(effect_target_key(t, 0xFFFF), kEffectContentFormat, region, 1, "ui effect backdrop");
                if (!image) {
                    return unexpected(image.error());
                }
                backdrop_image = *image;
                backdrop_copy = import_target(*backdrop_image, "ui effect backdrop");
                Renderer::CustomPostProcessEffect capture;
                capture.shader_path = "Shaders/ui_effect_backdrop.slang";
                capture.module_name = "ui_effect_backdrop";
                capture.fragment_entry_point = "fragmentMain";
                capture.label = UString{"ui effect backdrop"};
                capture.blend = Renderer::FullscreenBlend::None;
                capture.sampler.min_filter = RHI::Filter::Nearest;
                capture.sampler.mag_filter = RHI::Filter::Nearest;
                const UiEffectConstants constants{.target_size = region_f, .content_size = region_f};
                const glm::vec4 area{region_origin, viewport_f};
                capture.push_constants.resize(sizeof(UiEffectConstants) + sizeof(glm::vec4));
                std::memcpy(capture.push_constants.data(), &constants, sizeof(UiEffectConstants));
                std::memcpy(capture.push_constants.data() + sizeof(UiEffectConstants), &area, sizeof(glm::vec4));
                if (Core::RendererResult ready = texture_resolver->prepare_fullscreen_effect(capture, kEffectContentFormat); !ready) {
                    return unexpected(Core::GraphicsBackendError{ready.error().code, "effect backdrop capture: " + ready.error().message});
                }
                const UString capture_label{"ui effect backdrop: " + effect.name.cpp_string()};
                render_graph.add_render_pass(capture_label)
                    .add_color_attachment(attachment_for(backdrop_copy, *backdrop_image))
                    .add_sampled_texture(Renderer::RenderGraphSampledTextureReadDesc{.texture = backdrop_target})
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = region.x, .height = region.y})
                    .set_execute([resolver = texture_resolver, &out_transient_bind_groups, backdrop_target, region, region_f,
                                  capture = std::move(capture)](Renderer::RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass_encoder = context.render_pass();
                        pass_encoder.set_viewport(RHI::Viewport{.width = region_f.x, .height = region_f.y, .min_depth = 0.0f, .max_depth = 1.0f});
                        pass_encoder.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = region.x, .height = region.y});
                        const Renderer::RenderGraphTextureAccess source = context.texture(backdrop_target);
                        return resolver->record_fullscreen_effect(pass_encoder, source.default_view, kEffectContentFormat, capture,
                                                                  out_transient_bind_groups, {});
                    });
                return {};
            };

            // 2. The effect's passes, each a fullscreen draw of the user's fragment shader.
            vector<Renderer::RenderGraphTextureHandle> pass_outputs(effect.passes.size());
            vector<FrameResources::EffectTarget *> pass_targets(effect.passes.size(), nullptr);
            FrameResources::EffectTarget *last_target = nullptr;
            for (usize i = 0; i < effect.passes.size(); ++i) {
                const UiEffectPass &pass = effect.passes[i];
                const std::string where = "effect '" + effect.name.cpp_string() + "' pass " + std::to_string(i) +
                                          (pass.label.empty() ? std::string{} : " ('" + pass.label.cpp_string() + "')");
                const Core::Extent2D extent{
                    std::max<u32>(1u, static_cast<u32>(std::lround(region_f.x * pass.output.scale))),
                    std::max<u32>(1u, static_cast<u32>(std::lround(region_f.y * pass.output.scale)))};
                auto target = ensure_target(effect_target_key(t, 1 + i), pass.output.format, extent, pass.output.mip_levels, "ui effect pass");
                if (!target) {
                    return unexpected(target.error());
                }
                last_target = *target;
                pass_targets[i] = *target;
                const Renderer::RenderGraphTextureHandle output = import_target(**target, "ui effect pass");
                pass_outputs[i] = output;

                // What each input reads: the graph's handle (for ordering and layout), the exact view the shader gets, and the part of
                // the image the pass declares it reads.
                vector<Renderer::RenderGraphSampledTextureReadDesc> reads;
                vector<RHI::TextureViewHandle> source_views;
                const auto mip_range = [](u32 base, u32 count) {
                    return RHI::TextureSubresourceRange{.base_mip_level = base, .mip_level_count = count == 0 ? RHI::all_remaining : count};
                };
                const auto add_chain_source = [&](Renderer::RenderGraphTextureHandle handle, FrameResources::EffectTarget &image,
                                                  const UiEffectInput &desc) -> Core::RendererResult {
                    auto view = view_for(image, desc.base_mip, desc.mip_count);
                    if (!view) {
                        return unexpected(view.error());
                    }
                    reads.push_back(Renderer::RenderGraphSampledTextureReadDesc{.texture = handle, .subresources = mip_range(desc.base_mip, desc.mip_count)});
                    source_views.push_back(*view);
                    return {};
                };
                if (pass.inputs.empty()) {
                    if (Core::RendererResult added = add_chain_source(content, content_image, UiEffectInput{}); !added) {
                        return added;
                    }
                }
                for (usize k = 0; k < pass.inputs.size(); ++k) {
                    const UiEffectInput &input_desc = pass.inputs[k];
                    switch (input_desc.kind) {
                        case UiEffectInputKind::GroupContent:
                            if (Core::RendererResult added = add_chain_source(content, content_image, input_desc); !added) {
                                return added;
                            }
                            break;
                        case UiEffectInputKind::PassOutput:
                            if (input_desc.pass >= i) {
                                return error(where + " input " + std::to_string(k) + " reads a pass that has not run");
                            }
                            if (Core::RendererResult added = add_chain_source(pass_outputs[input_desc.pass], *pass_targets[input_desc.pass], input_desc);
                                !added) {
                                return added;
                            }
                            break;
                        case UiEffectInputKind::Backdrop:
                            if (Core::RendererResult ready = ensure_backdrop(); !ready) {
                                return ready;
                            }
                            if (Core::RendererResult added = add_chain_source(backdrop_copy, *backdrop_image, input_desc); !added) {
                                return added;
                            }
                            break;
                        case UiEffectInputKind::Texture: {
                            Renderer::TextureResource *resource = texture_resolver->texture(input_desc.texture);
                            if (resource == nullptr || !resource->view) {
                                return error(where + " input " + std::to_string(k) + " names a texture that no longer exists");
                            }
                            auto found = imported_app_textures.find(input_desc.texture.value);
                            if (found == imported_app_textures.end()) {
                                found = imported_app_textures
                                            .emplace(input_desc.texture.value,
                                                     render_graph.import_texture(Renderer::RenderGraphImportedTextureDesc{
                                                         .texture = resource->texture,
                                                         .default_view = resource->view,
                                                         .format = resource->format,
                                                         .extent = RHI::Extent3D{.width = resource->width, .height = resource->height, .depth_or_layers = 1},
                                                         .mip_levels = resource->mip_levels,
                                                         .samples = RHI::SampleCount::X1,
                                                         .usage = RHI::TextureUsage::Sampled,
                                                         .initial_layout = RHI::TextureLayout::ShaderReadOnly,
                                                         .initial_stage = RHI::PipelineStage::FragmentShader,
                                                         .initial_access = RHI::AccessFlags::ShaderRead,
                                                         .final_layout = RHI::TextureLayout::ShaderReadOnly,
                                                         .final_stage = RHI::PipelineStage::FragmentShader,
                                                         .final_access = RHI::AccessFlags::ShaderRead,
                                                         .label = "ui effect app texture",
                                                     }))
                                            .first;
                            }
                            reads.push_back(Renderer::RenderGraphSampledTextureReadDesc{.texture = found->second});
                            source_views.push_back(resource->view);
                            break;
                        }
                    }
                }

                const UiEffectConstants constants{
                    .target_size = glm::vec2{extent},
                    .content_size = region_f,
                    .group_origin = group.position - region_origin,
                    .group_size = group.size,
                    .pass_index = static_cast<u32>(i),
                    .frame_index = static_cast<u32>(frame_resources.effect_frame),
                };
                Renderer::CustomPostProcessEffect fullscreen;
                fullscreen.shader_path = pass.shader_path.cpp_string();
                fullscreen.module_name = pass.module_name.cpp_string();
                fullscreen.fragment_entry_point = pass.fragment_entry_point.cpp_string();
                fullscreen.label = UString{where};
                fullscreen.extra_input_count = static_cast<u32>(source_views.size() - 1);
                fullscreen.blend = Renderer::FullscreenBlend::None;
                fullscreen.sampler = to_fullscreen_sampler(pass.sampler);
                fullscreen.push_constants.resize(sizeof(UiEffectConstants) + pass.parameters.size());
                std::memcpy(fullscreen.push_constants.data(), &constants, sizeof(UiEffectConstants));
                if (!pass.parameters.empty()) {
                    std::memcpy(fullscreen.push_constants.data() + sizeof(UiEffectConstants), pass.parameters.data(), pass.parameters.size());
                }
                // Compiles the shader and builds its pipeline now (and once), so a broken effect fails here with the compiler's reason
                // rather than as a black rectangle.
                if (Core::RendererResult ready = texture_resolver->prepare_fullscreen_effect(fullscreen, pass.output.format); !ready) {
                    return unexpected(Core::GraphicsBackendError{ready.error().code, where + ": " + ready.error().message});
                }
                const UString pass_label{"ui effect: " + where};
                auto &builder = render_graph.add_render_pass(pass_label);
                builder.add_color_attachment(attachment_for(output, **target));
                for (const Renderer::RenderGraphSampledTextureReadDesc &read : reads) {
                    builder.add_sampled_texture(read);
                }
                builder.set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
                    .set_execute([resolver = texture_resolver, &out_transient_bind_groups, source_views, extent, format = pass.output.format,
                                  fullscreen = std::move(fullscreen)](Renderer::RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass_encoder = context.render_pass();
                        pass_encoder.set_viewport(RHI::Viewport{.width = static_cast<f32>(extent.x),
                                                                .height = static_cast<f32>(extent.y),
                                                                .min_depth = 0.0f,
                                                                .max_depth = 1.0f});
                        pass_encoder.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y});
                        return resolver->record_fullscreen_effect(pass_encoder, source_views.front(), format, fullscreen, out_transient_bind_groups,
                                                                  span<const RHI::TextureViewHandle>{source_views.data() + 1, source_views.size() - 1});
                    });
                if ((*target)->mip_levels > 1) {
                    if (Core::RendererResult mips = add_mip_passes(output, **target, pass.output.mip_filter, where); !mips) {
                        return mips;
                    }
                }
            }
            out_glow_bloom_outputs.push_back(pass_outputs.back());

            // 3. The composite: the last pass's image over the group's region, as a premultiplied quad at the group's place in paint order.
            if (effect.opacity <= 0.0f || last_target == nullptr) {
                continue;
            }
            const i32 x0 = std::max<i32>(group.scissor.x, region_x0);
            const i32 y0 = std::max<i32>(group.scissor.y, region_y0);
            const i32 x1 = std::min<i32>(group.scissor.x + static_cast<i32>(group.scissor.width), region_x1);
            const i32 y1 = std::min<i32>(group.scissor.y + static_cast<i32>(group.scissor.height), region_y1);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            out_effective.quads_.push_back(QuadDraw{
                .instance = UiQuadInstance{
                    .position = region_origin,
                    .size = region_f,
                    .fill_color = {1.0f, 1.0f, 1.0f, effect.opacity},
                    .uv_min = {0.0f, 0.0f},
                    .uv_max = {1.0f, 1.0f},
                    // Additive and multiply want the colour premultiplied going into the blend; normal and replace want it straight.
                    .kind = static_cast<f32>(effect.composite == UiEffectBlend::Additive || effect.composite == UiEffectBlend::Multiply
                                                 ? UiQuadKind::ImagePremultipliedOut
                                                 : UiQuadKind::ImagePremultiplied),
                },
                .image_ref = nullptr,
                .scissor = RHI::Rect2D{.x = x0, .y = y0, .width = static_cast<u32>(x1 - x0), .height = static_cast<u32>(y1 - y0)},
                .paint = group.paint,
                .view_override = last_target->view,
                .view_revision = last_target->revision,
                .blend = effect.composite,
            });
        }

        // The deferred content preparations: on workers, each with an encoder of its own, when there are several; inline when there is one.
        if (!deferred_prepares.empty()) {
            const auto failure = [](const DeferredPrepare &job, const Core::GraphicsBackendError &error) {
                return unexpected(Core::GraphicsBackendError{error.code, "effect '" + job.name + "' content: " + error.message});
            };
            if (deferred_prepares.size() == 1) {
                const DeferredPrepare &job = deferred_prepares.front();
                if (Core::RendererResult prepared = job.child->prepare(device, encoder, render_graph, *job.sub, texture_resolver, surface,
                                                                       frame_resource_index, out_transient_buffers, out_retired_atlas_resources,
                                                                       out_transient_bind_groups, out_glow_bloom_outputs);
                    !prepared) {
                    return failure(job, prepared.error());
                }
            } else {
                parallel_group_prepares_ += static_cast<u32>(deferred_prepares.size());
                struct Outcome {
                    Core::RendererResult status{};
                    unique_ptr<RHI::CommandEncoder> encoder;
                    vector<RHI::BufferHandle> buffers;
                    Renderer::TextAtlasRetiredResources retired;
                    vector<RHI::BindGroupHandle> bind_groups;
                    vector<Renderer::RenderGraphTextureHandle> glow;
                };
                vector<Outcome> outcomes(deferred_prepares.size());
                for (Outcome &outcome : outcomes) {
                    auto made = device.create_command_encoder(RHI::CommandEncoderDesc{.label = "ui effect content prepare"});
                    if (!made) {
                        return error("could not create a command encoder for an effect group's content: " + made.error().message);
                    }
                    outcome.encoder = std::move(*made);
                }
                Async::par_for(deferred_prepares.size(), [&](usize k) {
                    outcomes[k].status = deferred_prepares[k].child->prepare(
                        device, *outcomes[k].encoder, render_graph, *deferred_prepares[k].sub, texture_resolver, surface, frame_resource_index,
                        outcomes[k].buffers, outcomes[k].retired, outcomes[k].bind_groups, outcomes[k].glow);
                });
                // Back on one thread: hand over what each recorded and created, in group order, and report the first failure.
                std::optional<Core::GraphicsBackendError> first_error;
                for (usize k = 0; k < outcomes.size(); ++k) {
                    Outcome &outcome = outcomes[k];
                    out_transient_buffers.insert(out_transient_buffers.end(), outcome.buffers.begin(), outcome.buffers.end());
                    out_transient_bind_groups.insert(out_transient_bind_groups.end(), outcome.bind_groups.begin(), outcome.bind_groups.end());
                    out_retired_atlas_resources.textures.insert(out_retired_atlas_resources.textures.end(), outcome.retired.textures.begin(),
                                                                outcome.retired.textures.end());
                    out_retired_atlas_resources.texture_views.insert(out_retired_atlas_resources.texture_views.end(),
                                                                     outcome.retired.texture_views.begin(), outcome.retired.texture_views.end());
                    out_glow_bloom_outputs.insert(out_glow_bloom_outputs.end(), outcome.glow.begin(), outcome.glow.end());
                    auto finished = outcome.encoder->finish();
                    if (finished) {
                        extra_command_buffers->push_back(*finished); // the renderer owns it from here, and frees it if the frame fails
                    } else if (!first_error) {
                        first_error = Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                                 "could not finish an effect group's upload commands: " + finished.error().message};
                    }
                    if (!outcome.status && !first_error) {
                        first_error = outcome.status.error();
                        first_error->message = "effect '" + deferred_prepares[k].name + "' content: " + first_error->message;
                    }
                }
                if (first_error) {
                    return unexpected(*first_error);
                }
            }
        }
        return {};
    }

    void UiRenderer::release_glow_targets(RHI::RhiDevice &device, FrameResources &resources) noexcept {
        if (resources.glow_mask_view) device.destroy_texture_view(resources.glow_mask_view);
        if (resources.glow_mask_texture) device.destroy_texture(resources.glow_mask_texture);
        if (resources.glow_bloom_view) device.destroy_texture_view(resources.glow_bloom_view);
        if (resources.glow_bloom_texture) device.destroy_texture(resources.glow_bloom_texture);
        resources.glow_mask_texture = {};
        resources.glow_mask_view = {};
        resources.glow_bloom_texture = {};
        resources.glow_bloom_view = {};
        resources.glow_extent = {};
        resources.glow_idle_frames = 0;
        // Its bind group points at the bloom view that was just destroyed.
        destroy_ui_quad_frame_resources(device, resources.glow_composite_quad);
    }

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
                                             const FrameSnapshot &input_snapshot, Renderer::Renderer *texture_resolver,
                                             Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                             vector<RHI::BufferHandle> &out_transient_buffers,
                                             Renderer::TextAtlasRetiredResources &out_retired_atlas_resources,
                                             vector<RHI::BindGroupHandle> &out_transient_bind_groups,
                                             vector<Renderer::RenderGraphTextureHandle> &out_glow_bloom_outputs,
                                             Renderer::RenderGraphTextureHandle backdrop_target,
                                             vector<RHI::CommandBufferHandle> *extra_command_buffers) {
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
        // The glow mask and bloom targets stay alive from frame to frame: a frame that draws glow reuses them (the render
        // graph imports them as Undefined, so no content is carried over), and the (surface, frame_resource_index)
        // slot being reused is itself the engine's signal that this slot's earlier GPU work has finished, so remaking or
        // releasing them here is safe. They are let go after a long enough stretch without any glow.
        frame_resources.glow_mask_stroke_batches.clear();
        if (frame_resources.glow_mask_texture && ++frame_resources.glow_idle_frames > kGlowIdleFramesBeforeRelease) {
            release_glow_targets(device, frame_resources);
        }
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
        // Effect groups (ElementDecl::effect) are rendered first, into their own images; what is prepared below is then the frame without
        // the claimed draws and with one composite quad per group. A group that cannot be rendered is drawn plainly, with the reason
        // logged, rather than failing the frame.
        FrameSnapshot effective;
        const FrameSnapshot *active_snapshot = &input_snapshot;
        const bool has_effect_groups = std::ranges::any_of(
            input_snapshot.effect_groups_, [](const EffectGroupDraw &group) { return group.effect != nullptr && !group.effect->passes.empty(); });
        if (has_effect_groups) {
            Core::RendererResult built = build_effect_groups(device, encoder, render_graph, input_snapshot, texture_resolver, surface,
                                                             frame_resource_index, frame_resources, out_transient_buffers,
                                                             out_retired_atlas_resources, out_transient_bind_groups,
                                                             out_glow_bloom_outputs, effective, backdrop_target,
                                                             extra_command_buffers);
            if (built) {
                active_snapshot = &effective;
            } else {
                SFT_LOG_ERROR_THROTTLED(5000, "UI effect groups could not be rendered, their elements are drawn without the effect: {}",
                                        built.error().message);
            }
        }
        const FrameSnapshot &snapshot = *active_snapshot;
        if (!frame_resources.effect_targets.empty()) {
            // Give back the memory of targets no effect has used for a while (an effect that was removed, or resized away).
            if (!has_effect_groups) {
                ++frame_resources.effect_frame;
            }
            for (auto it = frame_resources.effect_targets.begin(); it != frame_resources.effect_targets.end();) {
                if (it->second.last_used_frame + kEffectTargetIdleFrames < frame_resources.effect_frame) {
                    destroy_effect_target(device, it->second);
                    it = frame_resources.effect_targets.erase(it);
                } else {
                    ++it;
                }
            }
        }

        const Core::Extent2D viewport_extent = snapshot.viewport_extent();
        const RHI::Rect2D full_viewport_scissor{.x = 0, .y = 0, .width = viewport_extent.x, .height = viewport_extent.y};

        PrepareScratch &scratch = scratch_;
        vector<PaintEntry> &entries = scratch.entries;
        entries.clear();
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


        auto &quad_instances = scratch.quad_instances;
        auto &quad_texture_views = scratch.quad_texture_views;
        auto &quad_texture_revisions = scratch.quad_texture_revisions;
        // Largest drawn size per image texture this frame, reported once after the loop.
        auto &drawn_image_sizes = scratch.drawn_image_sizes;
        auto &quad_scissors = scratch.quad_scissors;
        auto &quad_groups = scratch.quad_groups;
        auto &quad_blends = scratch.quad_blends;
        auto &stroke_instances = scratch.stroke_instances;
        auto &stroke_scissors = scratch.stroke_scissors;
        auto &stroke_groups = scratch.stroke_groups;
        auto &mask_instances = scratch.mask_instances;
        auto &mask_scissors = scratch.mask_scissors;
        auto &mask_groups = scratch.mask_groups;
        auto &sector_instances = scratch.sector_instances;
        auto &sector_scissors = scratch.sector_scissors;
        auto &sector_groups = scratch.sector_groups;
        auto &ordered_glyphs = scratch.ordered_glyphs;
        auto &ordered_glyph_scissors = scratch.ordered_glyph_scissors;
        auto &glyph_groups = scratch.glyph_groups;
        quad_instances.clear();
        quad_texture_views.clear();
        quad_texture_revisions.clear();
        drawn_image_sizes.clear();
        quad_scissors.clear();
        quad_groups.clear();
        quad_blends.clear();
        stroke_instances.clear();
        stroke_scissors.clear();
        stroke_groups.clear();
        mask_instances.clear();
        mask_scissors.clear();
        mask_groups.clear();
        sector_instances.clear();
        sector_scissors.clear();
        sector_groups.clear();
        ordered_glyphs.clear();
        ordered_glyph_scissors.clear();
        glyph_groups.clear();

        assign_paint_groups(snapshot, snapshot.viewport_extent(), entries);
        for (const PaintEntry &entry : entries) {
            const u32 group_id = entry.group;

            switch (entry.kind) {
                case PaintEntry::Kind::Quad: {
                    const QuadDraw &quad = snapshot.quads_[entry.index];
                    if (!overlaps_scissor(quad.scissor, quad.instance.position, quad.instance.size)) {
                        break;
                    }
                    RHI::TextureViewHandle view = white_view;
                    u64 revision = 0;
                    if (quad.image_ref != nullptr) {
                        if (Renderer::TextureResource *resource = texture_resolver->texture(quad.image_ref->texture)) {
                            view = resource->view;
                            revision = resource->revision;
                            f32 &drawn = drawn_image_sizes[quad.image_ref->texture.value];
                            drawn = std::max(drawn, std::max(quad.instance.size.x, quad.instance.size.y));
                        }
                    }
                    if (quad.view_override) {
                        // An effect group's composite: its own image, not a texture the application owns.
                        view = quad.view_override;
                        revision = quad.view_revision;
                    }
                    quad_blends.push_back(quad.blend);
                    quad_instances.push_back(quad.instance);
                    quad_texture_views.push_back(view);
                    quad_texture_revisions.push_back(revision);
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
                        quad_blends.push_back(UiQuadBlend::Normal);
                        quad_instances.push_back(fill_quad_to_instance(quad));
                        quad_texture_views.push_back(white_view);
                        quad_texture_revisions.push_back(0);
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

        if (!drawn_image_sizes.empty()) {
            auto &drawn = scratch.drawn_images;
            drawn.clear();
            drawn.reserve(drawn_image_sizes.size());
            for (const auto &[value, pixels] : drawn_image_sizes) {
                drawn.emplace_back(Renderer::TextureHandle{value}, pixels);
            }
            texture_resolver->note_texture_demand(drawn);
        }
        if (Core::RendererResult quad_prepared = quad_pipeline_.prepare(
                device, quad_instances, quad_texture_views, quad_texture_revisions, quad_scissors, quad_groups,
                frame_resources.quads, frame_resources.quad_batches, quad_blends);
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
            auto &requests = scratch.glyph_requests;
            requests.clear();
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
            auto &slots = scratch.glyph_slots;
            slots.clear();
            if (auto resident = surface_resources->text_atlas.ensure_resident(
                    device, encoder, requests, slots, out_transient_buffers, out_retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }

            auto &instances = scratch.glyph_instances;
            instances.clear();
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

        stats_ = UiRenderStats{
            .quads = frame_resources.quads.stats.instances,
            .glyphs = static_cast<u32>(ordered_glyphs.size()),
            .strokes = static_cast<u32>(stroke_instances.size()),
            .sectors = static_cast<u32>(sector_instances.size()),
            .custom_elements = static_cast<u32>(frame_resources.custom_draws.size() + frame_resources.custom_strokes.size()),
            .quad_batches = static_cast<u32>(frame_resources.quad_batches.size()),
            .text_batches = static_cast<u32>(frame_resources.text_batches.size()),
            .stroke_batches = static_cast<u32>(frame_resources.stroke_batches.size()),
            .sector_batches = static_cast<u32>(frame_resources.sector_batches.size()),
            .quad_textures = frame_resources.quads.stats.textures,
            .bind_groups_created = frame_resources.quads.stats.bind_groups_created,
            .quad_bytes_uploaded = frame_resources.quads.stats.bytes_uploaded,
            .bindless_textures = quad_pipeline_.bindless(),
            .parallel_group_prepares = parallel_group_prepares_,
        };
        SFT_LOG_INFO_THROTTLED(5000,
            "UI frame: {} quads in {} batches ({} textures, {}), {} glyphs in {} batches, {} stroke segments in {} batches, "
            "{} sectors in {} batches, {} custom elements; {} bind groups created, {} bytes of quads uploaded.",
            stats_.quads, stats_.quad_batches, stats_.quad_textures, stats_.bindless_textures ? "bindless" : "bind group per texture",
            stats_.glyphs, stats_.text_batches, stats_.strokes, stats_.stroke_batches, stats_.sectors, stats_.sector_batches,
            stats_.custom_elements, stats_.bind_groups_created, stats_.quad_bytes_uploaded);

        if (!mask_instances.empty()) {
            if (Core::RendererResult mask_prepared = stroke_pipeline_.prepare(
                    device, mask_instances, mask_scissors, mask_groups,
                    frame_resources.glow_mask_stroke, frame_resources.glow_mask_stroke_batches);
                !mask_prepared) {
                return mask_prepared;
            }

            if (!frame_resources.glow_mask_texture || frame_resources.glow_extent.x != viewport_extent.x ||
                frame_resources.glow_extent.y != viewport_extent.y) {
                release_glow_targets(device, frame_resources);
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
                frame_resources.glow_extent = viewport_extent;
            }
            frame_resources.glow_idle_frames = 0;

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
            settings.frame.bloom.intensity = 1.0f;
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
            const vector<u64> composite_revisions{0u};
            const vector<RHI::Rect2D> composite_scissors{full_viewport_scissor};
            const vector<u32> composite_groups{0u};
            if (Core::RendererResult composite_prepared = quad_pipeline_.prepare(
                    device, composite_instances, composite_views, composite_revisions, composite_scissors, composite_groups,
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


        // What the quad pipeline left bound; any other pipeline drawing on the pass disturbs it.
        UiQuadDrawState quad_state;
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
                        pass, span<const UiQuadDrawBatch>{&frame_resources.quad_batches[quad_cursor], 1}, viewport_size, &quad_state);
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
                quad_state = {};
            }
            while (sector_cursor < frame_resources.sector_batches.size() &&
                   frame_resources.sector_batches[sector_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = sector_pipeline_.draw(
                        pass, span<const UiSectorDrawBatch>{&frame_resources.sector_batches[sector_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++sector_cursor;
                quad_state = {};
            }
            while (text_cursor < frame_resources.text_batches.size() &&
                   frame_resources.text_batches[text_cursor].paint_group == next_group) {
                if (Core::RendererResult drawn = text_pipeline_.draw(
                        pass, span<const Renderer::TextDrawBatch>{&frame_resources.text_batches[text_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++text_cursor;
                quad_state = {};
            }
            while (custom_cursor < frame_resources.custom_draws.size() &&
                   frame_resources.custom_group_ids[custom_cursor] == next_group) {
                if (Core::RendererResult drawn = custom_element_pipeline_.draw(
                        pass, color_format_, span<const CustomDraw>{&frame_resources.custom_draws[custom_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++custom_cursor;
                quad_state = {};
            }
            while (custom_stroke_cursor < frame_resources.custom_strokes.size() &&
                   frame_resources.custom_stroke_group_ids[custom_stroke_cursor] == next_group) {
                if (Core::RendererResult drawn = custom_stroke_element_pipeline_.draw(
                        pass, color_format_, span<const CustomStrokeDraw>{&frame_resources.custom_strokes[custom_stroke_cursor], 1}, viewport_size);
                    !drawn) {
                    return drawn;
                }
                ++custom_stroke_cursor;
                quad_state = {};
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

    UiRenderStats UiRenderer::stats() const {
        auto guard = operation_mutex_->lock();
        (void)guard;
        return stats_;
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
                release_glow_targets(device, resources);
                release_effect_targets(device, resources);
            }
            surface_resources.text_atlas.destroy(device);
        }
        surface_frame_resources_.clear();
        for (const std::unique_ptr<UiRenderer> &child : effect_children_) {
            if (child) {
                child->destroy(device);
            }
        }
        effect_children_.clear();
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
