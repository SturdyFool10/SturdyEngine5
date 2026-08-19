#include <Core/Vulkan/VulkanCommandBuffer.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanCommandBuffer::~VulkanCommandBuffer() { destroy(); }

/// Performs the vulkan command buffer operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&o) noexcept
            : device_(o.device_), command_pool_(o.command_pool_), buffer_(o.buffer_), level_(o.level_) {
            ZoneScopedN("VulkanCommandBuffer::VulkanCommandBuffer");
            o.device_ = VK_NULL_HANDLE;
            o.command_pool_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanCommandBuffer &VulkanCommandBuffer::operator=(VulkanCommandBuffer &&o) noexcept {
            ZoneScopedN("VulkanCommandBuffer::operator=");
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

/// Allocates storage or a resource.
///
/// @param device Device used or affected by the operation.
/// @param command_pool `command_pool` value used by the operation.
/// @param level `level` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanCommandBuffer> VulkanCommandBuffer::allocate(
            VkDevice device,
            VkCommandPool command_pool,
            VkCommandBufferLevel level) noexcept {
            ZoneScopedN("VulkanCommandBuffer::allocate");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkCommandBuffer VulkanCommandBuffer::vk_handle() const noexcept { return buffer_; }

/// Returns the current submit info.
///
/// @return Returns the current submit info value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkCommandBufferSubmitInfo VulkanCommandBuffer::submit_info() const noexcept {
            ZoneScopedN("VulkanCommandBuffer::submit_info");
            return VkCommandBufferSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = buffer_,
                .deviceMask = 0,
            };
        }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanCommandBuffer::is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }

/// Returns the current or globally available command pool value.
///
/// @return Returns the current command pool value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkCommandPool VulkanCommandBuffer::command_pool() const noexcept { return command_pool_; }

/// Returns the current or globally available level value.
///
/// @return Returns the current level value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkCommandBufferLevel VulkanCommandBuffer::level() const noexcept { return level_; }

/// Returns an iterator to the first element in the range.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns an iterator referring to the first element.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanCommandBuffer::begin(VkCommandBufferUsageFlags flags) noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin");
            return begin_inherited(flags, nullptr, nullptr);
        }

/// Performs the begin inherited operation for `Vulkan` using the supplied arguments.
///
/// @param flags Flags controlling optional behavior.
/// @param inheritance `inheritance` value used by the operation.
/// @param pnext `pnext` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanCommandBuffer::begin_inherited(VkCommandBufferUsageFlags flags,
                                                     const VkCommandBufferInheritanceInfo *inheritance,
                                                     const void *pnext) noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin_inherited");
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

/// Returns the one-past-the-end iterator for the range.
///
/// @return Returns the one-past-the-end iterator.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanCommandBuffer::end() noexcept {
            ZoneScopedN("VulkanCommandBuffer::end");
            if (vkEndCommandBuffer(buffer_) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkEndCommandBuffer failed.");
            return {};
        }

/// Resets the object to its baseline state.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanCommandBuffer::reset(VkCommandBufferResetFlags flags) noexcept {
            ZoneScopedN("VulkanCommandBuffer::reset");
            if (vkResetCommandBuffer(buffer_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandBuffer failed.");
            return {};
        }

/// Performs the pipeline barrier2 operation for `Vulkan` using the supplied arguments.
///
/// @param dependency_info Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::pipeline_barrier2(const VkDependencyInfo &dependency_info) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::pipeline_barrier2");
            vkCmdPipelineBarrier2(buffer_, &dependency_info);
        }

/// Performs the pipeline barrier2 operation for `Vulkan` using the supplied arguments.
///
/// @param memory_barriers `memory_barriers` value used by the operation.
/// @param buffer_barriers Buffer used or affected by the operation.
/// @param image_barriers `image_barriers` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::pipeline_barrier2(span<const VkMemoryBarrier2> memory_barriers,
                               span<const VkBufferMemoryBarrier2> buffer_barriers,
                               span<const VkImageMemoryBarrier2> image_barriers,
                               VkDependencyFlags flags) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::pipeline_barrier2");
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

