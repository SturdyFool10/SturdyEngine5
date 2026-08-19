#pragma once

#include <Foundation/Foundation.hpp>
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
        /// Constructs a `VulkanCommandBuffer` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanCommandBuffer() = default;
        /// Destroys the `VulkanCommandBuffer` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanCommandBuffer();

        /// Disables this construction form for `VulkanCommandBuffer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;
        /// Assigns a new value to this `VulkanCommandBuffer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;

        /// Constructs a `VulkanCommandBuffer` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanCommandBuffer(VulkanCommandBuffer &&o) noexcept;

        /// Assigns a new value to this `VulkanCommandBuffer`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanCommandBuffer &operator=(VulkanCommandBuffer &&o) noexcept;

        /// Allocates storage or a resource.
        ///
        /// @param device Device used or affected by the operation.
        /// @param command_pool `command_pool` value used by the operation.
        /// @param level `level` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanCommandBuffer> allocate(
            VkDevice device,
            VkCommandPool command_pool,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanCommandBuffer`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandBuffer vk_handle() const noexcept;

        /// Returns the current submit info.
        ///
        /// @return Returns the current submit info value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandBufferSubmitInfo submit_info() const noexcept;
        /// Reports whether valid holds for this `VulkanCommandBuffer`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the current or globally available command pool value.
        ///
        /// @return Returns the current command pool value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandPool command_pool() const noexcept;
        /// Returns the current or globally available level value.
        ///
        /// @return Returns the current level value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandBufferLevel level() const noexcept;

        /// Returns an iterator to the first element in the range.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult begin(VkCommandBufferUsageFlags flags = 0) noexcept;

        /// Performs the begin inherited operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param flags Flags controlling optional behavior.
        /// @param inheritance `inheritance` value used by the operation.
        /// @param pnext `pnext` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult begin_inherited(VkCommandBufferUsageFlags flags,
                                                     const VkCommandBufferInheritanceInfo *inheritance,
                                                     const void *pnext = nullptr) noexcept;

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult end() noexcept;

        /// Resets the object to its baseline state.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset(VkCommandBufferResetFlags flags = 0) noexcept;

        /// Performs the pipeline barrier2 operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param dependency_info Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
        void pipeline_barrier2(const VkDependencyInfo &dependency_info) const noexcept;

        /// Performs the pipeline barrier2 operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param memory_barriers `memory_barriers` value used by the operation.
        /// @param buffer_barriers Buffer used or affected by the operation.
        /// @param image_barriers `image_barriers` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void pipeline_barrier2(span<const VkMemoryBarrier2> memory_barriers,
                               span<const VkBufferMemoryBarrier2> buffer_barriers,
                               span<const VkImageMemoryBarrier2> image_barriers,
                               VkDependencyFlags flags = 0) const noexcept;


        /// Performs the pipeline barrier2 operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param image_barriers `image_barriers` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void pipeline_barrier2(span<const VkImageMemoryBarrier2> image_barriers) const noexcept;
        /// Performs the pipeline barrier2 operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param image_barriers `image_barriers` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void pipeline_barrier2(const vector<VkImageMemoryBarrier2> &image_barriers) const noexcept;


        /// Sets the event2 for this `VulkanCommandBuffer`.
        ///
        /// @param event Event used or affected by the operation.
        /// @param dependency `dependency` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_event2(VkEvent event, const VkDependencyInfo &dependency) const noexcept;
        /// Resets event2 to its baseline state.
        ///
        /// @param event Event used or affected by the operation.
        /// @param stage `stage` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void reset_event2(VkEvent event, VkPipelineStageFlags2 stage) const noexcept;
        /// Waits for events2 to complete.
        ///
        /// @param events Event used or affected by the operation.
        /// @param dependencies `dependencies` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void wait_events2(span<const VkEvent> events, span<const VkDependencyInfo> dependencies) const noexcept;

        /// Performs the begin rendering operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
        void begin_rendering(const VkRenderingInfo &info) const noexcept;

        /// Performs the end rendering operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void end_rendering() const noexcept;

        /// Sets the viewport for this `VulkanCommandBuffer`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_viewport(const VkViewport &viewport) const noexcept;

        /// Sets the scissor for this `VulkanCommandBuffer`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_scissor(const VkRect2D &scissor) const noexcept;


        /// Sets the viewports for this `VulkanCommandBuffer`.
        ///
        /// @param viewports `viewports` value used by the operation.
        /// @param first_viewport `first_viewport` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_viewports(span<const VkViewport> viewports, u32 first_viewport = 0) const noexcept;
        /// Sets the scissors for this `VulkanCommandBuffer`.
        ///
        /// @param scissors `scissors` value used by the operation.
        /// @param first_scissor `first_scissor` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_scissors(span<const VkRect2D> scissors, u32 first_scissor = 0) const noexcept;


        /// Sets the viewport with count for this `VulkanCommandBuffer`.
        ///
        /// @param viewports `viewports` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_viewport_with_count(span<const VkViewport> viewports) const noexcept;
        /// Sets the scissor with count for this `VulkanCommandBuffer`.
        ///
        /// @param scissors `scissors` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_scissor_with_count(span<const VkRect2D> scissors) const noexcept;
        /// Sets the cull mode for this `VulkanCommandBuffer`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @note This function does not throw exceptions.
        void set_cull_mode(VkCullModeFlags mode) const noexcept;
        /// Sets the front face for this `VulkanCommandBuffer`.
        ///
        /// @param face `face` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_front_face(VkFrontFace face) const noexcept;
        /// Sets the primitive topology for this `VulkanCommandBuffer`.
        ///
        /// @param topology `topology` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_primitive_topology(VkPrimitiveTopology topology) const noexcept;
        /// Sets the primitive restart enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_primitive_restart_enable(bool enable) const noexcept;
        /// Sets the rasterizer discard enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_rasterizer_discard_enable(bool enable) const noexcept;
        /// Sets the depth test enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_test_enable(bool enable) const noexcept;
        /// Sets the depth write enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_write_enable(bool enable) const noexcept;
        /// Sets the depth compare op for this `VulkanCommandBuffer`.
        ///
        /// @param op `op` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_compare_op(VkCompareOp op) const noexcept;
        /// Sets the depth bounds test enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_bounds_test_enable(bool enable) const noexcept;
        /// Sets the depth bias enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_bias_enable(bool enable) const noexcept;
        /// Sets the stencil test enable for this `VulkanCommandBuffer`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_test_enable(bool enable) const noexcept;
        /// Sets the stencil op for this `VulkanCommandBuffer`.
        ///
        /// @param faces `faces` value used by the operation.
        /// @param fail_op `fail_op` value used by the operation.
        /// @param pass_op `pass_op` value used by the operation.
        /// @param depth_fail_op `depth_fail_op` value used by the operation.
        /// @param compare_op `compare_op` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_op(VkStencilFaceFlags faces, VkStencilOp fail_op, VkStencilOp pass_op,
                            VkStencilOp depth_fail_op, VkCompareOp compare_op) const noexcept;


        /// Sets the line width for this `VulkanCommandBuffer`.
        ///
        /// @param width Width of the target extent.
        ///
        /// @note This function does not throw exceptions.
        void set_line_width(float width) const noexcept;
        /// Sets the depth bias for this `VulkanCommandBuffer`.
        ///
        /// @param constant `constant` value used by the operation.
        /// @param clamp `clamp` value used by the operation.
        /// @param slope `slope` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_bias(float constant, float clamp, float slope) const noexcept;
        /// Sets the depth bounds for this `VulkanCommandBuffer`.
        ///
        /// @param min_depth `min_depth` value used by the operation.
        /// @param max_depth `max_depth` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_bounds(float min_depth, float max_depth) const noexcept;
        /// Sets custom MSAA sample positions for this `VulkanCommandBuffer`.
        ///
        /// @param info `info` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_sample_locations(const VkSampleLocationsInfoEXT &info) const noexcept;
        /// Sets the blend constants for this `VulkanCommandBuffer`.
        ///
        /// @param constants `constants` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_blend_constants(const float constants[4]) const noexcept;
        /// Sets the stencil reference for this `VulkanCommandBuffer`.
        ///
        /// @param faces `faces` value used by the operation.
        /// @param reference `reference` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_reference(VkStencilFaceFlags faces, u32 reference) const noexcept;
        /// Sets the stencil compare mask for this `VulkanCommandBuffer`.
        ///
        /// @param faces `faces` value used by the operation.
        /// @param mask `mask` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_compare_mask(VkStencilFaceFlags faces, u32 mask) const noexcept;
        /// Sets the stencil write mask for this `VulkanCommandBuffer`.
        ///
        /// @param faces `faces` value used by the operation.
        /// @param mask `mask` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_write_mask(VkStencilFaceFlags faces, u32 mask) const noexcept;


        /// Sets the fragment shading rate for this `VulkanCommandBuffer`.
        ///
        /// @param fragment_size Requested or available size for the operation.
        /// @param combiners `combiners` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_fragment_shading_rate(VkExtent2D fragment_size,
                                       const VkFragmentShadingRateCombinerOpKHR combiners[2]) const noexcept;

        /// Binds pipeline for subsequent operations.
        ///
        /// @param bind_point `bind_point` value used by the operation.
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept;

        /// Binds descriptor sets for subsequent operations.
        ///
        /// @param bind_point `bind_point` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param first_set `first_set` value used by the operation.
        /// @param sets `sets` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void bind_descriptor_sets(VkPipelineBindPoint bind_point,
                                  VkPipelineLayout layout,
                                  u32 first_set,
                                  span<const VkDescriptorSet> sets,
                                  span<const u32> dynamic_offsets = {}) const noexcept;

        /// Adds the supplied value to the end or work queue.
        ///
        /// @param layout `layout` value used by the operation.
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function does not throw exceptions.
        void push_constants(VkPipelineLayout layout,
                            VkShaderStageFlags stages,
                            u32 offset,
                            u32 size,
                            const void *data) const noexcept;

        /// Draws the requested content using the current rendering state.
        ///
        /// @param vertex_count Number of elements or operations to process.
        /// @param instance_count Number of elements or operations to process.
        /// @param first_vertex `first_vertex` value used by the operation.
        /// @param first_instance Instance used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0) const noexcept;

        /// Binds vertex buffer for subsequent operations.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param binding `binding` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset = 0, u32 binding = 0) const noexcept;


        /// Binds vertex buffers for subsequent operations.
        ///
        /// @param buffers Buffer used or affected by the operation.
        /// @param offsets Offset from the beginning of the relevant range or buffer.
        /// @param first_binding `first_binding` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void bind_vertex_buffers(span<const VkBuffer> buffers,
                                 span<const VkDeviceSize> offsets,
                                 u32 first_binding = 0) const noexcept;

        /// Binds index buffer for subsequent operations.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param index_type Zero-based index of the target element or entry.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void bind_index_buffer(VkBuffer buffer, VkIndexType index_type, VkDeviceSize offset = 0) const noexcept;

        /// Draws indexed using the current rendering state.
        ///
        /// @param index_count Zero-based index of the target element or entry.
        /// @param instance_count Number of elements or operations to process.
        /// @param first_index Zero-based index of the target element or entry.
        /// @param vertex_offset Offset from the beginning of the relevant range or buffer.
        /// @param first_instance Instance used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0, i32 vertex_offset = 0, u32 first_instance = 0) const noexcept;

        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;

        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;


        /// Returns the draw indirect count for this `VulkanCommandBuffer`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                 VkBuffer count_buffer, VkDeviceSize count_offset,
                                 u32 max_draws, u32 stride) const noexcept;
        /// Returns the draw indexed indirect count for this `VulkanCommandBuffer`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                         VkBuffer count_buffer, VkDeviceSize count_offset,
                                         u32 max_draws, u32 stride) const noexcept;


        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param group_count_x `group_count_x` value used by the operation.
        /// @param group_count_y `group_count_y` value used by the operation.
        /// @param group_count_z `group_count_z` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) const noexcept;
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept;
        /// Returns the draw mesh tasks indirect count for this `VulkanCommandBuffer`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                            VkBuffer count_buffer, VkDeviceSize count_offset,
                                            u32 max_draws, u32 stride) const noexcept;

        /// Dispatches the requested work.
        ///
        /// @param group_count_x `group_count_x` value used by the operation.
        /// @param group_count_y `group_count_y` value used by the operation.
        /// @param group_count_z `group_count_z` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) const noexcept;

        /// Dispatches indirect.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void dispatch_indirect(VkBuffer indirect_buffer, VkDeviceSize offset) const noexcept;


        /// Copies buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param size Requested or available size for the operation.
        /// @param src_offset Offset from the beginning of the relevant range or buffer.
        /// @param dst_offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize src_offset = 0, VkDeviceSize dst_offset = 0) const noexcept;

        /// Copies buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param regions `regions` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void copy_buffer(VkBuffer src, VkBuffer dst, span<const VkBufferCopy> regions) const noexcept;


        /// Fills buffer using the supplied arguments and current state.
        ///
        /// @param dst Destination value or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void fill_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 value) const noexcept;


        /// Updates buffer from the supplied values.
        ///
        /// @param dst Destination value or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function does not throw exceptions.
        void update_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) const noexcept;

        /// Copies buffer to image to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param dst_layout `dst_layout` value used by the operation.
        /// @param regions `regions` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void copy_buffer_to_image(VkBuffer src,
                                  VkImage dst,
                                  VkImageLayout dst_layout,
                                  span<const VkBufferImageCopy> regions) const noexcept;

        /// Copies image to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param src_layout `src_layout` value used by the operation.
        /// @param dst Destination value or resource.
        /// @param regions `regions` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void copy_image_to_buffer(VkImage src,
                                  VkImageLayout src_layout,
                                  VkBuffer dst,
                                  span<const VkBufferImageCopy> regions) const noexcept;

        /// Copies image to its destination.
        ///
        /// @param src Source value or resource.
        /// @param src_layout `src_layout` value used by the operation.
        /// @param dst Destination value or resource.
        /// @param dst_layout `dst_layout` value used by the operation.
        /// @param regions `regions` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void copy_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageCopy> regions) const noexcept;

        /// Performs the blit image operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param src Source value or resource.
        /// @param src_layout `src_layout` value used by the operation.
        /// @param dst Destination value or resource.
        /// @param dst_layout `dst_layout` value used by the operation.
        /// @param regions `regions` value used by the operation.
        /// @param filter `filter` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void blit_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageBlit> regions,
                        VkFilter filter = VK_FILTER_LINEAR) const noexcept;

        /// Resolves image into the concrete value used by the engine.
        ///
        /// @param src Source value or resource.
        /// @param src_layout `src_layout` value used by the operation.
        /// @param dst Destination value or resource.
        /// @param dst_layout `dst_layout` value used by the operation.
        /// @param regions `regions` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void resolve_image(VkImage src,
                           VkImageLayout src_layout,
                           VkImage dst,
                           VkImageLayout dst_layout,
                           span<const VkImageResolve> regions) const noexcept;

        /// Clears color image.
        ///
        /// @param image `image` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param color `color` value used by the operation.
        /// @param ranges `ranges` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void clear_color_image(VkImage image,
                               VkImageLayout layout,
                               const VkClearColorValue &color,
                               span<const VkImageSubresourceRange> ranges) const noexcept;

        /// Clears depth stencil image.
        ///
        /// @param image `image` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param clear `clear` value used by the operation.
        /// @param ranges `ranges` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void clear_depth_stencil_image(VkImage image,
                                       VkImageLayout layout,
                                       const VkClearDepthStencilValue &clear,
                                       span<const VkImageSubresourceRange> ranges) const noexcept;

        /// Resets query pool to its baseline state.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param first_query `first_query` value used by the operation.
        /// @param query_count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        void reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) const noexcept;

        /// Writes timestamp2 to the associated destination.
        ///
        /// @param stage `stage` value used by the operation.
        /// @param pool `pool` value used by the operation.
        /// @param query `query` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void write_timestamp2(VkPipelineStageFlagBits2 stage, VkQueryPool pool, u32 query) const noexcept;


        /// Performs the begin query operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param query `query` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void begin_query(VkQueryPool pool, u32 query, VkQueryControlFlags flags = 0) const noexcept;
        /// Performs the end query operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param query `query` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void end_query(VkQueryPool pool, u32 query) const noexcept;


        /// Copies query pool results to its destination.
        ///
        /// @param pool `pool` value used by the operation.
        /// @param first_query `first_query` value used by the operation.
        /// @param query_count Number of elements or operations to process.
        /// @param dst Destination value or resource.
        /// @param dst_offset Offset from the beginning of the relevant range or buffer.
        /// @param stride `stride` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        void copy_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count,
                                     VkBuffer dst, VkDeviceSize dst_offset, VkDeviceSize stride,
                                     VkQueryResultFlags flags) const noexcept;


        /// Executes commands.
        ///
        /// @param secondaries `secondaries` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void execute_commands(span<const VkCommandBuffer> secondaries) const noexcept;


        /// Performs the begin conditional rendering operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void begin_conditional_rendering(VkBuffer buffer, VkDeviceSize offset,
                                         VkConditionalRenderingFlagsEXT flags = 0) const noexcept;
        /// Performs the end conditional rendering operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void end_conditional_rendering() const noexcept;


        /// Builds acceleration structures.
        ///
        /// @param builds `builds` value used by the operation.
        /// @param ranges `ranges` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void build_acceleration_structures(
            span<const VkAccelerationStructureBuildGeometryInfoKHR> builds,
            span<const VkAccelerationStructureBuildRangeInfoKHR *const> ranges) const noexcept;
        /// Builds opacity micromaps.
        ///
        /// @param builds `builds` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void build_opacity_micromaps(span<const VkMicromapBuildInfoEXT> builds) const noexcept;
        /// Copies acceleration structure to its destination.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @note This function does not throw exceptions.
        void copy_acceleration_structure(const VkCopyAccelerationStructureInfoKHR &info) const noexcept;

        /// Traces rays using the supplied arguments and current state.
        ///
        /// @param raygen `raygen` value used by the operation.
        /// @param miss `miss` value used by the operation.
        /// @param hit `hit` value used by the operation.
        /// @param callable `callable` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param depth Depth of the target extent.
        ///
        /// @note This function does not throw exceptions.
        void trace_rays(const VkStridedDeviceAddressRegionKHR &raygen,
                        const VkStridedDeviceAddressRegionKHR &miss,
                        const VkStridedDeviceAddressRegionKHR &hit,
                        const VkStridedDeviceAddressRegionKHR &callable,
                        u32 width, u32 height, u32 depth = 1) const noexcept;
        /// Traces rays indirect using the supplied arguments and current state.
        ///
        /// @param raygen `raygen` value used by the operation.
        /// @param miss `miss` value used by the operation.
        /// @param hit `hit` value used by the operation.
        /// @param callable `callable` value used by the operation.
        /// @param indirect_device_address Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void trace_rays_indirect(const VkStridedDeviceAddressRegionKHR &raygen,
                                 const VkStridedDeviceAddressRegionKHR &miss,
                                 const VkStridedDeviceAddressRegionKHR &hit,
                                 const VkStridedDeviceAddressRegionKHR &callable,
                                 VkDeviceAddress indirect_device_address) const noexcept;

        /// Performs the begin debug label operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @param label `label` value used by the operation.
        /// @param r `r` value used by the operation.
        /// @param g `g` value used by the operation.
        /// @param b `b` value used by the operation.
        /// @param a `a` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void begin_debug_label(const char *label,
                               float r = 1.0f,
                               float g = 1.0f,
                               float b = 1.0f,
                               float a = 1.0f) const noexcept;

        /// Performs the end debug label operation for `VulkanCommandBuffer` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void end_debug_label() const noexcept;

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param label `label` value used by the operation.
        /// @param r `r` value used by the operation.
        /// @param g `g` value used by the operation.
        /// @param b `b` value used by the operation.
        /// @param a `a` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void insert_debug_label(const char *label,
                                float r = 1.0f,
                                float g = 1.0f,
                                float b = 1.0f,
                                float a = 1.0f) const noexcept;

        /// Destroys or releases the `VulkanCommandBuffer` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        VkCommandBuffer buffer_ = VK_NULL_HANDLE;
        VkCommandBufferLevel level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    };

} // namespace SFT::Core::Vulkan
