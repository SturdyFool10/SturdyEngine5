#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    class VulkanCommandBuffer {
      public:
        VulkanCommandBuffer() = default;
        ~VulkanCommandBuffer();

        VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;
        VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;

        VulkanCommandBuffer(VulkanCommandBuffer &&o) noexcept;

        VulkanCommandBuffer &operator=(VulkanCommandBuffer &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanCommandBuffer> allocate(
            VkDevice device,
            VkCommandPool command_pool,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) noexcept;

        [[nodiscard]] VkCommandBuffer vk_handle() const noexcept;

        [[nodiscard]] VkCommandBufferSubmitInfo submit_info() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkCommandPool command_pool() const noexcept;
        [[nodiscard]] VkCommandBufferLevel level() const noexcept;

        [[nodiscard]] RendererResult begin(VkCommandBufferUsageFlags flags = 0) noexcept;

        [[nodiscard]] RendererResult begin_inherited(VkCommandBufferUsageFlags flags,
                                                     const VkCommandBufferInheritanceInfo *inheritance,
                                                     const void *pnext = nullptr) noexcept;

        [[nodiscard]] RendererResult end() noexcept;

        [[nodiscard]] RendererResult reset(VkCommandBufferResetFlags flags = 0) noexcept;

        void pipeline_barrier2(const VkDependencyInfo &dependency_info) const noexcept;

        void pipeline_barrier2(span<const VkMemoryBarrier2> memory_barriers,
                               span<const VkBufferMemoryBarrier2> buffer_barriers,
                               span<const VkImageMemoryBarrier2> image_barriers,
                               VkDependencyFlags flags = 0) const noexcept;

        // Records an image-memory-barrier-only synchronization2 dependency.
        void pipeline_barrier2(span<const VkImageMemoryBarrier2> image_barriers) const noexcept;
        void pipeline_barrier2(const vector<VkImageMemoryBarrier2> &image_barriers) const noexcept;

        // ── Split barriers via events (synchronization2) ──
        // Signal `event` once the src half of `dependency` completes; a later wait_events2 on the same
        // event enforces the dst half, leaving the span between free for unrelated overlap.
        void set_event2(VkEvent event, const VkDependencyInfo &dependency) const noexcept;
        void reset_event2(VkEvent event, VkPipelineStageFlags2 stage) const noexcept;
        void wait_events2(span<const VkEvent> events, span<const VkDependencyInfo> dependencies) const noexcept;

        void begin_rendering(const VkRenderingInfo &info) const noexcept;

        void end_rendering() const noexcept;

        void set_viewport(const VkViewport &viewport) const noexcept;

        void set_scissor(const VkRect2D &scissor) const noexcept;

        // Multi-viewport / multi-scissor (MultiViewport feature). `first_viewport` is the base index;
        // the arrays supply consecutive entries — the shadow-cascade / layered-render fan-out path.
        void set_viewports(span<const VkViewport> viewports, u32 first_viewport = 0) const noexcept;
        void set_scissors(span<const VkRect2D> scissors, u32 first_scissor = 0) const noexcept;

        // ── Extended dynamic state (core in Vulkan 1.3, always available at our 1.4 baseline) ──
        // These move fixed-function state out of the pipeline object and onto the command stream, so
        // one pipeline can serve many state permutations — the flexibility the RHI wants when it maps a
        // descriptor's rasterization/depth/stencil fields to dynamic state rather than baking a variant.
        void set_viewport_with_count(span<const VkViewport> viewports) const noexcept;
        void set_scissor_with_count(span<const VkRect2D> scissors) const noexcept;
        void set_cull_mode(VkCullModeFlags mode) const noexcept;
        void set_front_face(VkFrontFace face) const noexcept;
        void set_primitive_topology(VkPrimitiveTopology topology) const noexcept;
        void set_primitive_restart_enable(bool enable) const noexcept;
        void set_rasterizer_discard_enable(bool enable) const noexcept;
        void set_depth_test_enable(bool enable) const noexcept;
        void set_depth_write_enable(bool enable) const noexcept;
        void set_depth_compare_op(VkCompareOp op) const noexcept;
        void set_depth_bounds_test_enable(bool enable) const noexcept;
        void set_depth_bias_enable(bool enable) const noexcept;
        void set_stencil_test_enable(bool enable) const noexcept;
        void set_stencil_op(VkStencilFaceFlags faces, VkStencilOp fail_op, VkStencilOp pass_op,
                            VkStencilOp depth_fail_op, VkCompareOp compare_op) const noexcept;

        // ── Dynamic pipeline knobs available since Vulkan 1.0 ──
        void set_line_width(float width) const noexcept;
        void set_depth_bias(float constant, float clamp, float slope) const noexcept;
        void set_depth_bounds(float min_depth, float max_depth) const noexcept;
        void set_blend_constants(const float constants[4]) const noexcept;
        void set_stencil_reference(VkStencilFaceFlags faces, u32 reference) const noexcept;
        void set_stencil_compare_mask(VkStencilFaceFlags faces, u32 mask) const noexcept;
        void set_stencil_write_mask(VkStencilFaceFlags faces, u32 mask) const noexcept;

        // Variable-rate shading: the per-draw shading rate + combiner ops (VariableRateShading /
        // pipeline fragment shading rate). Function-pointer-guarded — null unless the extension loaded.
        void set_fragment_shading_rate(VkExtent2D fragment_size,
                                       const VkFragmentShadingRateCombinerOpKHR combiners[2]) const noexcept;

        void bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept;

        void bind_descriptor_sets(VkPipelineBindPoint bind_point,
                                  VkPipelineLayout layout,
                                  u32 first_set,
                                  span<const VkDescriptorSet> sets,
                                  span<const u32> dynamic_offsets = {}) const noexcept;

        void push_constants(VkPipelineLayout layout,
                            VkShaderStageFlags stages,
                            u32 offset,
                            u32 size,
                            const void *data) const noexcept;

        void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0) const noexcept;

        void bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset = 0, u32 binding = 0) const noexcept;

        // Binds several vertex buffers in one call starting at `first_binding` — the interleaved +
        // per-instance stream setup a real mesh submission uses. `offsets` must match `buffers` in size.
        void bind_vertex_buffers(span<const VkBuffer> buffers,
                                 span<const VkDeviceSize> offsets,
                                 u32 first_binding = 0) const noexcept;

        void bind_index_buffer(VkBuffer buffer, VkIndexType index_type, VkDeviceSize offset = 0) const noexcept;

        void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0, i32 vertex_offset = 0, u32 first_instance = 0) const noexcept;

        void draw_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;

        void draw_indexed_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;

        // GPU-provided draw count (DrawIndirectCount, core since Vulkan 1.2): the actual number of
        // draws is read as a u32 from `count_buffer`, capped at `max_draws`. The fully GPU-driven
        // submission path — a compute cull pass writes both the arg blocks and the count.
        void draw_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                 VkBuffer count_buffer, VkDeviceSize count_offset,
                                 u32 max_draws, u32 stride) const noexcept;
        void draw_indexed_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                         VkBuffer count_buffer, VkDeviceSize count_offset,
                                         u32 max_draws, u32 stride) const noexcept;

        // ── Mesh-shader draw path (MeshShader/TaskShader; function-pointer-guarded) ──
        void draw_mesh_tasks(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) const noexcept;
        void draw_mesh_tasks_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;
        void draw_mesh_tasks_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                            VkBuffer count_buffer, VkDeviceSize count_offset,
                                            u32 max_draws, u32 stride) const noexcept;

        void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) const noexcept;

        void dispatch_indirect(VkBuffer indirect_buffer, VkDeviceSize offset) const noexcept;

        // Whole-region copy, `size` bytes from `src` to `dst`. Higher layers batch copies through
        // RHI command encoders; this low-level wrapper keeps the common single-region helper simple.
        void copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize src_offset = 0, VkDeviceSize dst_offset = 0) const noexcept;

        void copy_buffer(VkBuffer src, VkBuffer dst, span<const VkBufferCopy> regions) const noexcept;

        // Fills `size` bytes of `dst` at `offset` with the repeating 32-bit `value` (offset/size must
        // be 4-byte aligned; VK_WHOLE_SIZE fills to the end). The per-frame GPU-counter / indirect-count
        // reset primitive.
        void fill_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 value) const noexcept;

        // Records a small inline data upload straight into the command stream (no staging buffer).
        // `size` must be ≤ 65536 and a multiple of 4 — for tiny per-frame constants only.
        void update_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) const noexcept;

        void copy_buffer_to_image(VkBuffer src,
                                  VkImage dst,
                                  VkImageLayout dst_layout,
                                  span<const VkBufferImageCopy> regions) const noexcept;

        void copy_image_to_buffer(VkImage src,
                                  VkImageLayout src_layout,
                                  VkBuffer dst,
                                  span<const VkBufferImageCopy> regions) const noexcept;

        void copy_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageCopy> regions) const noexcept;

        void blit_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageBlit> regions,
                        VkFilter filter = VK_FILTER_LINEAR) const noexcept;

        void resolve_image(VkImage src,
                           VkImageLayout src_layout,
                           VkImage dst,
                           VkImageLayout dst_layout,
                           span<const VkImageResolve> regions) const noexcept;

        void clear_color_image(VkImage image,
                               VkImageLayout layout,
                               const VkClearColorValue &color,
                               span<const VkImageSubresourceRange> ranges) const noexcept;

        void clear_depth_stencil_image(VkImage image,
                                       VkImageLayout layout,
                                       const VkClearDepthStencilValue &clear,
                                       span<const VkImageSubresourceRange> ranges) const noexcept;

        void reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) const noexcept;

        void write_timestamp2(VkPipelineStageFlagBits2 stage, VkQueryPool pool, u32 query) const noexcept;

        // Occlusion / pipeline-statistics query scope: the draws between begin and end accumulate into
        // slot `query`. `flags` is VK_QUERY_CONTROL_PRECISE_BIT for exact occlusion sample counts.
        void begin_query(VkQueryPool pool, u32 query, VkQueryControlFlags flags = 0) const noexcept;
        void end_query(VkQueryPool pool, u32 query) const noexcept;
        // GPU-side copy of query results into a buffer (no host stall) — the counterpart to
        // VulkanQueryPool::get_results() when results are consumed on the GPU.
        void copy_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count,
                                     VkBuffer dst, VkDeviceSize dst_offset, VkDeviceSize stride,
                                     VkQueryResultFlags flags) const noexcept;

        // Executes pre-recorded secondary command buffers inside this primary — the backing for render
        // bundles / parallel render-pass recording. Secondaries must have been begun with the matching
        // inheritance info (see begin_inherited).
        void execute_commands(span<const VkCommandBuffer> secondaries) const noexcept;

        // Predicated draws (ConditionalRendering; function-pointer-guarded): draws inside the scope are
        // skipped when the 32-bit value at `buffer`+`offset` is zero. Occlusion-predicated submission.
        void begin_conditional_rendering(VkBuffer buffer, VkDeviceSize offset,
                                         VkConditionalRenderingFlagsEXT flags = 0) const noexcept;
        void end_conditional_rendering() const noexcept;

        // ── Ray tracing (RayTracingPipeline / AccelerationStructures; function-pointer-guarded) ──
        // AS builds are explicit GPU work with caller-owned scratch; `ranges` supplies one primitive
        // range pointer per build (parallel to `builds`).
        void build_acceleration_structures(
            span<const VkAccelerationStructureBuildGeometryInfoKHR> builds,
            span<const VkAccelerationStructureBuildRangeInfoKHR *const> ranges) const noexcept;
        void copy_acceleration_structure(const VkCopyAccelerationStructureInfoKHR &info) const noexcept;
        // Dispatches a ray tracing pipeline through the shader-binding-table regions.
        void trace_rays(const VkStridedDeviceAddressRegionKHR &raygen,
                        const VkStridedDeviceAddressRegionKHR &miss,
                        const VkStridedDeviceAddressRegionKHR &hit,
                        const VkStridedDeviceAddressRegionKHR &callable,
                        u32 width, u32 height, u32 depth = 1) const noexcept;
        void trace_rays_indirect(const VkStridedDeviceAddressRegionKHR &raygen,
                                 const VkStridedDeviceAddressRegionKHR &miss,
                                 const VkStridedDeviceAddressRegionKHR &hit,
                                 const VkStridedDeviceAddressRegionKHR &callable,
                                 VkDeviceAddress indirect_device_address) const noexcept;

        void begin_debug_label(const char *label,
                               float r = 1.0f,
                               float g = 1.0f,
                               float b = 1.0f,
                               float a = 1.0f) const noexcept;

        void end_debug_label() const noexcept;

        void insert_debug_label(const char *label,
                                float r = 1.0f,
                                float g = 1.0f,
                                float b = 1.0f,
                                float a = 1.0f) const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        VkCommandBuffer buffer_ = VK_NULL_HANDLE;
        VkCommandBufferLevel level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    };

} // namespace SFT::Core::Vulkan