/// Performs the pipeline barrier2 operation for `Vulkan` using the supplied arguments.
///
/// @param image_barriers `image_barriers` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::pipeline_barrier2(span<const VkImageMemoryBarrier2> image_barriers) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::pipeline_barrier2");
            pipeline_barrier2({}, {}, image_barriers);
        }

/// Performs the pipeline barrier2 operation for `Vulkan` using the supplied arguments.
///
/// @param image_barriers `image_barriers` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::pipeline_barrier2(const vector<VkImageMemoryBarrier2> &image_barriers) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::pipeline_barrier2");
            pipeline_barrier2(span<const VkImageMemoryBarrier2>{image_barriers.data(), image_barriers.size()});
        }

/// Sets the event2 for this `Vulkan`.
///
/// @param event Event used or affected by the operation.
/// @param dependency `dependency` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_event2(VkEvent event, const VkDependencyInfo &dependency) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_event2");
            vkCmdSetEvent2(buffer_, event, &dependency);
        }

/// Resets event2 to its baseline state.
///
/// @param event Event used or affected by the operation.
/// @param stage `stage` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::reset_event2(VkEvent event, VkPipelineStageFlags2 stage) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::reset_event2");
            vkCmdResetEvent2(buffer_, event, stage);
        }

/// Waits for events2 to complete.
///
/// @param events Event used or affected by the operation.
/// @param dependencies `dependencies` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::wait_events2(span<const VkEvent> events, span<const VkDependencyInfo> dependencies) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::wait_events2");
            vkCmdWaitEvents2(buffer_, static_cast<u32>(events.size()), events.data(), dependencies.data());
        }

/// Performs the begin rendering operation for `Vulkan` using the supplied arguments.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::begin_rendering(const VkRenderingInfo &info) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin_rendering");
            vkCmdBeginRendering(buffer_, &info);
        }

/// Returns the current or globally available end rendering value.
///
/// @return Returns the current end rendering value.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::end_rendering() const noexcept {
            ZoneScopedN("VulkanCommandBuffer::end_rendering");
            vkCmdEndRendering(buffer_);
        }

/// Sets the viewport for this `Vulkan`.
///
/// @param viewport `viewport` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_viewport(const VkViewport &viewport) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_viewport");
            vkCmdSetViewport(buffer_, 0, 1, &viewport);
        }

/// Sets the scissor for this `Vulkan`.
///
/// @param scissor `scissor` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_scissor(const VkRect2D &scissor) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_scissor");
            vkCmdSetScissor(buffer_, 0, 1, &scissor);
        }

/// Sets the viewports for this `Vulkan`.
///
/// @param viewports `viewports` value used by the operation.
/// @param first_viewport `first_viewport` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_viewports(span<const VkViewport> viewports, u32 first_viewport) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_viewports");
            vkCmdSetViewport(buffer_, first_viewport, static_cast<u32>(viewports.size()), viewports.data());
        }

/// Sets the scissors for this `Vulkan`.
///
/// @param scissors `scissors` value used by the operation.
/// @param first_scissor `first_scissor` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_scissors(span<const VkRect2D> scissors, u32 first_scissor) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_scissors");
            vkCmdSetScissor(buffer_, first_scissor, static_cast<u32>(scissors.size()), scissors.data());
        }

/// Sets the viewport with count for this `Vulkan`.
///
/// @param viewports `viewports` value used by the operation.
///
/// @return Returns the requested count or size.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_viewport_with_count(span<const VkViewport> viewports) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_viewport_with_count");
            vkCmdSetViewportWithCount(buffer_, static_cast<u32>(viewports.size()), viewports.data());
        }

/// Sets the scissor with count for this `Vulkan`.
///
/// @param scissors `scissors` value used by the operation.
///
/// @return Returns the requested count or size.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_scissor_with_count(span<const VkRect2D> scissors) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_scissor_with_count");
            vkCmdSetScissorWithCount(buffer_, static_cast<u32>(scissors.size()), scissors.data());
        }

