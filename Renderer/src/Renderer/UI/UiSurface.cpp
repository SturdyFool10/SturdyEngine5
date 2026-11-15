#include <Renderer/UI/UiSurface.hpp>

#include <utility>

namespace SFT::UI {

    UiRenderStats UiSurface::render_stats() const {
        auto guard = renderer_state_->renderer.lock();
        return guard->has_value() ? (*guard)->stats() : UiRenderStats{};
    }

    bool UiSurface::ensure_ready(RHI::RhiDevice &device, RHI::Format color_format) {
        {
            auto guard = renderer_state_->renderer.lock();
            if (guard->has_value()) {
                return true;
            }
        }
        if (create_attempted_) {
            return false;
        }
        create_attempted_ = true;

        if (!ensure_context()) {
            return false;
        }

        renderer_state_->bindless_preferred.store(UiQuadPipeline::bindless_preferred());
        auto renderer = UiRenderer::create(device, color_format);
        if (!renderer) {
            // Carry the backend's reason: the actual cause (a pipeline or format the device rejected) is only known here.
            Foundation::log_error("UI::UiSurface: failed to create UI::UiRenderer: {}", renderer.error().message);
            return false;
        }
        auto guard = renderer_state_->renderer.lock();
        *guard = std::move(*renderer);
        renderer_state_->format.store(color_format);
        return true;
    }

    bool UiSurface::ensure_context() {
        if (context_created_) {
            return true;
        }
        auto context = Context::create(Context::Config{});
        if (!context) {
            Foundation::log_error("UI::UiSurface: failed to create UI::Context: {}", context.error().message);
            return false;
        }
        context_ = std::move(*context);
        context_created_ = true;
        return true;
    }

    bool UiSurface::ready() const {
        auto guard = renderer_state_->renderer.lock();
        return guard->has_value();
    }

    Context &UiSurface::begin_frame(glm::vec2 extent, f32 delta_seconds) {
        context_.set_pixel_scale(pixel_scale());
        if (texture_resolver_ != nullptr) {
            context_.set_mask_alpha_sampler([resolver = texture_resolver_](Renderer::TextureHandle texture, glm::vec2 uv, bool luminance) -> std::optional<f32> {
                const Renderer::TextureResource *resource = resolver->texture(texture);
                if (resource == nullptr || resource->width == 0 || resource->height == 0 || resource->pixel_data.empty()) {
                    return std::nullopt;
                }
                const bool bgra = resource->format == RHI::Format::BGRA8Unorm || resource->format == RHI::Format::BGRA8UnormSrgb;
                const bool rgba = resource->format == RHI::Format::RGBA8Unorm || resource->format == RHI::Format::RGBA8UnormSrgb;
                if (!bgra && !rgba) {
                    return std::nullopt;
                }
                const u32 x = std::min<u32>(resource->width - 1, static_cast<u32>(std::max(uv.x, 0.0f) * static_cast<f32>(resource->width)));
                const u32 y = std::min<u32>(resource->height - 1, static_cast<u32>(std::max(uv.y, 0.0f) * static_cast<f32>(resource->height)));
                const usize at = (static_cast<usize>(y) * resource->width + x) * 4;
                if (at + 3 >= resource->pixel_data.size()) {
                    return std::nullopt;
                }
                const auto channel = [&](usize i) { return static_cast<f32>(std::to_integer<u32>(resource->pixel_data[at + i])) / 255.0f; };
                const f32 r = bgra ? channel(2) : channel(0);
                const f32 g = channel(1);
                const f32 b = bgra ? channel(0) : channel(2);
                const f32 a = channel(3);
                return luminance ? (0.2126f * r + 0.7152f * g + 0.0722f * b) * a : a;
            });
        }
        context_.begin_layout(extent, input_.pointer(), delta_seconds);
        input_.set_pointer_consumed(context_.pointer_over_any() || context_.pointer_captured());
        // Only the pointer's one-frame edges are spent here. Typed text and edit keys are read by the widgets that are
        // built *after* this call, so they are cleared once the frame is finished (see finish_overlay/overlay_for_snapshot).
        input_.end_pointer_frame();
        return context_;
    }

    Renderer::OverlayPass UiSurface::finish_overlay(Renderer::Renderer *texture_resolver, OverlayOptions options) {
        return overlay_for_snapshot(std::make_shared<FrameSnapshot>(context_.finish_frame()), texture_resolver, std::move(options));
    }

