module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <memory>
#include <span>
#pragma endregion

export module Sturdy.RHI:Command;

import :Error;
import :Types;
import :Handles;
import :Shader;
import :Resources; // Filter (blit), TextureAspect helpers
import :Queues;
import :Barrier;
import :Queries;
import :RayTracing;

using std::span;
using std::unique_ptr;

export namespace SFT::RHI {

    // ─── Attachments / render pass ───────────────────────────────────────────────

    // What happens to an attachment's existing contents when a render pass begins.
    enum class LoadOp : u32 {
        Load,     // preserve what's already there
        Clear,    // clear to the attachment's clear value
        DontCare, // contents are undefined — cheapest when the pass fully overwrites
    };

    // What happens to an attachment's contents when a render pass ends.
    enum class StoreOp : u32 {
        Store,    // keep the rendered result
        DontCare, // result may be discarded (e.g. a transient depth buffer)
    };

    struct ColorAttachment {
        TextureViewHandle view{};
        // Set for MSAA: `view` is the multisample target, `resolve_view` the single-sample
        // destination the pass resolves into. Null when not resolving.
        TextureViewHandle resolve_view{};
        LoadOp load_op = LoadOp::Clear;
        StoreOp store_op = StoreOp::Store;
        ClearColor clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct DepthStencilAttachment {
        TextureViewHandle view{};
        LoadOp depth_load_op = LoadOp::Clear;
        StoreOp depth_store_op = StoreOp::Store;
        LoadOp stencil_load_op = LoadOp::DontCare;
        StoreOp stencil_store_op = StoreOp::DontCare;
        ClearDepthStencil clear_value{};
    };

    // A dynamic-rendering pass: the attachments to render into this pass, plus the render area. This
    // is the RHI's only rendering-scope primitive — there is no separate render-pass/framebuffer
    // object, matching the engine's deliberate all-dynamic-rendering stance (legacy render passes
    // were removed for the debuggability cost they impose). `color_attachments` is non-owning
    // (consumed by begin_render_pass()); `depth_stencil` is optional (null `view` => none).
    struct RenderPassDesc {
        span<const ColorAttachment> color_attachments;
        DepthStencilAttachment depth_stencil{};
        // Zero-sized render_area => cover the full attachment extent.
        Rect2D render_area{};
        // Multiview view mask (requires Feature::Multiview): each set bit renders one view into the
        // matching attachment array layer in a single pass — single-pass cascaded shadow maps, cubemap
        // shadows, stereo VR. 0 disables multiview. Pipelines executed here must have a matching
        // RenderPipelineDesc::view_mask.
        u32 view_mask = 0;
        const char *label = nullptr;
    };

    // Compatibility contract for a render bundle: the attachment formats/sample count of the render
    // pass it will later execute inside. This is the portable shape of Vulkan secondary-command-buffer
    // inheritance, D3D12 bundles, Metal parallel render encoders, and WebGPU render bundles.
    struct RenderBundleDesc {
        span<const Format> color_formats;
        Format depth_stencil_format = Format::Undefined;
        SampleCount samples = SampleCount::X1;
        // View mask of the passes this bundle will execute inside (must match their RenderPassDesc /
        // pipeline view masks). 0 for non-multiview passes.
        u32 view_mask = 0;
        const char *label = nullptr;
    };

    struct ComputePassDesc {
        const char *label = nullptr;
    };

    // ─── Copy regions ────────────────────────────────────────────────────────────

    struct BufferCopy {
        u64 src_offset = 0;
        u64 dst_offset = 0;
        u64 size = 0;
    };

    // A rectangular region of a buffer <-> texture transfer. `buffer_row_length`/
    // `buffer_image_height` describe the buffer-side packing (0 = tightly packed to `extent`).
    struct BufferTextureCopy {
        u64 buffer_offset = 0;
        u32 buffer_row_length = 0;
        u32 buffer_image_height = 0;
        u32 mip_level = 0;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;
        Offset3D texture_offset{};
        Extent3D texture_extent{};
    };

    // One mip/layer slice of a texture, as a copy/blit/clear source or destination.
    struct TextureSubresourceLayers {
        u32 mip_level = 0;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;
    };

    // A same-size texture->texture copy region (a raw texel copy — matching formats/sizes, no scaling
    // or filtering). The bloom/SSR ping-pong and history-buffer copies of a deferred pipeline.
    struct TextureCopy {
        TextureSubresourceLayers src_subresource{};
        Offset3D src_offset{};
        TextureSubresourceLayers dst_subresource{};
        Offset3D dst_offset{};
        Extent3D extent{};
    };