/// Sets the cull mode for this `Vulkan`.
///
/// @param mode Mode controlling how the operation is performed.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_cull_mode(VkCullModeFlags mode) const noexcept { vkCmdSetCullMode(buffer_, mode); }

/// Sets the front face for this `Vulkan`.
///
/// @param face `face` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_front_face(VkFrontFace face) const noexcept { vkCmdSetFrontFace(buffer_, face); }

/// Sets the primitive topology for this `Vulkan`.
///
/// @param topology `topology` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_primitive_topology(VkPrimitiveTopology topology) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_primitive_topology");
            vkCmdSetPrimitiveTopology(buffer_, topology);
        }

/// Sets the primitive restart enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_primitive_restart_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_primitive_restart_enable");
            vkCmdSetPrimitiveRestartEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the rasterizer discard enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_rasterizer_discard_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_rasterizer_discard_enable");
            vkCmdSetRasterizerDiscardEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the depth test enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_test_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_test_enable");
            vkCmdSetDepthTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the depth write enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_write_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_write_enable");
            vkCmdSetDepthWriteEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the depth compare op for this `Vulkan`.
///
/// @param op `op` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_compare_op(VkCompareOp op) const noexcept { vkCmdSetDepthCompareOp(buffer_, op); }

/// Sets the depth bounds test enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_bounds_test_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_bounds_test_enable");
            vkCmdSetDepthBoundsTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the depth bias enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_bias_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_bias_enable");
            vkCmdSetDepthBiasEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the stencil test enable for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_stencil_test_enable(bool enable) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_stencil_test_enable");
            vkCmdSetStencilTestEnable(buffer_, enable ? VK_TRUE : VK_FALSE);
        }

/// Sets the stencil op for this `Vulkan`.
///
/// @param faces `faces` value used by the operation.
/// @param fail_op `fail_op` value used by the operation.
/// @param pass_op `pass_op` value used by the operation.
/// @param depth_fail_op `depth_fail_op` value used by the operation.
/// @param compare_op `compare_op` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_stencil_op(VkStencilFaceFlags faces, VkStencilOp fail_op, VkStencilOp pass_op,
                            VkStencilOp depth_fail_op, VkCompareOp compare_op) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_stencil_op");
            vkCmdSetStencilOp(buffer_, faces, fail_op, pass_op, depth_fail_op, compare_op);
        }

/// Sets the line width for this `Vulkan`.
///
/// @param width Width of the target extent.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_line_width(float width) const noexcept { vkCmdSetLineWidth(buffer_, width); }

/// Sets the depth bias for this `Vulkan`.
///
/// @param constant `constant` value used by the operation.
/// @param clamp `clamp` value used by the operation.
/// @param slope `slope` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_bias(float constant, float clamp, float slope) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_bias");
            vkCmdSetDepthBias(buffer_, constant, clamp, slope);
        }

/// Sets the depth bounds for this `Vulkan`.
///
/// @param min_depth `min_depth` value used by the operation.
/// @param max_depth `max_depth` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_depth_bounds(float min_depth, float max_depth) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_depth_bounds");
            vkCmdSetDepthBounds(buffer_, min_depth, max_depth);
        }

/// Sets custom MSAA sample positions for this `Vulkan`.
///
/// @param info `info` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_sample_locations(const VkSampleLocationsInfoEXT &info) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_sample_locations");
            vkCmdSetSampleLocationsEXT(buffer_, &info);
        }

/// Sets the blend constants for this `Vulkan`.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_blend_constants(const float constants[4]) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_blend_constants");
            vkCmdSetBlendConstants(buffer_, constants);
        }

/// Sets the stencil reference for this `Vulkan`.
///
/// @param faces `faces` value used by the operation.
/// @param reference `reference` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_stencil_reference(VkStencilFaceFlags faces, u32 reference) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_stencil_reference");
            vkCmdSetStencilReference(buffer_, faces, reference);
        }

