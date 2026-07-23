#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>

#include "Context.hpp"
#include "UiQuadPipeline.hpp"

using std::vector;

namespace SFT::UI {

    // Batches one UI::Context::FrameSnapshot into the minimum practical number of draw calls and
    // issues them — the host of Clay's render-command list (plans/clay-ui-renderer.md). Owns its
    // own Renderer::TextAtlas/TextPipeline instance (independent of Renderer's debug text overlay)
    // plus a UiQuadPipeline for rects/borders/images.
    //
    // Two-phase, mirroring Renderer::prepare_text_overlay()/draw_text_overlay() exactly: prepare()
    // resolves any IMAGE command's texture through `texture_resolver`, shapes/rasterizes any new
    // text, and uploads this frame's instance data via `encoder` (before a render pass has begun);
    // draw() then issues the actual draw calls against an already-open `pass`. Unlike Context
    // (which must run on whichever thread built the layout tree), UiRenderer touches only the
    // already-resolved FrameSnapshot plus GPU state, so it's safe to run prepare()/draw() on a
    // different thread than the one that built the snapshot (e.g. Engine's dedicated render
    // thread) — see Renderer::UiOverlayHooks for the seam this is meant to plug into.
    //
    // Quads (rects/borders/images) are batched by (texture, active clip rect) and drawn first;
    // text is drawn last, over everything, and does not yet respect nested clip regions (see the
    // note on draw() below) — a deliberate, documented Phase 1 scope cut; see
    // plans/clay-ui-renderer.md's Phase 2. UiRenderer's own per-frame GPU resources
    // (TextFrameResources/UiQuadFrameResources) are also single-buffered, not N-buffered per
    // frame-in-flight — safe for a snapshot whose *content* doesn't change frame to frame (the
    // write is skipped once uploaded once) but not yet safe for animated/dynamic UI content; see
    // the same Phase 2 note.
    class UiRenderer {
      public:
        UiRenderer() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<UiRenderer> create(RHI::RhiDevice &device, RHI::Format color_format);

        // `out_retired_atlas_resources` collects atlas images superseded by grow-only replacement,
        // same deferred-destruction contract as Renderer::TextAtlas::ensure_resident() itself. No
        // separate viewport_size parameter — the snapshot already carries it (every instance
        // position and the full-viewport scissor were resolved against it in finish_frame()).
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                                    vector<RHI::BufferHandle> &out_transient_buffers,
                                                    Renderer::TextAtlasRetiredResources &out_retired_atlas_resources);

        // Issues the batches prepare() built. Sets its own scissor per quad batch; the caller should
        // not rely on scissor state surviving this call.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        Renderer::TextAtlas text_atlas_;
        Renderer::TextPipeline text_pipeline_;
        UiQuadPipeline quad_pipeline_;
        Renderer::TextFrameResources text_frame_resources_;
        UiQuadFrameResources quad_frame_resources_;

        vector<Renderer::TextDrawBatch> text_batches_;
        vector<UiQuadDrawBatch> quad_batches_;
        RHI::Rect2D full_viewport_scissor_{};
        // Lazily created on first prepare() that has a texture_resolver — see prepare()'s doc
        // comment for why this isn't Renderer::ensure_default_white_texture().
        Renderer::TextureHandle white_texture_{};

        bool ready_ = false;
    };

} // namespace SFT::UI