    // A scaled, optionally-filtered texture->texture blit region (`src`/`dst` rectangles may differ in
    // size). The mip-generation and bloom down/up-sample workhorse — `filter` picks nearest vs linear.
    struct TextureBlit {
        TextureSubresourceLayers src_subresource{};
        Offset3D src_min{};
        Offset3D src_max{};
        TextureSubresourceLayers dst_subresource{};
        Offset3D dst_min{};
        Offset3D dst_max{};
    };

    // ─── Draw / dispatch parameter blocks ────────────────────────────────────────

    struct DrawArgs {
        u32 vertex_count = 0;
        u32 instance_count = 1;
        u32 first_vertex = 0;
        u32 first_instance = 0;
    };

    struct DrawIndexedArgs {
        u32 index_count = 0;
        u32 instance_count = 1;
        u32 first_index = 0;
        i32 base_vertex = 0;
        u32 first_instance = 0;
    };

    struct DrawMeshTasksArgs {
        u32 group_count_x = 1;
        u32 group_count_y = 1;
        u32 group_count_z = 1;
    };

    // ─── Encoders ────────────────────────────────────────────────────────────────

    // Records draws within one render pass. Obtained from CommandEncoder::begin_render_pass() and
    // valid only until end() — every draw between begin and end targets that pass's attachments.
    // Viewport and scissor are dynamic state set here, not baked into the pipeline. Setters take
    // effect for subsequent draws (classic bind-then-draw state machine).
    class RenderPassEncoder {
      public:
        virtual ~RenderPassEncoder() = default;

        virtual void set_pipeline(RenderPipelineHandle pipeline) = 0;

        // Binds `bind_group` at set index `index`. `dynamic_offsets` supplies, in binding order, one
        // offset per has_dynamic_offset entry in the group's layout (empty if it has none).
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;

        virtual void set_vertex_buffer(u32 slot, BufferHandle buffer, u64 offset = 0) = 0;
        virtual void set_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset = 0) = 0;

        // Inline constants for `stages`, at byte `offset` into the layout's push-constant space.
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;

        virtual void set_viewport(const Viewport &viewport) = 0;
        virtual void set_scissor(const Rect2D &scissor) = 0;
        // Blend constant referenced by BlendFactor::ConstantColor, and the dynamic stencil reference.
        virtual void set_blend_constant(const ClearColor &color) = 0;
        virtual void set_stencil_reference(u32 reference) = 0;

        virtual void draw(const DrawArgs &args) = 0;
        virtual void draw_indexed(const DrawIndexedArgs &args) = 0;

        // Mesh-shader draw path (requires Feature::MeshShader; Feature::TaskShader if the bound
        // pipeline has a task/amplification stage). Maps to vkCmdDrawMeshTasksEXT,
        // ID3D12GraphicsCommandList6::DispatchMesh, and Metal drawMeshThreadgroups.
        virtual void draw_mesh_tasks(const DrawMeshTasksArgs &args) = 0;

        // Single indirect draw: reads one arg block from `indirect_buffer` at `offset` (an
        // Indirect-usage buffer). The GPU-driven single-draw case.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset) = 0;

        // Batched multi-draw indirect: `draw_count` consecutive arg blocks, `stride` bytes apart,
        // in one call (requires Feature::MultiDrawIndirect). One dispatch of many instanced batches
        // without per-batch CPU overhead — the foliage/scene-submission path.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;