/// Sets the stencil compare mask for this `Vulkan`.
///
/// @param faces `faces` value used by the operation.
/// @param mask `mask` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_stencil_compare_mask(VkStencilFaceFlags faces, u32 mask) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_stencil_compare_mask");
            vkCmdSetStencilCompareMask(buffer_, faces, mask);
        }

/// Sets the stencil write mask for this `Vulkan`.
///
/// @param faces `faces` value used by the operation.
/// @param mask `mask` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_stencil_write_mask(VkStencilFaceFlags faces, u32 mask) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_stencil_write_mask");
            vkCmdSetStencilWriteMask(buffer_, faces, mask);
        }

/// Sets the fragment shading rate for this `Vulkan`.
///
/// @param fragment_size Requested or available size for the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::set_fragment_shading_rate(VkExtent2D fragment_size,
                                       const VkFragmentShadingRateCombinerOpKHR combiners[2]) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::set_fragment_shading_rate");
            if (vkCmdSetFragmentShadingRateKHR == nullptr) {
                return;
            }
            vkCmdSetFragmentShadingRateKHR(buffer_, &fragment_size, combiners);
        }

/// Binds pipeline for subsequent operations.
///
/// @param bind_point `bind_point` value used by the operation.
/// @param pipeline Pipeline used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::bind_pipeline");
            vkCmdBindPipeline(buffer_, bind_point, pipeline);
        }

/// Binds descriptor sets for subsequent operations.
///
/// @param bind_point `bind_point` value used by the operation.
/// @param layout `layout` value used by the operation.
/// @param first_set `first_set` value used by the operation.
/// @param sets `sets` value used by the operation.
/// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::bind_descriptor_sets(VkPipelineBindPoint bind_point,
                                  VkPipelineLayout layout,
                                  u32 first_set,
                                  span<const VkDescriptorSet> sets,
                                  span<const u32> dynamic_offsets) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::bind_descriptor_sets");
            vkCmdBindDescriptorSets(buffer_, bind_point, layout, first_set,
                                    static_cast<u32>(sets.size()), sets.empty() ? nullptr : sets.data(),
                                    static_cast<u32>(dynamic_offsets.size()),
                                    dynamic_offsets.empty() ? nullptr : dynamic_offsets.data());
        }

/// Adds the supplied value to the end or work queue.
///
/// @param layout `layout` value used by the operation.
/// @param stages `stages` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
/// @param data Data consumed or referenced by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::push_constants(VkPipelineLayout layout,
                            VkShaderStageFlags stages,
                            u32 offset,
                            u32 size,
                            const void *data) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::push_constants");
            vkCmdPushConstants(buffer_, layout, stages, offset, size, data);
        }

/// Draws the requested content using the current rendering state.
///
/// @param vertex_count Number of elements or operations to process.
/// @param instance_count Number of elements or operations to process.
/// @param first_vertex `first_vertex` value used by the operation.
/// @param first_instance Instance used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw");
            vkCmdDraw(buffer_, vertex_count, instance_count, first_vertex, first_instance);
        }

/// Binds vertex buffer for subsequent operations.
///
/// @param buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param binding `binding` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset, u32 binding) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::bind_vertex_buffer");
            vkCmdBindVertexBuffers(buffer_, binding, 1, &buffer, &offset);
        }

/// Binds vertex buffers for subsequent operations.
///
/// @param buffers Buffer used or affected by the operation.
/// @param offsets Offset from the beginning of the relevant range or buffer.
/// @param first_binding `first_binding` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::bind_vertex_buffers(span<const VkBuffer> buffers,
                                 span<const VkDeviceSize> offsets,
                                 u32 first_binding) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::bind_vertex_buffers");
            vkCmdBindVertexBuffers(buffer_, first_binding, static_cast<u32>(buffers.size()),
                                   buffers.data(), offsets.data());
        }

/// Binds index buffer for subsequent operations.
///
/// @param buffer Buffer used or affected by the operation.
/// @param index_type Zero-based index of the target element or entry.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::bind_index_buffer(VkBuffer buffer, VkIndexType index_type, VkDeviceSize offset) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::bind_index_buffer");
            vkCmdBindIndexBuffer(buffer_, buffer, offset, index_type);
        }

