#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Ecs/src/Resource.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>
#include <UI/UI.hpp>

#include <memory>
#include <optional>
#include <utility>

// Engine-level ECS integration for Sturdy.UI: UI itself stays Ecs-agnostic (see UI.hpp's own doc
// comment), so this is the seam that lets an ECS app build UI trees through ordinary systems
// instead of hand-rolling the pointer-input plumbing and prepare()/draw() glue every consumer
// would otherwise duplicate — see plans/clay-ui-renderer.md's Phase 2 status note.
namespace SFT::Engine {

    // Accumulated primary-pointer state (position + primary-button-down), kept current by a
    // built-in system Engine registers in its own constructor (folding MouseMoveEvent/
    // MouseButtonEvent, the same streams RuntimeClient's own FlyCameraState reads). This is the
    // minimal slice of the still-unbuilt InputState (plans/ecs-engine-subsystem-access.md) that
    // UI hit-testing actually needs — not a general input-state resource. Read via
    // Ecs::ReadResource<UiPointerState>; hand `.state()` straight to UI::Context::begin_layout().
    class UiPointerState {
      public:
        void set_position(glm::vec2 position) noexcept { state_.position = position; }
        void set_down(bool down) noexcept { state_.down = down; }
        [[nodiscard]] const UI::PointerState &state() const noexcept { return state_; }

      private:
        UI::PointerState state_{};
    };

    // Owns one UI::Context + UI::UiRenderer pair as an ordinary World resource
    // (Ecs::WriteResource<UiContext>), so any system with resource access can build/query a UI
    // tree — not just whichever code happens to own the Engine instance. Font registration stays
    // the caller's job (font choice/discovery is app policy, not Engine policy); this only owns
    // the two GPU-backed objects every UI consumer needs regardless of what it draws.
    class UiContext {
      public:
        // Lazily creates the UI::Context/UI::UiRenderer pair against `device`/`color_format`.
        // Needs a live RHI device, which isn't necessarily up before the first frame — safe to
        // call every frame; a cheap no-op once ready() (or once creation has already failed once,
        // to avoid retrying a hard failure every single frame).
        [[nodiscard]] bool ensure_ready(RHI::RhiDevice &device, RHI::Format color_format) {
            if (renderer_.has_value()) {
                return true;
            }
            if (create_attempted_) {
                return false;
            }
            create_attempted_ = true;

            auto context = UI::Context::create(UI::Context::Config{});
            if (!context) {
                Foundation::log_error("Engine::UiContext: failed to create UI::Context.");
                return false;
            }
            context_ = std::move(*context);

            auto renderer = UI::UiRenderer::create(device, color_format);
            if (!renderer) {
                Foundation::log_error("Engine::UiContext: failed to create UI::UiRenderer.");
                context_.destroy();
                return false;
            }
            renderer_ = std::move(*renderer);
            return true;
        }

        [[nodiscard]] bool ready() const noexcept { return renderer_.has_value(); }
        [[nodiscard]] UI::Context &context() noexcept { return context_; }

        // Packages an already-finished FrameSnapshot (UI::Context::finish_frame()'s result) into
        // the RenderGraph's UiOverlayHooks seam (Renderer::Scene.hpp) — the exact prepare()/draw()
        // glue every consumer of this UI package would otherwise write by hand. `snapshot` is
        // shared (not moved) since the returned hooks' `prepare`/`draw` closures both need to
        // outlive this call, holding it alive until the frame graph actually runs them.
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(std::shared_ptr<UI::FrameSnapshot> snapshot,
                                                                    Renderer::Renderer *texture_resolver) {
            Renderer::UiOverlayHooks hooks;
            if (!renderer_.has_value()) {
                return hooks;
            }
            hooks.prepare = [this, snapshot, texture_resolver](
                                RHI::RhiDevice &device, RHI::CommandEncoder &encoder, glm::vec2 /*viewport_size*/,
                                std::vector<RHI::BufferHandle> &transient_buffers,
                                Renderer::TextAtlasRetiredResources &retired_atlas_resources) {
                return renderer_->prepare(device, encoder, *snapshot, texture_resolver, transient_buffers,
                                          retired_atlas_resources);
            };
            hooks.draw = [this](RHI::RenderPassEncoder &pass, glm::vec2 viewport_size) {
                return renderer_->draw(pass, viewport_size);
            };
            return hooks;
        }

        void destroy(RHI::RhiDevice &device) noexcept {
            if (renderer_) {
                renderer_->destroy(device);
                renderer_.reset();
            }
            context_.destroy();
            create_attempted_ = false;
        }

      private:
        UI::Context context_{};
        std::optional<UI::UiRenderer> renderer_{};
        bool create_attempted_ = false;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::UiPointerState, "sturdy.engine.ui_pointer_state");
SFT_ECS_RESOURCE(SFT::Engine::UiContext, "sturdy.engine.ui_context");