    Renderer::OverlayPass UiSurface::overlay_for_snapshot(std::shared_ptr<FrameSnapshot> snapshot,
                                                          Renderer::Renderer *texture_resolver, OverlayOptions options) {
        input_.end_text_frame(); // the frame's widgets have read what was typed
        return make_overlay(std::move(snapshot), texture_resolver, std::move(options));
    }

    Renderer::OverlayPass UiSurface::make_overlay(std::shared_ptr<FrameSnapshot> snapshot, Renderer::Renderer *texture_resolver,
                                                  OverlayOptions options) const {
        return make_overlay_for(renderer_state_, false, std::move(snapshot), texture_resolver, std::move(options));
    }

    Renderer::OverlayPass UiSurface::make_target_overlay(std::shared_ptr<FrameSnapshot> snapshot, Renderer::Renderer *texture_resolver,
                                                         Renderer::OffscreenRenderTargetHandle target, glm::vec4 clear_color,
                                                         OverlayOptions options) const {
        Renderer::OverlayPass overlay;
        overlay.name = options.name;
        overlay.prepare_only = true;
        if (!snapshot || texture_resolver == nullptr || !renderer_state_->renderer.lock()->has_value()) {
            return overlay;
        }
        const std::shared_ptr<RendererState> state = renderer_state_;
        overlay.prepare = [state, snapshot, texture_resolver, target, clear_color,
                           name = overlay.name](Renderer::OverlayPrepareContext &prepare) -> Core::RendererResult {
            const std::optional<RHI::Format> draw_format = texture_resolver->offscreen_target_draw_format(target);
            const Renderer::TextureResource *resource = texture_resolver->texture(texture_resolver->offscreen_render_target_texture(target));
            if (!draw_format || resource == nullptr) {
                return {}; // gone: the caller renders it the ordinary way
            }
            const Core::Extent2D extent{resource->width, resource->height};
            if (snapshot->viewport_extent() != extent) {
                return {}; // laid out for another size; the next layout fixes it
            }
            auto guard = state->renderer.lock();
            if (!*guard || state->format.load() != *draw_format) {
                return {};
            }
            std::vector<Renderer::RenderGraphTextureHandle> sampled;
            if (Core::RendererResult prepared = (*guard)->prepare(prepare.device, prepare.encoder, prepare.graph, *snapshot, texture_resolver,
                                                                  prepare.surface, prepare.frame_slot_index, prepare.transient_buffers,
                                                                  prepare.retired_text_atlas_resources, prepare.transient_bind_groups, sampled, {},
                                                                  prepare.extra_command_buffers);
                !prepared) {
                return prepared;
            }
            // The renderer declares the clear, the draw and the passes that finish the target (shader encode, mips) like a frame aimed at it.
            return texture_resolver->add_offscreen_target_draw(
                prepare.graph, target, prepare.transient_bind_groups,
                Renderer::Renderer::OffscreenTargetDraw{
                    .label = UString{name.empty() ? std::string{"ui target"} : name + " target"},
                    .clear = RHI::ClearColor{clear_color.r, clear_color.g, clear_color.b, clear_color.a},
                    .sampled = std::move(sampled),
                    .draw = [state, surface = prepare.surface, slot = prepare.frame_slot_index](RHI::RenderPassEncoder &pass,
                                                                                                    Core::Extent2D target_extent) -> Core::RendererResult {
                        auto draw_guard = state->renderer.lock();
                        if (!*draw_guard) {
                            return {};
                        }
                        return (*draw_guard)->draw(pass, glm::vec2{target_extent}, surface, slot);
                    }});
        };
        return overlay;
    }

    std::vector<Renderer::OverlayPass> UiSurface::finish_overlays(Renderer::Renderer *texture_resolver, OverlayOptions options) {
        input_.end_text_frame();
        return make_layer_overlays(FrameSnapshot::split_layers(std::make_shared<FrameSnapshot>(context_.finish_frame())), texture_resolver,
                                   std::move(options));
    }

    std::vector<Renderer::OverlayPass> UiSurface::make_layer_overlays(std::vector<std::shared_ptr<FrameSnapshot>> pieces,
                                                                      Renderer::Renderer *texture_resolver, OverlayOptions options) {
        std::vector<Renderer::OverlayPass> overlays;
        if (layer_states_.size() + 1 < pieces.size()) {
            layer_states_.resize(pieces.size() - 1);
        }
        for (usize i = 0; i < pieces.size(); ++i) {
            OverlayOptions piece_options = options;
            if (i > 0) {
                if (!layer_states_[i - 1]) {
                    layer_states_[i - 1] = std::make_shared<RendererState>();
                }
                piece_options.name = options.name + " layer " + std::to_string(i);
            }
            overlays.push_back(make_overlay_for(i == 0 ? renderer_state_ : layer_states_[i - 1], i > 0, std::move(pieces[i]), texture_resolver,
                                                std::move(piece_options)));
        }
        return overlays;
    }