/// Draws indexed using the current rendering state.
///
/// @param index_count Zero-based index of the target element or entry.
/// @param instance_count Number of elements or operations to process.
/// @param first_index Zero-based index of the target element or entry.
/// @param vertex_offset Offset from the beginning of the relevant range or buffer.
/// @param first_instance Instance used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_indexed");
            vkCmdDrawIndexed(buffer_, index_count, instance_count, first_index, vertex_offset, first_instance);
        }

/// Draws indirect using the current rendering state.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param draw_count Number of elements or operations to process.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_indirect");
            vkCmdDrawIndirect(buffer_, indirect_buffer, offset, draw_count, stride);
        }

/// Draws indexed indirect using the current rendering state.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param draw_count Number of elements or operations to process.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_indexed_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_indexed_indirect");
            vkCmdDrawIndexedIndirect(buffer_, indirect_buffer, offset, draw_count, stride);
        }

/// Returns the draw indirect count for this `Vulkan`.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param indirect_offset Offset from the beginning of the relevant range or buffer.
/// @param count_buffer Buffer used or affected by the operation.
/// @param count_offset Offset from the beginning of the relevant range or buffer.
/// @param max_draws `max_draws` value used by the operation.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the requested count or size.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                 VkBuffer count_buffer, VkDeviceSize count_offset,
                                 u32 max_draws, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_indirect_count");
            vkCmdDrawIndirectCount(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

/// Returns the draw indexed indirect count for this `Vulkan`.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param indirect_offset Offset from the beginning of the relevant range or buffer.
/// @param count_buffer Buffer used or affected by the operation.
/// @param count_offset Offset from the beginning of the relevant range or buffer.
/// @param max_draws `max_draws` value used by the operation.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the requested count or size.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_indexed_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                         VkBuffer count_buffer, VkDeviceSize count_offset,
                                         u32 max_draws, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_indexed_indirect_count");
            vkCmdDrawIndexedIndirectCount(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

/// Draws mesh tasks using the current rendering state.
///
/// @param group_count_x `group_count_x` value used by the operation.
/// @param group_count_y `group_count_y` value used by the operation.
/// @param group_count_z `group_count_z` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_mesh_tasks(u32 group_count_x, u32 group_count_y, u32 group_count_z) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_mesh_tasks");
            if (vkCmdDrawMeshTasksEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksEXT(buffer_, group_count_x, group_count_y, group_count_z);
        }

/// Draws mesh tasks indirect using the current rendering state.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param draw_count Number of elements or operations to process.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_mesh_tasks_indirect(VkBuffer indirect_buffer, VkDeviceSize offset, u32 draw_count, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_mesh_tasks_indirect");
            if (vkCmdDrawMeshTasksIndirectEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksIndirectEXT(buffer_, indirect_buffer, offset, draw_count, stride);
        }

/// Returns the draw mesh tasks indirect count for this `Vulkan`.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param indirect_offset Offset from the beginning of the relevant range or buffer.
/// @param count_buffer Buffer used or affected by the operation.
/// @param count_offset Offset from the beginning of the relevant range or buffer.
/// @param max_draws `max_draws` value used by the operation.
/// @param stride `stride` value used by the operation.
///
/// @return Returns the requested count or size.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::draw_mesh_tasks_indirect_count(VkBuffer indirect_buffer, VkDeviceSize indirect_offset,
                                            VkBuffer count_buffer, VkDeviceSize count_offset,
                                            u32 max_draws, u32 stride) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::draw_mesh_tasks_indirect_count");
            if (vkCmdDrawMeshTasksIndirectCountEXT == nullptr) {
                return;
            }
            vkCmdDrawMeshTasksIndirectCountEXT(buffer_, indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }

/// Dispatches the requested work.
///
/// @param group_count_x `group_count_x` value used by the operation.
/// @param group_count_y `group_count_y` value used by the operation.
/// @param group_count_z `group_count_z` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::dispatch");
            vkCmdDispatch(buffer_, group_count_x, group_count_y, group_count_z);
        }

