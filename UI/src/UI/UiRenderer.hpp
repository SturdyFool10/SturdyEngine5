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
#include "UiCustomElementPipeline.hpp"
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
    // Quads (rects/borders/images), text, and custom-shader draws are interleaved into one global
    // paint order (see PaintKey, Style.hpp) rather than three fixed phases: prepare() sorts every
    // draw item from the snapshot by (z, paint_index), regroups consecutive same-kind runs into
    // batches (still respecting texture/atlas-tile/scissor batching within a run — see
    // UiQuadPipeline::prepare()/Renderer::TextPipeline::prepare()'s own doc comments for the extra
    // paint_group key this drives), and draw() then walks that merged order, switching pipelines
    // between groups so an element's background/text/border, and different elements at different
    // z, all composite correctly relative to each other. UiRenderer's own per-frame GPU resources
    // (TextFrameResources/UiQuadFrameResources) are also single-buffered, not N-buffered per
    // frame-in-flight — safe for a snapshot whose *content* doesn't change frame to frame (the
    // write is skipped once uploaded once) but not yet safe for animated/dynamic UI content; see
    // plans/clay-ui-renderer.md's Phase 2 note.
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

        // Issues the batches prepare() built, interleaved by paint order (see class doc comment).
        // Each pipeline sets its own scissor per batch; the caller should not rely on scissor (or
        // bound-pipeline) state surviving this call.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        Renderer::TextAtlas text_atlas_;
        Renderer::TextPipeline text_pipeline_;
        UiQuadPipeline quad_pipeline_;
        UiCustomElementPipeline custom_element_pipeline_;
        Renderer::TextFrameResources text_frame_resources_;
        UiQuadFrameResources quad_frame_resources_;

        vector<Renderer::TextDrawBatch> text_batches_;
        vector<UiQuadDrawBatch> quad_batches_;
        // Reordered by prepare() into the same global paint order as quad_batches_/text_batches_
        // (see PaintKey, Style.hpp) — custom_group_ids_[i] is custom_draws_[i]'s paint-order group,
        // parallel to it (a separate array rather than a field on CustomDraw itself since
        // CustomDraw::paint already carries the *source* PaintKey; the group id is a UiRenderer-
        // internal renumbering of it, only meaningful alongside quad_batches_/text_batches_'s own
        // paint_group). draw() walks all three in lockstep by ascending group id — see its own doc
        // comment.
        vector<CustomDraw> custom_draws_;
        vector<u32> custom_group_ids_;
        // Stashed from create() — UiCustomElementPipeline's shader cache is keyed by color_format,
        // and needs it again at both prepare() and draw() time, neither of which otherwise takes it.
        RHI::Format color_format_{};
        // Lazily created on first prepare() that has a texture_resolver — see prepare()'s doc
        // comment for why this isn't Renderer::ensure_default_white_texture().
        Renderer::TextureHandle white_texture_{};

        bool ready_ = false;
    };

} // namespace SFT::UI