    Renderer::OverlayPass UiSurface::make_overlay_for(std::shared_ptr<RendererState> state, bool create_on_demand,
                                                      std::shared_ptr<FrameSnapshot> snapshot, Renderer::Renderer *texture_resolver,
                                                      OverlayOptions options) const {
        Renderer::OverlayPass overlay;
        overlay.name = std::move(options.name);
        overlay.hdr_reference_white_scale = options.hdr_reference_white_scale;
        overlay.hdr_paper_white_nits = options.hdr_paper_white_nits;
        if (!snapshot || !renderer_state_->renderer.lock()->has_value()) {
            return overlay; // no draw: the pass is skipped (layers above the first need the first's renderer to exist, so the UI is ready)
        }
        overlay.prepare = [state, create_on_demand, snapshot, texture_resolver](Renderer::OverlayPrepareContext &prepare) -> Core::RendererResult {
            const Core::Extent2D layout_extent = snapshot->viewport_extent();
            if (prepare.viewport != glm::vec2{layout_extent}) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UI snapshot extent does not match the frame it is drawn on; call begin_frame() with the presentation "
                    "(or off-screen target) extent.");
            }
            auto guard = state->renderer.lock();
            if (!*guard && !create_on_demand) {
                return {}; // released (graphics reconstruction) while this frame was in flight: draw nothing
            }
            // The pass draws into the swapchain, or into a linear RGBA16Float layer for HDR output and for a UI-only frame
            // on a transparent surface. The pipelines are built for one format, so follow the pass: callers cannot know
            // in advance which of the two the renderer will pick (the display may refuse HDR the app asked for).
            const bool bindless_changed = state->bindless_preferred.load() != UiQuadPipeline::bindless_preferred();
            if (prepare.format != RHI::Format::Undefined && (!*guard || state->format.load() != prepare.format || bindless_changed)) {
                if (*guard) {
                    (*guard)->destroy(prepare.device);
                    guard->reset();
                }
                state->bindless_preferred.store(UiQuadPipeline::bindless_preferred());
                auto rebuilt = UiRenderer::create(prepare.device, prepare.format);
                if (!rebuilt) {
                    Foundation::log_error("UI::UiSurface: failed to rebuild UI::UiRenderer for the overlay target format: {}",
                                          rebuilt.error().message);
                    return {};
                }
                *guard = std::move(*rebuilt);
                state->format.store(prepare.format);
            }
            return (*guard)->prepare(prepare.device, prepare.encoder, prepare.graph, *snapshot, texture_resolver, prepare.surface,
                                     prepare.frame_slot_index, prepare.transient_buffers, prepare.retired_text_atlas_resources,
                                     prepare.transient_bind_groups, prepare.sampled_textures, prepare.target,
                                     prepare.extra_command_buffers);
        };
        overlay.draw = [state](Renderer::OverlayPassContext &draw) -> Core::RendererResult {
            auto guard = state->renderer.lock();
            if (!*guard || (draw.format != RHI::Format::Undefined && state->format.load() != draw.format)) {
                return {}; // no renderer, or one built for another target format: skip rather than fail validation
            }
            return (*guard)->draw(draw.pass, glm::vec2{draw.extent}, draw.surface, draw.frame_slot_index);
        };
        return overlay;
    }

    void UiSurface::release_renderer(RHI::RhiDevice &device) noexcept {
        for (const std::shared_ptr<RendererState> &layer : layer_states_) {
            if (!layer) {
                continue;
            }
            auto layer_guard = layer->renderer.lock();
            if (*layer_guard) {
                (*layer_guard)->destroy(device);
                layer_guard->reset();
            }
            layer->format.store(RHI::Format::Undefined);
        }
        auto guard = renderer_state_->renderer.lock();
        if (*guard) {
            (*guard)->destroy(device);
            guard->reset();
        }
        create_attempted_ = false;
    }

    void UiSurface::destroy(RHI::RhiDevice &device) noexcept {
        release_renderer(device);
        if (context_created_) {
            context_.destroy();
            context_created_ = false;
        }
    }

} // namespace SFT::UI