/// Dispatches indirect.
///
/// @param indirect_buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::dispatch_indirect(VkBuffer indirect_buffer, VkDeviceSize offset) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::dispatch_indirect");
            vkCmdDispatchIndirect(buffer_, indirect_buffer, offset);
        }

/// Copies buffer to its destination.
///
/// @param src Source value or resource.
/// @param dst Destination value or resource.
/// @param size Requested or available size for the operation.
/// @param src_offset Offset from the beginning of the relevant range or buffer.
/// @param dst_offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize src_offset, VkDeviceSize dst_offset) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_buffer");
            VkBufferCopy region{.srcOffset = src_offset, .dstOffset = dst_offset, .size = size};
            vkCmdCopyBuffer(buffer_, src, dst, 1, &region);
        }

/// Copies buffer to its destination.
///
/// @param src Source value or resource.
/// @param dst Destination value or resource.
/// @param regions `regions` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_buffer(VkBuffer src, VkBuffer dst, span<const VkBufferCopy> regions) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_buffer");
            vkCmdCopyBuffer(buffer_, src, dst, static_cast<u32>(regions.size()), regions.data());
        }

/// Fills buffer using the supplied arguments and current state.
///
/// @param dst Destination value or resource.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
/// @param value Value consumed by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::fill_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 value) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::fill_buffer");
            vkCmdFillBuffer(buffer_, dst, offset, size, value);
        }

/// Updates buffer from the supplied values.
///
/// @param dst Destination value or resource.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
/// @param data Data consumed or referenced by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::update_buffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::update_buffer");
            vkCmdUpdateBuffer(buffer_, dst, offset, size, data);
        }

/// Copies buffer to image to its destination.
///
/// @param src Source value or resource.
/// @param dst Destination value or resource.
/// @param dst_layout `dst_layout` value used by the operation.
/// @param regions `regions` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_buffer_to_image(VkBuffer src,
                                  VkImage dst,
                                  VkImageLayout dst_layout,
                                  span<const VkBufferImageCopy> regions) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_buffer_to_image");
            vkCmdCopyBufferToImage(buffer_, src, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

/// Copies image to buffer to its destination.
///
/// @param src Source value or resource.
/// @param src_layout `src_layout` value used by the operation.
/// @param dst Destination value or resource.
/// @param regions `regions` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_image_to_buffer(VkImage src,
                                  VkImageLayout src_layout,
                                  VkBuffer dst,
                                  span<const VkBufferImageCopy> regions) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_image_to_buffer");
            vkCmdCopyImageToBuffer(buffer_, src, src_layout, dst, static_cast<u32>(regions.size()), regions.data());
        }

/// Copies image to its destination.
///
/// @param src Source value or resource.
/// @param src_layout `src_layout` value used by the operation.
/// @param dst Destination value or resource.
/// @param dst_layout `dst_layout` value used by the operation.
/// @param regions `regions` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageCopy> regions) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_image");
            vkCmdCopyImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

/// Performs the blit image operation for `Vulkan` using the supplied arguments.
///
/// @param src Source value or resource.
/// @param src_layout `src_layout` value used by the operation.
/// @param dst Destination value or resource.
/// @param dst_layout `dst_layout` value used by the operation.
/// @param regions `regions` value used by the operation.
/// @param filter `filter` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::blit_image(VkImage src,
                        VkImageLayout src_layout,
                        VkImage dst,
                        VkImageLayout dst_layout,
                        span<const VkImageBlit> regions,
                        VkFilter filter) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::blit_image");
            vkCmdBlitImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data(), filter);
        }

/// Resolves image into the concrete value used by the engine.
///
/// @param src Source value or resource.
/// @param src_layout `src_layout` value used by the operation.
/// @param dst Destination value or resource.
/// @param dst_layout `dst_layout` value used by the operation.
/// @param regions `regions` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::resolve_image(VkImage src,
                           VkImageLayout src_layout,
                           VkImage dst,
                           VkImageLayout dst_layout,
                           span<const VkImageResolve> regions) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::resolve_image");
            vkCmdResolveImage(buffer_, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()), regions.data());
        }

