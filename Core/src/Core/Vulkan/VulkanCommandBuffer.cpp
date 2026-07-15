#include "VulkanCommandBuffer.hpp"

namespace SFT::Core::Vulkan {

VulkanCommandBuffer::~VulkanCommandBuffer() { destroy(); }

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&o) noexcept
            : device_(o.device_), command_pool_(o.command_pool_), buffer_(o.buffer_), level_(o.level_) {
            o.device_ = VK_NULL_HANDLE;
            o.command_pool_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

VulkanCommandBuffer &VulkanCommandBuffer::operator=(VulkanCommandBuffer &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                command_pool_ = o.command_pool_;
                buffer_ = o.buffer_;
                level_ = o.level_;
                o.device_ = VK_NULL_HANDLE;
                o.command_pool_ = VK_NULL_HANDLE;
                o.buffer_ = VK_NULL_HANDLE;
                o.level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanCommandBuffer> VulkanCommandBuffer::allocate(
            VkDevice device,
            VkCommandPool command_pool,
            VkCommandBufferLevel level) noexcept {
            VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = command_pool,
                .level = level,
                .commandBufferCount = 1,
            };

            VkCommandBuffer buffer = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(device, &info, &buffer) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");

            VulkanCommandBuffer out;
            out.device_ = device;
            out.command_pool_ = command_pool;
            out.buffer_ = buffer;
            out.level_ = level;
            return out;
        }

[[nodiscard]] VkCommandBuffer VulkanCommandBuffer::vk_handle() const noexcept { return buffer_; }

[[nodiscard]] VkCommandBufferSubmitInfo VulkanCommandBuffer::submit_info() const noexcept {
            return VkCommandBufferSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = buffer_,
                .deviceMask = 0,
            };
        }

[[nodiscard]] bool VulkanCommandBuffer::is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }

[[nodiscard]] VkCommandPool VulkanCommandBuffer::command_pool() const noexcept { return command_pool_; }

[[nodiscard]] VkCommandBufferLevel VulkanCommandBuffer::level() const noexcept { return level_; }

[[nodiscard]] RendererResult VulkanCommandBuffer::begin(VkCommandBufferUsageFlags flags) noexcept {
            return begin_inherited(flags, nullptr, nullptr);
        }

