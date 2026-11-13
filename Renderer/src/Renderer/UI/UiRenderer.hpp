#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <glm/vec2.hpp>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <RHI/Threading.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/CustomStrokeElementPipeline.hpp>
#include <Renderer/UI/UiCustomElementPipeline.hpp>
#include <Renderer/UI/UiQuadPipeline.hpp>
#include <Renderer/UI/UiSectorPipeline.hpp>
#include <Renderer/UI/UiStrokePipeline.hpp>

using std::vector;

namespace SFT::UI {


    /// What the last `UiRenderer::prepare` built, for finding out what a UI costs (draw calls, descriptor traffic, bytes
    /// uploaded) before deciding what to optimise.
    struct UiRenderStats {
        /// Quads, glyphs, stroke segments, sectors and custom-shader elements the frame drew after culling.
        u32 quads = 0;
        u32 glyphs = 0;
        u32 strokes = 0;
        u32 sectors = 0;
        u32 custom_elements = 0;
        /// Draws issued per kind, which is what batching is meant to shrink.
        u32 quad_batches = 0;
        u32 text_batches = 0;
        u32 stroke_batches = 0;
        u32 sector_batches = 0;
        /// Different textures the quads sampled; with bindless textures this is the size of the table.
        u32 quad_textures = 0;
        /// Bind groups created while preparing (zero on a frame that looks like the last one).
        u32 bind_groups_created = 0;
        /// Bytes written to the quad instance buffer (zero when the frame's quads matched the last upload).
        u64 quad_bytes_uploaded = 0;
        /// Whether quad textures are bound as one array.
        bool bindless_textures = false;
    };

    class UiRenderer {
      public:
        /// Constructs a `UiRenderer` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UiRenderer() noexcept = default;
        /// Constructs a `UiRenderer` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        UiRenderer(UiRenderer &&other) noexcept;
        /// Assigns a new value to this `UiRenderer`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        UiRenderer &operator=(UiRenderer &&other) noexcept;
        /// Disables this construction form for `UiRenderer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        UiRenderer(const UiRenderer &) = delete;
        /// Assigns a new value to this `UiRenderer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        UiRenderer &operator=(const UiRenderer &) = delete;

        /// Creates a `UiRenderer` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<UiRenderer> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param render_graph The current frame's render graph. Any stroke path flagged with
        ///        `StrokeStyle::glow_intensity > 0` draws into this frame's shared, viewport-sized glow
        ///        mask instead of the normal batched stroke stream; one real bloom pass chain
        ///        (`Renderer::add_ui_glow_bloom_passes`) then blurs that whole mask and the result is
        ///        composited additively over the entire surface, unclipped by any container — see
        ///        FrameResources' own doc comment for the full rationale.
        /// @param snapshot `snapshot` value used by the operation.
        /// @param texture_resolver Texture used or affected by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame_resource_index Zero-based index of the target element or entry.
        /// @param out_transient_buffers Buffer used or affected by the operation.
        /// @param out_retired_atlas_resources `out_retired_atlas_resources` value used by the operation.
        /// @param out_transient_bind_groups Bind groups any glow-bloom pass chain created — the caller
        ///        is responsible for destroying them once this frame's GPU work has completed, same
        ///        convention as every other transient-resource out-param in the frame path.
        /// @param out_glow_bloom_outputs Real, `render_graph`-imported destination texture for each
        ///        glow element's bloom composite this call queued — the caller must declare each one
        ///        as a sampled-texture read dependency on whatever pass ends up sampling it (the "UI
        ///        overlay" pass), so the graph's transient-memory aliasing preserves it correctly.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    Renderer::RenderGraph &render_graph,
                                                    const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                                    Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                                    vector<RHI::BufferHandle> &out_transient_buffers,
                                                    Renderer::TextAtlasRetiredResources &out_retired_atlas_resources,
                                                    vector<RHI::BindGroupHandle> &out_transient_bind_groups,
                                                    vector<Renderer::RenderGraphTextureHandle> &out_glow_bloom_outputs);

        /// Issues the batches prepare() built, interleaved by paint order (see class doc comment).
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param viewport_size Requested or available size for the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame_resource_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Each pipeline sets its own scissor per batch; the caller should not rely on scissor
        ///       (or bound-pipeline) state surviving this call.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                                 Core::RenderSurfaceHandle surface, u32 frame_resource_index);

        /// Destroys or releases the `UiRenderer` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;
        /// Reads the requested data from the associated source.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool ready() const noexcept;


        /// Returns the current or globally available generation value.
        ///
        /// @return Returns the current generation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 generation() const noexcept;

        /// What the most recent `prepare` built; see `UiRenderStats`.
        [[nodiscard]] UiRenderStats stats() const;

      private:
        /// One thing to paint, before the paint order puts them in sequence.
        struct PaintEntry {
            PaintKey paint;
            enum class Kind : u8 { Quad, Text, Custom, Stroke, Fill, Sector, CustomStroke } kind = Kind::Quad;
            usize index = 0;
            usize count = 1;
            /// The draw group the entry was assigned (see `assign_paint_groups`).
            u32 group = 0;
        };