/// Clears color image.
///
/// @param image `image` value used by the operation.
/// @param layout `layout` value used by the operation.
/// @param color `color` value used by the operation.
/// @param ranges `ranges` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::clear_color_image(VkImage image,
                               VkImageLayout layout,
                               const VkClearColorValue &color,
                               span<const VkImageSubresourceRange> ranges) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::clear_color_image");
            vkCmdClearColorImage(buffer_, image, layout, &color, static_cast<u32>(ranges.size()), ranges.data());
        }

/// Clears depth stencil image.
///
/// @param image `image` value used by the operation.
/// @param layout `layout` value used by the operation.
/// @param clear `clear` value used by the operation.
/// @param ranges `ranges` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::clear_depth_stencil_image(VkImage image,
                                       VkImageLayout layout,
                                       const VkClearDepthStencilValue &clear,
                                       span<const VkImageSubresourceRange> ranges) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::clear_depth_stencil_image");
            vkCmdClearDepthStencilImage(buffer_, image, layout, &clear, static_cast<u32>(ranges.size()), ranges.data());
        }

/// Resets query pool to its baseline state.
///
/// @param pool `pool` value used by the operation.
/// @param first_query `first_query` value used by the operation.
/// @param query_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::reset_query_pool");
            vkCmdResetQueryPool(buffer_, pool, first_query, query_count);
        }

/// Writes timestamp2 to the associated destination.
///
/// @param stage `stage` value used by the operation.
/// @param pool `pool` value used by the operation.
/// @param query `query` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::write_timestamp2(VkPipelineStageFlagBits2 stage, VkQueryPool pool, u32 query) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::write_timestamp2");
            vkCmdWriteTimestamp2(buffer_, stage, pool, query);
        }

/// Performs the begin query operation for `Vulkan` using the supplied arguments.
///
/// @param pool `pool` value used by the operation.
/// @param query `query` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::begin_query(VkQueryPool pool, u32 query, VkQueryControlFlags flags) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin_query");
            vkCmdBeginQuery(buffer_, pool, query, flags);
        }

/// Performs the end query operation for `Vulkan` using the supplied arguments.
///
/// @param pool `pool` value used by the operation.
/// @param query `query` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::end_query(VkQueryPool pool, u32 query) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::end_query");
            vkCmdEndQuery(buffer_, pool, query);
        }

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
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count,
                                     VkBuffer dst, VkDeviceSize dst_offset, VkDeviceSize stride,
                                     VkQueryResultFlags flags) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_query_pool_results");
            vkCmdCopyQueryPoolResults(buffer_, pool, first_query, query_count, dst, dst_offset, stride, flags);
        }

/// Executes commands.
///
/// @param secondaries `secondaries` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::execute_commands(span<const VkCommandBuffer> secondaries) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::execute_commands");
            vkCmdExecuteCommands(buffer_, static_cast<u32>(secondaries.size()), secondaries.data());
        }

/// Performs the begin conditional rendering operation for `Vulkan` using the supplied arguments.
///
/// @param buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::begin_conditional_rendering(VkBuffer buffer, VkDeviceSize offset,
                                         VkConditionalRenderingFlagsEXT flags) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin_conditional_rendering");
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

/// Returns the current or globally available end conditional rendering value.
///
/// @return Returns the current end conditional rendering value.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::end_conditional_rendering() const noexcept {
            ZoneScopedN("VulkanCommandBuffer::end_conditional_rendering");
            if (vkCmdEndConditionalRenderingEXT != nullptr) {
                vkCmdEndConditionalRenderingEXT(buffer_);
            }
        }

/// Builds acceleration structures.
///
/// @param builds `builds` value used by the operation.
/// @param ranges `ranges` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::build_acceleration_structures(
            span<const VkAccelerationStructureBuildGeometryInfoKHR> builds,
            span<const VkAccelerationStructureBuildRangeInfoKHR *const> ranges) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::build_acceleration_structures");
            if (vkCmdBuildAccelerationStructuresKHR == nullptr) {
                return;
            }
            vkCmdBuildAccelerationStructuresKHR(buffer_, static_cast<u32>(builds.size()), builds.data(), ranges.data());
        }