[[nodiscard]] RendererResult VulkanCommandBuffer::begin_inherited(VkCommandBufferUsageFlags flags,
                                                     const VkCommandBufferInheritanceInfo *inheritance,
                                                     const void *pnext) noexcept {
            VkCommandBufferBeginInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = pnext,
                .flags = flags,
                .pInheritanceInfo = inheritance,
            };
            if (vkBeginCommandBuffer(buffer_, &info) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBeginCommandBuffer failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanCommandBuffer::end() noexcept {
            if (vkEndCommandBuffer(buffer_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkEndCommandBuffer failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanCommandBuffer::reset(VkCommandBufferResetFlags flags) noexcept {
            if (vkResetCommandBuffer(buffer_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandBuffer failed.");
            return {};
        }

void VulkanCommandBuffer::pipeline_barrier2(const VkDependencyInfo &dependency_info) const noexcept {
            vkCmdPipelineBarrier2(buffer_, &dependency_info);
        }

void VulkanCommandBuffer::pipeline_barrier2(span<const VkMemoryBarrier2> memory_barriers,
                               span<const VkBufferMemoryBarrier2> buffer_barriers,
                               span<const VkImageMemoryBarrier2> image_barriers,
                               VkDependencyFlags flags) const noexcept {
            VkDependencyInfo dependency_info{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = flags,
                .memoryBarrierCount = static_cast<u32>(memory_barriers.size()),
                .pMemoryBarriers = memory_barriers.empty() ? nullptr : memory_barriers.data(),
                .bufferMemoryBarrierCount = static_cast<u32>(buffer_barriers.size()),
                .pBufferMemoryBarriers = buffer_barriers.empty() ? nullptr : buffer_barriers.data(),
                .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
                .pImageMemoryBarriers = image_barriers.empty() ? nullptr : image_barriers.data(),
            };
            vkCmdPipelineBarrier2(buffer_, &dependency_info);
        }

void VulkanCommandBuffer::pipeline_barrier2(span<const VkImageMemoryBarrier2> image_barriers) const noexcept {
            pipeline_barrier2({}, {}, image_barriers);
        }

void VulkanCommandBuffer::pipeline_barrier2(const vector<VkImageMemoryBarrier2> &image_barriers) const noexcept {
            pipeline_barrier2(span<const VkImageMemoryBarrier2>{image_barriers.data(), image_barriers.size()});
        }

void VulkanCommandBuffer::set_event2(VkEvent event, const VkDependencyInfo &dependency) const noexcept {
            vkCmdSetEvent2(buffer_, event, &dependency);
        }

void VulkanCommandBuffer::reset_event2(VkEvent event, VkPipelineStageFlags2 stage) const noexcept {
            vkCmdResetEvent2(buffer_, event, stage);
        }

void VulkanCommandBuffer::wait_events2(span<const VkEvent> events, span<const VkDependencyInfo> dependencies) const noexcept {
            vkCmdWaitEvents2(buffer_, static_cast<u32>(events.size()), events.data(), dependencies.data());
        }

void VulkanCommandBuffer::begin_rendering(const VkRenderingInfo &info) const noexcept {
            vkCmdBeginRendering(buffer_, &info);
        }

void VulkanCommandBuffer::end_rendering() const noexcept {
            vkCmdEndRendering(buffer_);
        }

void VulkanCommandBuffer::set_viewport(const VkViewport &viewport) const noexcept {
            vkCmdSetViewport(buffer_, 0, 1, &viewport);
        }

void VulkanCommandBuffer::set_scissor(const VkRect2D &scissor) const noexcept {
            vkCmdSetScissor(buffer_, 0, 1, &scissor);
        }

void VulkanCommandBuffer::set_viewports(span<const VkViewport> viewports, u32 first_viewport) const noexcept {
            vkCmdSetViewport(buffer_, first_viewport, static_cast<u32>(viewports.size()), viewports.data());
        }

void VulkanCommandBuffer::set_scissors(span<const VkRect2D> scissors, u32 first_scissor) const noexcept {
            vkCmdSetScissor(buffer_, first_scissor, static_cast<u32>(scissors.size()), scissors.data());
        }

void VulkanCommandBuffer::set_viewport_with_count(span<const VkViewport> viewports) const noexcept {
            vkCmdSetViewportWithCount(buffer_, static_cast<u32>(viewports.size()), viewports.data());
        }

void VulkanCommandBuffer::set_scissor_with_count(span<const VkRect2D> scissors) const noexcept {
            vkCmdSetScissorWithCount(buffer_, static_cast<u32>(scissors.size()), scissors.data());
        }

void VulkanCommandBuffer::set_cull_mode(VkCullModeFlags mode) const noexcept { vkCmdSetCullMode(buffer_, mode); }

void VulkanCommandBuffer::set_front_face(VkFrontFace face) const noexcept { vkCmdSetFrontFace(buffer_, face); }

void VulkanCommandBuffer::set_primitive_topology(VkPrimitiveTopology topology) const noexcept {
            vkCmdSetPrimitiveTopology(buffer_, topology);
        }

void VulkanCommandBuffer::set_primitive_restart_enable(bool enable) const noexcept {
            vkCmdSetPrimitiveRestartEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_rasterizer_discard_enable(bool enable) const noexcept {
            vkCmdSetRasterizerDiscardEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_depth_test_enable(bool enable) const noexcept {
            vkCmdSetDepthTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_depth_write_enable(bool enable) const noexcept {
            vkCmdSetDepthWriteEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_depth_compare_op(VkCompareOp op) const noexcept { vkCmdSetDepthCompareOp(buffer_, op); }

void VulkanCommandBuffer::set_depth_bounds_test_enable(bool enable) const noexcept {
            vkCmdSetDepthBoundsTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_depth_bias_enable(bool enable) const noexcept {
            vkCmdSetDepthBiasEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_stencil_test_enable(bool enable) const noexcept {
            vkCmdSetStencilTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

void VulkanCommandBuffer::set_stencil_op(VkStencilFaceFlags faces, VkStencilOp fail_op, VkStencilOp pass_op,
                            VkStencilOp depth_fail_op, VkCompareOp compare_op) const noexcept {
            vkCmdSetStencilOp(buffer_, faces, fail_op, pass_op, depth_fail_op, compare_op);
        }

void VulkanCommandBuffer::set_line_width(float width) const noexcept { vkCmdSetLineWidth(buffer_, width); }

void VulkanCommandBuffer::set_depth_bias(float constant, float clamp, float slope) const noexcept {
            vkCmdSetDepthBias(buffer_, constant, clamp, slope);
        }

void VulkanCommandBuffer::set_depth_bounds(float min_depth, float max_depth) const noexcept {
            vkCmdSetDepthBounds(buffer_, min_depth, max_depth);
        }

void VulkanCommandBuffer::set_blend_constants(const float constants[4]) const noexcept {
            vkCmdSetBlendConstants(buffer_, constants);
        }

void VulkanCommandBuffer::set_stencil_reference(VkStencilFaceFlags faces, u32 reference) const noexcept {
            vkCmdSetStencilReference(buffer_, faces, reference);
        }

void VulkanCommandBuffer::set_stencil_compare_mask(VkStencilFaceFlags faces, u32 mask) const noexcept {
            vkCmdSetStencilCompareMask(buffer_, faces, mask);
        }

void VulkanCommandBuffer::set_stencil_write_mask(VkStencilFaceFlags faces, u32 mask) const noexcept {
            vkCmdSetStencilWriteMask(buffer_, faces, mask);
        }

void VulkanCommandBuffer::set_fragment_shading_rate(VkExtent2D fragment_size,
                                       const VkFragmentShadingRateCombinerOpKHR combiners[2]) const noexcept {
            if (vkCmdSetFragmentShadingRateKHR == nullptr) {
                return;
            }
            vkCmdSetFragmentShadingRateKHR(buffer_, &fragment_size, combiners);
        }

void VulkanCommandBuffer::bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept {
            vkCmdBindPipeline(buffer_, bind_point, pipeline);
        }

void VulkanCommandBuffer::bind_descriptor_sets(VkPipelineBindPoint bind_point,
                                  VkPipelineLayout layout,
                                  u32 first_set,
                                  span<const VkDescriptorSet> sets,
                                  span<const u32> dynamic_offsets) const noexcept {
            vkCmdBindDescriptorSets(buffer_, bind_point, layout, first_set,
                                    static_cast<u32>(sets.size()), sets.empty() ? nullptr : sets.data(),
                                    static_cast<u32>(dynamic_offsets.size()),
                                    dynamic_offsets.empty() ? nullptr : dynamic_offsets.data());
        }

void VulkanCommandBuffer::push_constants(VkPipelineLayout layout,
                            VkShaderStageFlags stages,
                            u32 offset,
                            u32 size,
                            const void *data) const noexcept {
            vkCmdPushConstants(buffer_, layout, stages, offset, size, data);
        }

void VulkanCommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) const noexcept {
            vkCmdDraw(buffer_, vertex_count, instance_count, first_vertex, first_instance);
        }

void VulkanCommandBuffer::bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset, u32 binding) const noexcept {
            vkCmdBindVertexBuffers(buffer_, binding, 1, &buffer, &offset);
        }

void VulkanCommandBuffer::bind_vertex_buffers(span<const VkBuffer> buffers,
                                 span<const VkDeviceSize> offsets,
                                 u32 first_binding) const noexcept {
            vkCmdBindVertexBuffers(buffer_, first_binding, static_cast<u32>(buffers.size()),
                                   buffers.data(), offsets.data());
        }

void VulkanCommandBuffer::bind_index_buffer(VkBuffer buffer, VkIndexType index_type, VkDeviceSize offset) const noexcept {
            vkCmdBindIndexBuffer(buffer_, buffer, offset, index_type);
        }

void VulkanCommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance) const noexcept {
            vkCmdDrawIndexed(buffer_, index_count, instance_count, first_index, vertex_offset, first_instance);
        }

void VulkanCommandBuffer::draw_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            vkCmdDrawIndirect(buffer_, indirect_buffer, offset, draw_count, stride);
        }

void VulkanCommandBuffer::draw_indexed_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            vkCmdDrawIndexedIndirect(buffer_, indirect_buffer, offset, draw_count, stride);
        }

void VulkanCommandBuffer::draw_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                 VkBuffer count_buffer, VkDeviceSize count_offset,
                                 u32 max_draws, u32 stride) const noexcept {
            vkCmdDrawIndirectCount(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

void VulkanCommandBuffer::draw_indexed_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                         VkBuffer count_buffer, VkDeviceSize count_offset,
                                         u32 max_draws, u32 stride) const noexcept {
            vkCmdDrawIndexedIndirectCount(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

void VulkanCommandBuffer::draw_mesh_tasks(u32 group_count_x, u32 group_count_y, u32 group_count_z) const noexcept {
            if (vkCmdDrawMeshTasksEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksEXT(buffer_, group_count_x, group_count_y, group_count_z);
        }

void VulkanCommandBuffer::draw_mesh_tasks_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            if (vkCmdDrawMeshTasksIndirectEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksIndirectEXT(buffer_, indirect_buffer, offset, draw_count, stride);
        }

void VulkanCommandBuffer::draw_mesh_tasks_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                            VkBuffer count_buffer, VkDeviceSize count_offset,
                                            u32 max_draws, u32 stride) const noexcept {
            if (vkCmdDrawMeshTasksIndirectCountEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksIndirectCountEXT(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

void VulkanCommandBuffer::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const noexcept {
            vkCmdDispatch(buffer_, group_count_x, group_count_y, group_count_z);
        }

void VulkanCommandBuffer::dispatch_indirect(VkBuffer indirect_buffer, VkDeviceSize offset) const noexcept {
            vkCmdDispatchIndirect(buffer_, indirect_buffer, offset);
        }

void VulkanCommandBuffer::copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize src_offset, VkDeviceSize dst_offset) const noexcept {
            VkBufferCopy region{.srcOffset = src_offset, .dstOffset = dst_offset, .size = size};
            vkCmdCopyBuffer(buffer_, src, dst, 1, &region);
        }

void VulkanCommandBuffer::copy_buffer(VkBuffer src, VkBuffer dst, span<const VkBufferCopy> regions) const noexcept {
            vkCmdCopyBuffer(buffer_, src, dst, static_cast<u32>(regions.size()), regions.data());
        }

void VulkanCommandBuffer::fill_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 value) const noexcept {
            vkCmdFillBuffer(buffer_, dst, offset, size, value);
        }

void VulkanCommandBuffer::update_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) const noexcept {
            vkCmdUpdateBuffer(buffer_, dst, offset, size, data);
        }

void VulkanCommandBuffer::copy_buffer_to_image(VkBuffer src,
                                  VkImage dst,
                                  VkImageLayout dst_layout,
                                  span<const VkBufferImageCopy> regions) const noexcept {
            vkCmdCopyBufferToImage(buffer_, src, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

void VulkanCommandBuffer::copy_image_to_buffer(VkImage src,
                                  VkImageLayout src_layout,
                                  VkBuffer dst,
                                  span<const VkBufferImageCopy> regions) const noexcept {
            vkCmdCopyImageToBuffer(buffer_, src, src_layout, dst, static_cast<u32>(regions.size()), regions.data());
        }

void VulkanCommandBuffer::copy_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageCopy> regions) const noexcept {
            vkCmdCopyImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

void VulkanCommandBuffer::blit_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageBlit> regions,
                        VkFilter filter) const noexcept {
            vkCmdBlitImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data(), filter);
        }

void VulkanCommandBuffer::resolve_image(VkImage src,
                           VkImageLayout src_layout,
                           VkImage dst,
                           VkImageLayout dst_layout,
                           span<const VkImageResolve> regions) const noexcept {
            vkCmdResolveImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

void VulkanCommandBuffer::clear_color_image(VkImage image,
                               VkImageLayout layout,
                               const VkClearColorValue &color,
                               span<const VkImageSubresourceRange> ranges) const noexcept {
            vkCmdClearColorImage(buffer_, image, layout, &color, static_cast<u32>(ranges.size()), ranges.data());
        }

void VulkanCommandBuffer::clear_depth_stencil_image(VkImage image,
                                       VkImageLayout layout,
                                       const VkClearDepthStencilValue &clear,
                                       span<const VkImageSubresourceRange> ranges) const noexcept {
            vkCmdClearDepthStencilImage(buffer_, image, layout, &clear, static_cast<u32>(ranges.size()), ranges.data());
        }

void VulkanCommandBuffer::reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) const noexcept {
            vkCmdResetQueryPool(buffer_, pool, first_query, query_count);
        }

void VulkanCommandBuffer::write_timestamp2(VkPipelineStageFlagBits2 stage, VkQueryPool pool, u32 query) const noexcept {
            vkCmdWriteTimestamp2(buffer_, stage, pool, query);
        }

void VulkanCommandBuffer::begin_query(VkQueryPool pool, u32 query, VkQueryControlFlags flags) const noexcept {
            vkCmdBeginQuery(buffer_, pool, query, flags);
        }

void VulkanCommandBuffer::end_query(VkQueryPool pool, u32 query) const noexcept {
            vkCmdEndQuery(buffer_, pool, query);
        }

void VulkanCommandBuffer::copy_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count,
                                     VkBuffer dst, VkDeviceSize dst_offset, VkDeviceSize stride,
                                     VkQueryResultFlags flags) const noexcept {
            vkCmdCopyQueryPoolResults(buffer_, pool, first_query, query_count, dst, dst_offset, stride, flags);
        }

void VulkanCommandBuffer::execute_commands(span<const VkCommandBuffer> secondaries) const noexcept {
            vkCmdExecuteCommands(buffer_, static_cast<u32>(secondaries.size()), secondaries.data());
        }

void VulkanCommandBuffer::begin_conditional_rendering(VkBuffer buffer, VkDeviceSize offset,
                                         VkConditionalRenderingFlagsEXT flags) const noexcept {
            if (vkCmdBeginConditionalRenderingEXT == nullptr) {
                return;
            }
            const VkConditionalRenderingBeginInfoEXT info{
                .sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
                .pNext = nullptr,
                .buffer = buffer,
                .offset = offset,
                .flags = flags,
            };
            vkCmdBeginConditionalRenderingEXT(buffer_, &info);
        }

void VulkanCommandBuffer::end_conditional_rendering() const noexcept {
            if (vkCmdEndConditionalRenderingEXT != nullptr) {
                vkCmdEndConditionalRenderingEXT(buffer_);
            }
        }

void VulkanCommandBuffer::build_acceleration_structures(
            span<const VkAccelerationStructureBuildGeometryInfoKHR> builds,
            span<const VkAccelerationStructureBuildRangeInfoKHR *const> ranges) const noexcept {
            if (vkCmdBuildAccelerationStructuresKHR == nullptr) {
                return;
            }
            vkCmdBuildAccelerationStructuresKHR(buffer_, static_cast<u32>(builds.size()), builds.data(), ranges.data());
        }

void VulkanCommandBuffer::copy_acceleration_structure(const VkCopyAccelerationStructureInfoKHR &info) const noexcept {
            if (vkCmdCopyAccelerationStructureKHR != nullptr) {
                vkCmdCopyAccelerationStructureKHR(buffer_, &info);
            }
        }

void VulkanCommandBuffer::trace_rays(const VkStridedDeviceAddressRegionKHR &raygen,
                        const VkStridedDeviceAddressRegionKHR &miss,
                        const VkStridedDeviceAddressRegionKHR &hit,
                        const VkStridedDeviceAddressRegionKHR &callable,
                        u32 width, u32 height, u32 depth) const noexcept {
            if (vkCmdTraceRaysKHR == nullptr) {
                return;
            }
            vkCmdTraceRaysKHR(buffer_, &raygen, &miss, &hit, &callable, width, height, depth);
        }

void VulkanCommandBuffer::trace_rays_indirect(const VkStridedDeviceAddressRegionKHR &raygen,
                                 const VkStridedDeviceAddressRegionKHR &miss,
                                 const VkStridedDeviceAddressRegionKHR &hit,
                                 const VkStridedDeviceAddressRegionKHR &callable,
                                 VkDeviceAddress indirect_device_address) const noexcept {
            if (vkCmdTraceRaysIndirectKHR == nullptr) {
                return;
            }
            vkCmdTraceRaysIndirectKHR(buffer_, &raygen, &miss, &hit, &callable, indirect_device_address);
        }

void VulkanCommandBuffer::begin_debug_label(const char *label,
                               float r,
                               float g,
                               float b,
                               float a) const noexcept {
            if (vkCmdBeginDebugUtilsLabelEXT == nullptr || label == nullptr) {
                return;
            }
            const VkDebugUtilsLabelEXT info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pNext = nullptr,
                .pLabelName = label,
                .color = {r, g, b, a},
            };
            vkCmdBeginDebugUtilsLabelEXT(buffer_, &info);
        }

void VulkanCommandBuffer::end_debug_label() const noexcept {
            if (vkCmdEndDebugUtilsLabelEXT != nullptr) {
                vkCmdEndDebugUtilsLabelEXT(buffer_);
            }
        }

void VulkanCommandBuffer::insert_debug_label(const char *label,
                                float r,
                                float g,
                                float b,
                                float a) const noexcept {
            if (vkCmdInsertDebugUtilsLabelEXT == nullptr || label == nullptr) {
                return;
            }
            const VkDebugUtilsLabelEXT info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pNext = nullptr,
                .pLabelName = label,
                .color = {r, g, b, a},
            };
            vkCmdInsertDebugUtilsLabelEXT(buffer_, &info);
        }

void VulkanCommandBuffer::destroy() noexcept {
            if (buffer_ == VK_NULL_HANDLE)
                return;
            vkFreeCommandBuffers(device_, command_pool_, 1, &buffer_);
            buffer_ = VK_NULL_HANDLE;
            command_pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

} // namespace SFT::Core::Vulkan