        /// Gives each entry (already in paint order) the draw group it is drawn in. A group is one pipeline's batches, so
        /// a UI that alternates panels and text would otherwise draw once per alternation. An entry moves into an earlier
        /// group of its own kind when nothing it would be drawn in front of overlaps it; entries that overlap keep their
        /// paint order. Entries of one group end up next to each other in the result.
        static void assign_paint_groups(const FrameSnapshot &snapshot, Core::Extent2D viewport, vector<PaintEntry> &entries);

        /// Working vectors of `prepare`, kept so a frame reuses the memory of the last instead of allocating it all
        /// again. Everything in it is cleared at the start of each `prepare`; `prepare` is serialised, so one set will do.
        struct PrepareScratch {
            vector<PaintEntry> entries;
            vector<UiQuadInstance> quad_instances;
            vector<RHI::TextureViewHandle> quad_texture_views;
            vector<u64> quad_texture_revisions;
            std::unordered_map<u64, f32> drawn_image_sizes;
            vector<std::pair<Renderer::TextureHandle, f32>> drawn_images;
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
            vector<Renderer::GlyphRequest> glyph_requests;
            vector<Renderer::GlyphSlot> glyph_slots;
            vector<Renderer::GlyphInstance> glyph_instances;
        };
        PrepareScratch scratch_;
        UiRenderStats stats_{};

        Renderer::TextPipeline text_pipeline_;
        UiQuadPipeline quad_pipeline_;
        UiStrokePipeline stroke_pipeline_;
        UiSectorPipeline sector_pipeline_;
        UiCustomElementPipeline custom_element_pipeline_;
        CustomStrokeElementPipeline custom_stroke_element_pipeline_;
        struct FrameResources {
            Renderer::TextFrameResources text;
            UiQuadFrameResources quads;
            UiStrokeFrameResources strokes;
            UiSectorFrameResources sectors;
            vector<Renderer::TextDrawBatch> text_batches;
            vector<UiQuadDrawBatch> quad_batches;
            vector<UiStrokeDrawBatch> stroke_batches;
            vector<UiSectorDrawBatch> sector_batches;


            vector<CustomDraw> custom_draws;
            vector<u32> custom_group_ids;
            vector<CustomStrokeDraw> custom_strokes;
            vector<u32> custom_stroke_group_ids;

            // A real, full-screen bloom, not a per-element approximation: every StrokeStyle::
            // glow_intensity > 0 path drawn this frame is skipped in the normal batched stroke stream
            // and instead drawn (at its own screen position and clip, brightness pre-scaled by its own
            // glow_intensity) into this ONE shared, viewport-sized mask. Renderer::add_ui_glow_bloom_passes
            // then blurs that whole mask once — real room to spread since the mask spans the entire
            // surface, not a small per-element crop — and the blurred result is composited back with
            // additive GPU blending as the very last draw of the frame, using full_viewport_scissor (no
            // clip at all), so the halo can flow out over whatever container the source line sits in,
            // the way a real glow does. glow_mask_texture/glow_bloom_texture are owned directly by
            // UiRenderer (not the render graph) so glow_bloom_view is known immediately at prepare()
            // time, before the graph has executed. Torn down and rebuilt every frame for the (surface,
            // frame_resource_index) slot they belong to, same reuse cadence as every other per-frame GPU
            // resource in FrameResources.
            RHI::TextureHandle glow_mask_texture{};
            RHI::TextureViewHandle glow_mask_view{};
            RHI::TextureHandle glow_bloom_texture{};
            RHI::TextureViewHandle glow_bloom_view{};
            /// The size those two textures were made at, and how many frames in a row have not used them. They are kept
            /// from frame to frame (the graph imports them as `Undefined`, so nothing is carried over) and only
            /// released after a stretch of frames without glow, or remade when the surface changes size.
            Core::Extent2D glow_extent{};
            u32 glow_idle_frames = 0;
            UiStrokeFrameResources glow_mask_stroke;
            vector<UiStrokeDrawBatch> glow_mask_stroke_batches;
            UiQuadFrameResources glow_composite_quad;
            vector<UiQuadDrawBatch> glow_composite_quad_batches;
        };

        struct SurfaceFrameResources {
            Core::RenderSurfaceHandle surface{};
            Renderer::TextAtlas text_atlas;
            vector<FrameResources> frames;
        };
        vector<SurfaceFrameResources> surface_frame_resources_;

        /// Destroys a frame slot's glow targets (and the composite quad bound to them).
        static void release_glow_targets(RHI::RhiDevice &device, FrameResources &resources) noexcept;


        RHI::Format color_format_{};
        std::atomic<u64> generation_{0};


        bool enable_shader_disk_cache_ = true;


        Renderer::TextureHandle white_texture_{};


        std::shared_ptr<Async::Mutex<u8>> operation_mutex_ = std::make_shared<Async::Mutex<u8>>(0);
        bool ready_ = false;
    };

} // namespace SFT::UI
