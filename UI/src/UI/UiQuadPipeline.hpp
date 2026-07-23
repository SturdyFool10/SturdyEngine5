#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include "UiQuad.hpp"

using std::span;
using std::vector;

namespace SFT::UI {

    // One draw call's worth of contiguous same-(texture, scissor) UiQuadInstances — mirrors
    // Renderer::TextDrawBatch's shape (one draw call per (format, atlas tile) run) exactly, batching
    // by (texture, active clip rect) instead of (atlas format, tile). Folding the scissor rect into
    // the batch key (rather than tracking it as a separate interleaved op) is what lets Clay's
    // SCISSOR_START/END commands turn into "just another thing a batch break happens on".
    struct UiQuadDrawBatch {
        RHI::TextureViewHandle texture_view{};
        RHI::Rect2D scissor{};
        RHI::BufferHandle instance_buffer{};
        u32 first_instance = 0;
        u32 instance_count = 0;
        struct BoundGroup {
            u32 set = 0;
            RHI::BindGroupHandle handle{};
        };
        vector<BoundGroup> bind_groups;
    };

    // Persistent GPU state for one independently fenced UI draw workload — same N-buffered
    // grow-only-buffer shape as Renderer::TextFrameResources, for the same reason
    // (plans/async-submission-model.md: no CPU stall to hide a write-while-in-flight race behind).
    struct UiQuadFrameResources {
        struct BindingCacheEntry {
            // Bind groups depend only on the texture (the buffer/sampler are the same every batch),
            // so the cache key is texture-only even though a draw batch's key also includes scissor.
            RHI::TextureViewHandle texture_view{};
            vector<UiQuadDrawBatch::BoundGroup> bind_groups;
        };

        RHI::BufferHandle instance_buffer{};
        u64 instance_capacity_bytes = 0;
        vector<UiQuadInstance> uploaded_instances;
        vector<BindingCacheEntry> binding_cache;
    };

    void destroy_ui_quad_frame_resources(RHI::RhiDevice &device, UiQuadFrameResources &resources) noexcept;

    // The instanced quad GPU pipeline (Shaders/ui_quad.slang): one render pipeline, alpha blended,
    // no depth test, driving vertex-pulled instanced draws — one per contiguous same-texture run of
    // rect/border/image render commands.
    class UiQuadPipeline {
      public:
        UiQuadPipeline() noexcept = default;

        [[nodiscard]] static Core::RendererExpected<UiQuadPipeline> create(RHI::RhiDevice &device, RHI::Format color_format);

        // `instance_texture_views[i]` is the texture UiInstances[i] samples — UiRenderer supplies
        // Renderer's shared default-white-texture view (Renderer::ensure_default_white_texture())
        // for plain rects/borders, so the shader always samples a texture rather than branching on
        // "is this a rect or an image", which is what keeps rects and images on one shader/pipeline.
        // `instance_scissors[i]` is the active clip rect UiInstances[i] was declared under. Forms
        // consecutive same-(texture, scissor) batches without reordering painter order, mirroring
        // Renderer::TextPipeline::prepare()'s (format, tile) batching.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, span<const UiQuadInstance> instances,
                                                    span<const RHI::TextureViewHandle> instance_texture_views,
                                                    span<const RHI::Rect2D> instance_scissors,
                                                    UiQuadFrameResources &resources, vector<UiQuadDrawBatch> &out_batches);

        // Sets each batch's scissor rect before drawing it — a caller does not need to (and should
        // not) call pass.set_scissor() itself around draw().
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, span<const UiQuadDrawBatch> batches,
                                                 glm::vec2 viewport_size);

        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct ResourceBinding {
            usize layout_index = 0;
            u32 binding = 0;
            bool found = false;
        };

        RHI::ShaderModuleHandle vertex_module_{};
        RHI::ShaderModuleHandle fragment_module_{};
        RHI::PipelineLayoutHandle pipeline_layout_{};
        RHI::RenderPipelineHandle pipeline_{};
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts_;
        vector<u32> bind_group_layout_sets_;
        RHI::SamplerHandle sampler_{};
        ResourceBinding instances_binding_{};
        ResourceBinding texture_binding_{};
        ResourceBinding sampler_binding_{};
    };

} // namespace SFT::UI