        // GPU-provided draw count: renders up to `max_draws` arg blocks, the actual number read as a
        // u32 from `count_buffer` at `count_offset` (requires Feature::DrawIndirectCount). This is
        // the fully GPU-driven case — a compute cull pass writes both the arg blocks and the count,
        // and the CPU never learns how many batches survived (e.g. frustum/occlusion-culled foliage).
        virtual void draw_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                         BufferHandle count_buffer, u64 count_offset,
                                         u32 max_draws, u32 stride) = 0;
        virtual void draw_indexed_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                 BufferHandle count_buffer, u64 count_offset,
                                                 u32 max_draws, u32 stride) = 0;
        virtual void draw_mesh_tasks_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        virtual void draw_mesh_tasks_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                    BufferHandle count_buffer, u64 count_offset,
                                                    u32 max_draws, u32 stride) = 0;

        // Executes pre-recorded render bundles inside this pass (requires Feature::RenderBundles).
        // Bundles must have been created with a RenderBundleDesc compatible with this pass's
        // attachment formats/sample count. This is the safe in-pass CPU parallelism hook: worker
        // threads build bundles independently, then the main pass encoder orders them explicitly here.
        virtual void execute_bundles(span<const RenderBundleHandle> bundles) = 0;

        // Occlusion query scope (requires Feature::OcclusionQueries): the draws recorded between begin
        // and end accumulate their passing-sample count into slot `index` of `query_set` (a
        // QueryType::Occlusion set). The occlusion-culling primitive — resolve the counts and skip
        // fully-occluded draws next frame. Not nestable; one query is open at a time.
        virtual void begin_occlusion_query(QuerySetHandle query_set, u32 index) = 0;
        virtual void end_occlusion_query() = 0;

        // Ends the pass. The encoder must not be used afterward.
        virtual void end() = 0;
    };

    // Records a render-only command chunk that can be built on any worker thread and executed later
    // inside a compatible RenderPassEncoder. It has no attachments of its own and cannot perform
    // barriers/copies/presentation; those stay on primary CommandEncoder/RenderPassEncoder objects so
    // ordering remains explicit. The encoder is single-threaded, but many bundle encoders may be used
    // concurrently by different threads.
    class RenderBundleEncoder {
      public:
        virtual ~RenderBundleEncoder() = default;

        virtual void set_pipeline(RenderPipelineHandle pipeline) = 0;
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;
        virtual void set_vertex_buffer(u32 slot, BufferHandle buffer, u64 offset = 0) = 0;
        virtual void set_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset = 0) = 0;
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;
        virtual void set_viewport(const Viewport &viewport) = 0;
        virtual void set_scissor(const Rect2D &scissor) = 0;
        virtual void set_blend_constant(const ClearColor &color) = 0;
        virtual void set_stencil_reference(u32 reference) = 0;

        virtual void draw(const DrawArgs &args) = 0;
        virtual void draw_indexed(const DrawIndexedArgs &args) = 0;
        virtual void draw_mesh_tasks(const DrawMeshTasksArgs &args) = 0;
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        virtual void draw_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                         BufferHandle count_buffer, u64 count_offset,
                                         u32 max_draws, u32 stride) = 0;
        virtual void draw_indexed_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                 BufferHandle count_buffer, u64 count_offset,
                                                 u32 max_draws, u32 stride) = 0;
        virtual void draw_mesh_tasks_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        virtual void draw_mesh_tasks_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                    BufferHandle count_buffer, u64 count_offset,
                                                    u32 max_draws, u32 stride) = 0;

        [[nodiscard]] virtual RhiExpected<RenderBundleHandle> finish() = 0;
    };

    // Records dispatches within one compute pass. Same lifetime rule as RenderPassEncoder.
    class ComputePassEncoder {
      public:
        virtual ~ComputePassEncoder() = default;

        virtual void set_pipeline(ComputePipelineHandle pipeline) = 0;
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;

        virtual void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) = 0;
        virtual void dispatch_indirect(BufferHandle indirect_buffer, u64 offset) = 0;

        virtual void end() = 0;
    };

    enum class CommandBufferUsage : u32 {
        OneTimeSubmit,
        MultiSubmit,
    };

    struct CommandEncoderDesc {
        QueueLane queue{};
        CommandBufferUsage usage = CommandBufferUsage::OneTimeSubmit;
        const char *label = nullptr;
    };

    // Records a batch of GPU work into one command buffer. Produced by
    // RhiDevice::create_command_encoder() (see :Device); a render/compute pass is opened for
    // rendering/dispatch, transfers are recorded directly, and finish() seals it into a
    // CommandBufferHandle you hand to RhiDevice::submit(). The parent encoder must outlive any
    // pass encoder it hands out.
    //
    // Threading contract: a single CommandEncoder (and any pass encoder borrowed from it) is
    // single-threaded. Parallel CPU recording is achieved by creating one encoder per worker/thread —
    // targeting the same queue lane for serial GPU execution, or different queue lanes/classes when the
    // enabled features/topology allow overlap.
    class CommandEncoder {
      public:
        virtual ~CommandEncoder() = default;

        [[nodiscard]] virtual RhiExpected<unique_ptr<RenderPassEncoder>> begin_render_pass(const RenderPassDesc &desc) = 0;
        [[nodiscard]] virtual RhiExpected<unique_ptr<ComputePassEncoder>> begin_compute_pass(const ComputePassDesc &desc) = 0;

        virtual void copy_buffer_to_buffer(BufferHandle src, BufferHandle dst, const BufferCopy &region) = 0;
        virtual void copy_buffer_to_texture(BufferHandle src, TextureHandle dst, const BufferTextureCopy &region) = 0;
        virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy &region) = 0;
        // Raw same-size texel copy between textures (matching formats, no scaling) — history/ping-pong
        // copies. Both textures need the matching TransferSrc/TransferDst usage and layouts.
        virtual void copy_texture_to_texture(TextureHandle src, TextureHandle dst, const TextureCopy &region) = 0;
        // Scaled, filtered copy between textures — the mip-generation and bloom down/up-sample path.
        // `filter` is Nearest for integer/depth formats, Linear for the usual color downsample.
        virtual void blit_texture(TextureHandle src, TextureHandle dst, const TextureBlit &region, Filter filter) = 0;

        // ── Inline buffer writes / clears (recorded outside any pass) ──
        // Sets `size` bytes of `buffer` at `offset` to the repeating 32-bit `value` (offset/size must
        // be 4-byte aligned; size 0 => to the end). Resets GPU counters / indirect-draw-count buffers
        // each frame — the GPU-driven-culling reset step.
        virtual void fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) = 0;
        // Records a small inline data upload straight into the command stream (no staging buffer). For
        // tiny per-frame constants only — backends cap this (Vulkan: 65536 bytes, `data` 4-byte sized).
        virtual void update_buffer(BufferHandle buffer, u64 offset, span<const std::byte> data) = 0;

        // ── Texture clears (recorded outside any pass) ──
        // Clears a color / depth-stencil texture subresource directly (vkCmdClearColorImage-style),
        // for clearing a storage image or accumulation target that isn't cleared via a render-pass
        // LoadOp. The texture must be in a transfer/general layout. Attachment clears still belong on
        // the render pass (ColorAttachment::load_op) — this is for the non-attachment case.
        virtual void clear_color_texture(TextureHandle texture, const ClearColor &color,
                                         const TextureSubresourceRange &range) = 0;
        virtual void clear_depth_stencil_texture(TextureHandle texture, const ClearDepthStencil &value,
                                                 const TextureSubresourceRange &range) = 0;

        // Ray tracing work (requires Feature::RayQuery and/or Feature::RayTracingPipeline as
        // appropriate): AS builds/copies are explicit GPU work using caller-provided scratch buffers;
        // trace_rays() dispatches a ray tracing pipeline through shader binding table regions.
        virtual void build_acceleration_structures(span<const AccelerationStructureBuildDesc> builds) = 0;
        virtual void copy_acceleration_structure(const AccelerationStructureCopyDesc &copy) = 0;
        virtual void trace_rays(const TraceRaysDesc &desc) = 0;

        // Records explicit execution/memory dependencies (see :Barrier) — the caller-stated sync the
        // RHI's explicit model requires between, e.g., a compute pass writing a texture/buffer and a
        // later pass reading it, or an attachment→sampled layout transition. Recorded outside any
        // render/compute pass. Batched: pass all barriers that share a dependency point together, so
        // the backend can coalesce them into one API call.
        virtual void barrier(span<const GlobalBarrier> global_barriers,
                             span<const BufferBarrier> buffer_barriers,
                             span<const TextureBarrier> texture_barriers) = 0;

        // ── Queries ──
        // Clears query slots `[first, first+count)` back to the unwritten state. Required before a
        // slot is (re)written unless the device advertises Feature::HostQueryReset and the caller
        // resets on the host — record this at the top of a frame's use of the set.
        virtual void reset_query_set(QuerySetHandle query_set, u32 first, u32 count) = 0;
        // Writes the GPU timestamp at `stage` into slot `index` of a QueryType::Timestamp set. Two of
        // these bracketing a pass, times DeviceLimits' timestamp period, give that pass's GPU duration —
        // the per-pass profiling every frame graph reports.
        virtual void write_timestamp(PipelineStage stage, QuerySetHandle query_set, u32 index) = 0;
        // Pipeline-statistics scope (requires Feature::PipelineStatisticsQueries): the draws/dispatches
        // between begin and end accumulate the set's configured counters into slot `index`. May wrap a
        // whole render pass, so it lives here rather than on the render-pass encoder.
        virtual void begin_pipeline_statistics_query(QuerySetHandle query_set, u32 index) = 0;
        virtual void end_pipeline_statistics_query() = 0;
        // Copies resolved results for slots `[first, first+count)` into `dst` at `dst_offset`, `stride`
        // bytes apart (GPU-side, no host stall). Pair with QueryResultFlags::Result64Bit/WithAvailability
        // to control element width and the trailing availability integer.
        virtual void resolve_query_set(QuerySetHandle query_set, u32 first, u32 count,
                                       BufferHandle dst, u64 dst_offset, u64 stride,
                                       QueryResultFlags flags = QueryResultFlags::Result64Bit) = 0;

        // Optional debug marker scope — surfaced to captures/validation (RenderDoc, VK_EXT_debug_utils).
        virtual void push_debug_group(const char *label) = 0;
        virtual void pop_debug_group() = 0;

        // Seals recording and yields a submittable command buffer. The encoder must not be used
        // afterward.
        [[nodiscard]] virtual RhiExpected<CommandBufferHandle> finish() = 0;
    };

} // namespace SFT::RHI