/// Builds opacity micromaps.
///
/// @param builds `builds` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::build_opacity_micromaps(span<const VkMicromapBuildInfoEXT> builds) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::build_opacity_micromaps");
            if (vkCmdBuildMicromapsEXT == nullptr) {
                return;
            }
            vkCmdBuildMicromapsEXT(buffer_, static_cast<u32>(builds.size()), builds.data());
        }

/// Copies acceleration structure to its destination.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::copy_acceleration_structure(const VkCopyAccelerationStructureInfoKHR &info) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::copy_acceleration_structure");
            if (vkCmdCopyAccelerationStructureKHR != nullptr) {
                vkCmdCopyAccelerationStructureKHR(buffer_, &info);
            }
        }

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
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::trace_rays(const VkStridedDeviceAddressRegionKHR &raygen,
                        const VkStridedDeviceAddressRegionKHR &miss,
                        const VkStridedDeviceAddressRegionKHR &hit,
                        const VkStridedDeviceAddressRegionKHR &callable,
                        u32 width, u32 height, u32 depth) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::trace_rays");
            if (vkCmdTraceRaysKHR == nullptr) {
                return;
            }
            vkCmdTraceRaysKHR(buffer_, &raygen, &miss, &hit, &callable, width, height, depth);
        }

/// Traces rays indirect using the supplied arguments and current state.
///
/// @param raygen `raygen` value used by the operation.
/// @param miss `miss` value used by the operation.
/// @param hit `hit` value used by the operation.
/// @param callable `callable` value used by the operation.
/// @param indirect_device_address Device used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::trace_rays_indirect(const VkStridedDeviceAddressRegionKHR &raygen,
                                 const VkStridedDeviceAddressRegionKHR &miss,
                                 const VkStridedDeviceAddressRegionKHR &hit,
                                 const VkStridedDeviceAddressRegionKHR &callable,
                                 VkDeviceAddress indirect_device_address) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::trace_rays_indirect");
            if (vkCmdTraceRaysIndirectKHR == nullptr) {
                return;
            }
            vkCmdTraceRaysIndirectKHR(buffer_, &raygen, &miss, &hit, &callable, indirect_device_address);
        }

/// Performs the begin debug label operation for `Vulkan` using the supplied arguments.
///
/// @param label `label` value used by the operation.
/// @param r `r` value used by the operation.
/// @param g `g` value used by the operation.
/// @param b `b` value used by the operation.
/// @param a `a` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::begin_debug_label(const char *label,
                               float r,
                               float g,
                               float b,
                               float a) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::begin_debug_label");
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

/// Returns the current or globally available end debug label value.
///
/// @return Returns the current end debug label value.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::end_debug_label() const noexcept {
            ZoneScopedN("VulkanCommandBuffer::end_debug_label");
            if (vkCmdEndDebugUtilsLabelEXT != nullptr) {
                vkCmdEndDebugUtilsLabelEXT(buffer_);
            }
        }

/// Inserts the supplied value or range at the requested position.
///
/// @param label `label` value used by the operation.
/// @param r `r` value used by the operation.
/// @param g `g` value used by the operation.
/// @param b `b` value used by the operation.
/// @param a `a` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::insert_debug_label(const char *label,
                                float r,
                                float g,
                                float b,
                                float a) const noexcept {
            ZoneScopedN("VulkanCommandBuffer::insert_debug_label");
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

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanCommandBuffer::destroy() noexcept {
            ZoneScopedN("VulkanCommandBuffer::destroy");
            if (buffer_ == VK_NULL_HANDLE)
                return;
            vkFreeCommandBuffers(device_, command_pool_, 1, &buffer_);
            buffer_ = VK_NULL_HANDLE;
            command_pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

} // namespace SFT::Core::Vulkan
