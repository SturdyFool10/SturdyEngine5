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
    // issues them — the host of Clay's render-command list (plans/clay-ui-renderer.md). Owns a
    // TextPipeline plus one Renderer::TextAtlas per render surface (independent of Renderer's debug
    // text overlay), along with a UiQuadPipeline for rects/borders/images.
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
    // z, all composite correctly relative to each other. Persistent text/quad instance buffers are
    // N-buffered by the renderer-provided (surface, frame-resource-slot) pair, so animated content can
    // update every frame without overwriting a buffer an older GPU frame or another window is reading.
    class UiRenderer {
      public:
        UiRenderer() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<UiRenderer> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);

        // `out_retired_atlas_resources` collects atlas images superseded by grow-only replacement,
        // same deferred-destruction contract as Renderer::TextAtlas::ensure_resident() itself. No
        // separate viewport_size parameter — the snapshot already carries it (every instance
        // position and the full-viewport scissor were resolved against it in finish_frame()).
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                                    Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                                    vector<RHI::BufferHandle> &out_transient_buffers,
                                                    Renderer::TextAtlasRetiredResources &out_retired_atlas_resources);

        // Issues the batches prepare() built, interleaved by paint order (see class doc comment).
        // Each pipeline sets its own scissor per batch; the caller should not rely on scissor (or
        // bound-pipeline) state surviving this call.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                                 Core::RenderSurfaceHandle surface, u32 frame_resource_index);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        Renderer::TextPipeline text_pipeline_;
        UiQuadPipeline quad_pipeline_;
        UiCustomElementPipeline custom_element_pipeline_;
        struct FrameResources {
            Renderer::TextFrameResources text;
            UiQuadFrameResources quads;
            vector<Renderer::TextDrawBatch> text_batches;
            vector<UiQuadDrawBatch> quad_batches;
            // Reordered by prepare() into the same global paint order as quad_batches/text_batches;
            // custom_group_ids[i] is custom_draws[i]'s paint-order group. Keeping all four arrays in
            // this surface/frame slot prevents another surface's prepare from replacing the batches
            // before this one records draw commands.
            vector<CustomDraw> custom_draws;
            vector<u32> custom_group_ids;
        };
        struct SurfaceFrameResources {
            Core::RenderSurfaceHandle surface{};
            Renderer::TextAtlas text_atlas;
            vector<FrameResources> frames;
        };
        vector<SurfaceFrameResources> surface_frame_resources_;
        // Stashed from create() — UiCustomElementPipeline's shader cache is keyed by color_format,
        // and needs it again at both prepare() and draw() time, neither of which otherwise takes it.
        RHI::Format color_format_{};
        // Stashed from create() — custom_element_pipeline_.prepare() needs it every frame (a custom
        // element's shader is only compiled lazily, on the first frame it's actually drawn).
        bool enable_shader_disk_cache_ = true;
        // Lazily created on first prepare() that has a texture_resolver — see prepare()'s doc
        // comment for why this isn't Renderer::ensure_default_white_texture().
        Renderer::TextureHandle white_texture_{};

        bool ready_ = false;
    };

} // namespace SFT::UI
