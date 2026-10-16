#include <Renderer/UI/UiSurface.hpp>

#include <utility>

namespace SFT::UI {

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

        if (!context_created_) {
            auto context = Context::create(Context::Config{});
            if (!context) {
                Foundation::log_error("UI::UiSurface: failed to create UI::Context: {}", context.error().message);
                return false;
            }
            context_ = std::move(*context);
            context_created_ = true;
        }

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

    bool UiSurface::ready() const {
        auto guard = renderer_state_->renderer.lock();
        return guard->has_value();
    }

    Context &UiSurface::begin_frame(glm::vec2 extent, f32 delta_seconds) {
        context_.set_pixel_scale(pixel_scale());
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
        Renderer::OverlayPass overlay;
        overlay.name = std::move(options.name);
        overlay.hdr_reference_white_scale = options.hdr_reference_white_scale;
        const std::shared_ptr<RendererState> state = renderer_state_;
        {
            auto guard = state->renderer.lock();
            if (!guard->has_value() || !snapshot) {
                return overlay; // no draw: the pass is skipped
            }
        }
        overlay.prepare = [state, snapshot, texture_resolver](Renderer::OverlayPrepareContext &prepare) -> Core::RendererResult {
            const Core::Extent2D layout_extent = snapshot->viewport_extent();
            if (prepare.viewport != glm::vec2{layout_extent}) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UI snapshot extent does not match the frame it is drawn on; call begin_frame() with the presentation "
                    "(or off-screen target) extent.");
            }
            auto guard = state->renderer.lock();
            if (!*guard) {
                return {}; // released (graphics reconstruction) while this frame was in flight: draw nothing
            }
            // The pass draws into the swapchain, or into a linear RGBA16Float layer for HDR output and for a UI-only frame
            // on a transparent surface. The pipelines are built for one format, so follow the pass: callers cannot know
            // in advance which of the two the renderer will pick (the display may refuse HDR the app asked for).
            if (prepare.format != RHI::Format::Undefined && state->format.load() != prepare.format) {
                (*guard)->destroy(prepare.device);
                guard->reset();
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
                                     prepare.transient_bind_groups, prepare.sampled_textures);
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
