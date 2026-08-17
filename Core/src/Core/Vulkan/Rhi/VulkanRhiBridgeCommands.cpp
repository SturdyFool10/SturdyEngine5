
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanCommandBuffer.hpp>
#include <Core/Vulkan/VulkanCommandPool.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>
#include <Core/Vulkan/VulkanPipeline.hpp>
#include <Core/Vulkan/VulkanQueryPool.hpp>
#include <Core/Vulkan/VulkanRendering.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::make_unique;
using std::span;
using std::unique_ptr;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param rect `rect` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan rect representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkRect2D to_vk_rect(const rhi::Rect2D &rect) noexcept {
            return VkRect2D{
                .offset = {.x = rect.x, .y = rect.y},
                .extent = {.width = rect.width, .height = rect.height},
            };
        }


        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkViewport to_vk_viewport(const rhi::Viewport &viewport) noexcept {
            return VkViewport{
                .x = viewport.x,
                .y = viewport.y + viewport.height,
                .width = viewport.width,
                .height = -viewport.height,
                .minDepth = viewport.min_depth,
                .maxDepth = viewport.max_depth,
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan clear color representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkClearColorValue to_vk_clear_color(const rhi::ClearColor &color) noexcept {
            return VkClearColorValue{.float32 = {color.r, color.g, color.b, color.a}};
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value converted to Vulkan clear depth stencil representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkClearDepthStencilValue to_vk_clear_depth_stencil(
            const rhi::ClearDepthStencil &value) noexcept {
            return VkClearDepthStencilValue{.depth = value.depth, .stencil = value.stencil};
        }

        /// Computes the to Vulkan offset required by the supplied values.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value converted to Vulkan offset representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkOffset3D to_vk_offset(const rhi::Offset3D &offset) noexcept {
            return VkOffset3D{.x = offset.x, .y = offset.y, .z = offset.z};
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan extent representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkExtent3D to_vk_extent(const rhi::Extent3D &extent) noexcept {
            return VkExtent3D{.width = extent.width, .height = extent.height, .depth = extent.depth_or_layers};
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param layers `layers` value used by the operation.
        /// @param aspect `aspect` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan layers representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageSubresourceLayers to_vk_layers(const rhi::TextureSubresourceLayers &layers,
                                                            VkImageAspectFlags aspect) noexcept {
            return VkImageSubresourceLayers{
                .aspectMask = aspect,
                .mipLevel = layers.mip_level,
                .baseArrayLayer = layers.base_array_layer,
                .layerCount = layers.array_layer_count,
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param range Range of values to process.
        /// @param aspect `aspect` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan range representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageSubresourceRange to_vk_range(const rhi::TextureSubresourceRange &range,
                                                          VkImageAspectFlags aspect) noexcept {
            return VkImageSubresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = range.base_mip_level,
                .levelCount = range.mip_level_count,
                .baseArrayLayer = range.base_array_layer,
                .layerCount = range.array_layer_count,
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param region `region` value used by the operation.
        /// @param aspect `aspect` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan buffer image copy representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkBufferImageCopy to_vk_buffer_image_copy(const rhi::BufferTextureCopy &region,
                                                                VkImageAspectFlags aspect) noexcept {
            return VkBufferImageCopy{
                .bufferOffset = region.buffer_offset,
                .bufferRowLength = region.buffer_row_length,
                .bufferImageHeight = region.buffer_image_height,
                .imageSubresource = {
                    .aspectMask = aspect,
                    .mipLevel = region.mip_level,
                    .baseArrayLayer = region.base_array_layer,
                    .layerCount = region.array_layer_count,
                },
                .imageOffset = to_vk_offset(region.texture_offset),
                .imageExtent = to_vk_extent(region.texture_extent),
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param barrier `barrier` value used by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param src_queue_family Queue used or affected by the operation.
        /// @param dst_queue_family Queue used or affected by the operation.
        ///
        /// @return Returns the value converted to Vulkan buffer barrier representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkBufferMemoryBarrier2 to_vk_buffer_barrier(const rhi::BufferBarrier &barrier,
                                                                  VkBuffer buffer,
                                                                  u32 src_queue_family,
                                                                  u32 dst_queue_family) noexcept {
            return VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = to_vk(barrier.src_stage),
                .srcAccessMask = to_vk(barrier.src_access),
                .dstStageMask = to_vk(barrier.dst_stage),
                .dstAccessMask = to_vk(barrier.dst_access),
                .srcQueueFamilyIndex = src_queue_family,
                .dstQueueFamilyIndex = dst_queue_family,
                .buffer = buffer,
                .offset = barrier.offset,
                .size = barrier.size == 0 ? VK_WHOLE_SIZE : barrier.size,
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param barrier `barrier` value used by the operation.
        /// @param image `image` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param src_queue_family Queue used or affected by the operation.
        /// @param dst_queue_family Queue used or affected by the operation.
        ///
        /// @return Returns the value converted to Vulkan image barrier representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageMemoryBarrier2 to_vk_image_barrier(const rhi::TextureBarrier &barrier,
                                                                VkImage image,
                                                                rhi::Format format,
                                                                u32 src_queue_family,
                                                                u32 dst_queue_family) noexcept {
            return VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = to_vk(barrier.src_stage),
                .srcAccessMask = to_vk(barrier.src_access),
                .dstStageMask = to_vk(barrier.dst_stage),
                .dstAccessMask = to_vk(barrier.dst_access),
                .oldLayout = to_vk(barrier.old_layout),
                .newLayout = to_vk(barrier.new_layout),
                .srcQueueFamilyIndex = src_queue_family,
                .dstQueueFamilyIndex = dst_queue_family,
                .image = image,
                .subresourceRange = to_vk_range(barrier.range, aspect_for_format(format)),
            };
        }

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param barrier `barrier` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan memory barrier representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryBarrier2 to_vk_memory_barrier(const rhi::GlobalBarrier &barrier) noexcept {
            return VkMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = to_vk(barrier.src_stage),
                .srcAccessMask = to_vk(barrier.src_access),
                .dstStageMask = to_vk(barrier.dst_stage),
                .dstAccessMask = to_vk(barrier.dst_access),
            };
        }

    } // namespace

    class VulkanRhiEncoderCommon {
      public:
        /// Constructs a `VulkanRhiEncoderCommon` from the supplied initialization values.
        ///
        /// @param bridge `bridge` value used by the operation.
        /// @param command_buffer Buffer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiEncoderCommon(VulkanRhiDeviceBridge &bridge, VulkanCommandBuffer &command_buffer)
            : bridge_(bridge), command_buffer_(command_buffer) {}

      protected:
        /// Performs the buffer operation for `VulkanRhiEncoderCommon` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanBuffer *buffer(rhi::BufferHandle handle) const noexcept {
            auto *record = bridge_.buffers_.find(handle);
            return record ? &record->buffer : nullptr;
        }

        /// Performs the texture operation for `VulkanRhiEncoderCommon` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanRhiDeviceBridge::TextureRecord *texture(rhi::TextureHandle handle) const noexcept {
            return bridge_.textures_.find(handle);
        }

        /// Performs the descriptor set operation for `VulkanRhiEncoderCommon` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDescriptorSet descriptor_set(rhi::BindGroupHandle handle) const noexcept {
            auto *record = bridge_.bind_groups_.find(handle);
            return record ? record->set : VK_NULL_HANDLE;
        }

        /// Performs the pipeline layout operation for `VulkanRhiEncoderCommon` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineLayout pipeline_layout(rhi::PipelineLayoutHandle handle) const noexcept {
            auto *layout = bridge_.pipeline_layouts_.find(handle);
            return layout ? layout->vk_handle() : VK_NULL_HANDLE;
        }

        /// Queries pool from the active backend or runtime state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanQueryPool *query_pool(rhi::QuerySetHandle handle) const noexcept {
            return bridge_.query_sets_.find(handle);
        }

        /// Performs the acceleration structure operation for `VulkanRhiEncoderCommon` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanRhiDeviceBridge::AccelerationStructureRecord *acceleration_structure(
            rhi::AccelerationStructureHandle handle) const noexcept {
            return bridge_.acceleration_structures_.find(handle);
        }

        /// Binds group impl for subsequent operations.
        ///
        /// @param bind_point `bind_point` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void bind_group_impl(VkPipelineBindPoint bind_point, u32 index, rhi::BindGroupHandle bind_group,
                             span<const u32> dynamic_offsets) const noexcept {
            const VkDescriptorSet set = descriptor_set(bind_group);
            if (set == VK_NULL_HANDLE || current_layout_ == VK_NULL_HANDLE) {
                return;
            }
            command_buffer_.bind_descriptor_sets(bind_point, current_layout_, index, span{&set, 1}, dynamic_offsets);
        }

        /// Sets the vertex buffer impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param handle Handle identifying the target object or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void set_vertex_buffer_impl(u32 slot, rhi::BufferHandle handle, u64 offset) const noexcept {
            if (VulkanBuffer *record = buffer(handle)) {
                command_buffer_.bind_vertex_buffer(record->vk_handle(), offset, slot);
            }
        }

        /// Sets the index buffer impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void set_index_buffer_impl(rhi::BufferHandle handle, rhi::IndexFormat format, u64 offset) const noexcept {
            if (VulkanBuffer *record = buffer(handle)) {
                command_buffer_.bind_index_buffer(record->vk_handle(), to_vk(format), offset);
            }
        }

        /// Sets the push constants impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_push_constants_impl(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) const noexcept {
            if (current_layout_ == VK_NULL_HANDLE || data.empty()) {
                return;
            }
            command_buffer_.push_constants(current_layout_, to_vk(stages), offset, static_cast<u32>(data.size()), data.data());
        }

        /// Sets the viewport impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_viewport_impl(const rhi::Viewport &viewport) const noexcept { command_buffer_.set_viewport(to_vk_viewport(viewport)); }
        /// Sets the scissor impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_scissor_impl(const rhi::Rect2D &scissor) const noexcept { command_buffer_.set_scissor(to_vk_rect(scissor)); }
        /// Sets the blend constant impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_blend_constant_impl(const rhi::ClearColor &color) const noexcept {
            const array<float, 4> constants{color.r, color.g, color.b, color.a};
            command_buffer_.set_blend_constants(constants.data());
        }
        /// Sets the stencil reference impl for this `VulkanRhiEncoderCommon`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_stencil_reference_impl(u32 reference) const noexcept {
            command_buffer_.set_stencil_reference(VK_STENCIL_FRONT_AND_BACK, reference);
        }

        /// Draws impl using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_impl(const rhi::DrawArgs &args) const noexcept {
            command_buffer_.draw(args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
        }
        /// Draws indexed impl using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed_impl(const rhi::DrawIndexedArgs &args) const noexcept {
            command_buffer_.draw_indexed(args.index_count, args.instance_count, args.first_index, args.base_vertex, args.first_instance);
        }
        /// Draws mesh tasks impl using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks_impl(const rhi::DrawMeshTasksArgs &args) const noexcept {
            command_buffer_.draw_mesh_tasks(args.group_count_x, args.group_count_y, args.group_count_z);
        }
        /// Draws indirect impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indirect_impl(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) const noexcept {
            if (VulkanBuffer *record = buffer(indirect_buffer)) {
                command_buffer_.draw_indirect(record->vk_handle(), offset, draw_count, stride);
            }
        }
        /// Draws indexed indirect impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed_indirect_impl(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) const noexcept {
            if (VulkanBuffer *record = buffer(indirect_buffer)) {
                command_buffer_.draw_indexed_indirect(record->vk_handle(), offset, draw_count, stride);
            }
        }
        /// Draws indirect count impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indirect_count_impl(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                      rhi::BufferHandle count_buffer, u64 count_offset,
                                      u32 max_draws, u32 stride) const noexcept {
            VulkanBuffer *indirect = buffer(indirect_buffer);
            VulkanBuffer *count = buffer(count_buffer);
            if (indirect != nullptr && count != nullptr) {
                command_buffer_.draw_indirect_count(indirect->vk_handle(), indirect_offset, count->vk_handle(), count_offset, max_draws, stride);
            }
        }
        /// Draws indexed indirect count impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_indexed_indirect_count_impl(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                              rhi::BufferHandle count_buffer, u64 count_offset,
                                              u32 max_draws, u32 stride) const noexcept {
            VulkanBuffer *indirect = buffer(indirect_buffer);
            VulkanBuffer *count = buffer(count_buffer);
            if (indirect != nullptr && count != nullptr) {
                command_buffer_.draw_indexed_indirect_count(indirect->vk_handle(), indirect_offset, count->vk_handle(), count_offset, max_draws, stride);
            }
        }
        /// Draws mesh tasks indirect impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks_indirect_impl(rhi::BufferHandle indirect_buffer, u64 offset) const noexcept {
            if (VulkanBuffer *record = buffer(indirect_buffer)) {
                command_buffer_.draw_mesh_tasks_indirect(record->vk_handle(), offset, 1, sizeof(rhi::DrawMeshTasksArgs));
            }
        }
        /// Draws mesh tasks indirect count impl using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void draw_mesh_tasks_indirect_count_impl(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                                 rhi::BufferHandle count_buffer, u64 count_offset,
                                                 u32 max_draws, u32 stride) const noexcept {
            VulkanBuffer *indirect = buffer(indirect_buffer);
            VulkanBuffer *count = buffer(count_buffer);
            if (indirect != nullptr && count != nullptr) {
                command_buffer_.draw_mesh_tasks_indirect_count(indirect->vk_handle(), indirect_offset, count->vk_handle(), count_offset, max_draws, stride);
            }
        }

        VulkanRhiDeviceBridge &bridge_;
        VulkanCommandBuffer &command_buffer_;
        VkPipelineLayout current_layout_ = VK_NULL_HANDLE;
    };

    class VulkanRhiRenderPassEncoder final : public rhi::RenderPassEncoder, private VulkanRhiEncoderCommon {
      public:
        /// Constructs a `VulkanRhiRenderPassEncoder` from the supplied initialization values.
        ///
        /// @param bridge `bridge` value used by the operation.
        /// @param command_buffer Buffer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiRenderPassEncoder(VulkanRhiDeviceBridge &bridge, VulkanCommandBuffer &command_buffer)
            : VulkanRhiEncoderCommon(bridge, command_buffer) {}

        /// Sets the pipeline for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_pipeline(rhi::RenderPipelineHandle pipeline) override {
            auto *record = bridge_.render_pipelines_.find(pipeline);
            if (record == nullptr) {
                return;
            }
            current_layout_ = pipeline_layout(record->layout);
            command_buffer_.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline.vk_handle());
        }
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override {
            bind_group_impl(VK_PIPELINE_BIND_POINT_GRAPHICS, index, bind_group, dynamic_offsets);
        }
        /// Sets the vertex buffer for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset = 0) override { set_vertex_buffer_impl(slot, buffer, offset); }
        /// Sets the index buffer for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset = 0) override { set_index_buffer_impl(buffer, format, offset); }
        /// Sets the push constants for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override { set_push_constants_impl(stages, offset, data); }
        /// Sets the viewport for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_viewport(const rhi::Viewport &viewport) override { set_viewport_impl(viewport); }
        /// Sets the scissor for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_scissor(const rhi::Rect2D &scissor) override { set_scissor_impl(scissor); }
        /// Sets the blend constant for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_blend_constant(const rhi::ClearColor &color) override { set_blend_constant_impl(color); }
        /// Sets the stencil reference for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_stencil_reference(u32 reference) override { set_stencil_reference_impl(reference); }
        /// Draws the requested content using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw(const rhi::DrawArgs &args) override { draw_impl(args); }
        /// Draws indexed using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed(const rhi::DrawIndexedArgs &args) override { draw_indexed_impl(args); }
        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) override { draw_mesh_tasks_impl(args); }
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_indirect_impl(indirect_buffer, offset, 1, sizeof(rhi::DrawArgs)); }
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_indexed_indirect_impl(indirect_buffer, offset, 1, sizeof(rhi::DrawIndexedArgs)); }
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override { draw_indirect_impl(indirect_buffer, offset, draw_count, stride); }
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override { draw_indexed_indirect_impl(indirect_buffer, offset, draw_count, stride); }
        /// Returns the draw indirect count for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override {
            draw_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }
        /// Returns the draw indexed indirect count for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override {
            draw_indexed_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_mesh_tasks_indirect_impl(indirect_buffer, offset); }
        /// Returns the draw mesh tasks indirect count for this `VulkanRhiRenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override {
            draw_mesh_tasks_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride);
        }
        /// Executes bundles.
        ///
        /// @param bundles `bundles` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void execute_bundles(span<const rhi::RenderBundleHandle> bundles) override {
            vector<VkCommandBuffer> commands;
            commands.reserve(bundles.size());
            for (const rhi::RenderBundleHandle bundle : bundles) {
                auto *record = bridge_.render_bundles_.find(bundle);
                if (record != nullptr) {
                    commands.push_back(record->command_buffer.vk_handle());
                }
            }
            if (!commands.empty()) {
                command_buffer_.execute_commands(commands);
            }
        }
        /// Performs the begin occlusion query operation for `VulkanRhiRenderPassEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_occlusion_query(rhi::QuerySetHandle query_set, u32 index) override {
            VulkanQueryPool *pool = query_pool(query_set);
            if (pool == nullptr) {
                active_occlusion_query_pool_ = VK_NULL_HANDLE;
                return;
            }
            active_occlusion_query_pool_ = pool->vk_handle();
            active_occlusion_query_index_ = index;
            command_buffer_.begin_query(active_occlusion_query_pool_, index);
        }
        /// Performs the end occlusion query operation for `VulkanRhiRenderPassEncoder` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end_occlusion_query() override {
            if (active_occlusion_query_pool_ != VK_NULL_HANDLE) {
                command_buffer_.end_query(active_occlusion_query_pool_, active_occlusion_query_index_);
                active_occlusion_query_pool_ = VK_NULL_HANDLE;
                active_occlusion_query_index_ = 0;
            }
        }
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end() override { command_buffer_.end_rendering(); }

      private:
        VkQueryPool active_occlusion_query_pool_ = VK_NULL_HANDLE;
        u32 active_occlusion_query_index_ = 0;
    };

    class VulkanRhiRenderBundleEncoder final : public rhi::RenderBundleEncoder, private VulkanRhiEncoderCommon {
      public:
        /// Constructs a `VulkanRhiRenderBundleEncoder` from the supplied initialization values.
        ///
        /// @param bridge `bridge` value used by the operation.
        /// @param record `record` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiRenderBundleEncoder(VulkanRhiDeviceBridge &bridge, VulkanRhiDeviceBridge::RenderBundleRecord &&record)
            : VulkanRhiEncoderCommon(bridge, record_.command_buffer), record_(std::move(record)) {}

        /// Sets the pipeline for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_pipeline(rhi::RenderPipelineHandle pipeline) override {
            auto *pipeline_record = bridge_.render_pipelines_.find(pipeline);
            if (pipeline_record == nullptr) {
                return;
            }
            current_layout_ = pipeline_layout(pipeline_record->layout);
            command_buffer_.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_record->pipeline.vk_handle());
        }
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override { bind_group_impl(VK_PIPELINE_BIND_POINT_GRAPHICS, index, bind_group, dynamic_offsets); }
        /// Sets the vertex buffer for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset = 0) override { set_vertex_buffer_impl(slot, buffer, offset); }
        /// Sets the index buffer for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset = 0) override { set_index_buffer_impl(buffer, format, offset); }
        /// Sets the push constants for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override { set_push_constants_impl(stages, offset, data); }
        /// Sets the viewport for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_viewport(const rhi::Viewport &viewport) override { set_viewport_impl(viewport); }
        /// Sets the scissor for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_scissor(const rhi::Rect2D &scissor) override { set_scissor_impl(scissor); }
        /// Sets the blend constant for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_blend_constant(const rhi::ClearColor &color) override { set_blend_constant_impl(color); }
        /// Sets the stencil reference for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_stencil_reference(u32 reference) override { set_stencil_reference_impl(reference); }
        /// Draws the requested content using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw(const rhi::DrawArgs &args) override { draw_impl(args); }
        /// Draws indexed using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed(const rhi::DrawIndexedArgs &args) override { draw_indexed_impl(args); }
        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) override { draw_mesh_tasks_impl(args); }
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_indirect_impl(indirect_buffer, offset, 1, sizeof(rhi::DrawArgs)); }
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_indexed_indirect_impl(indirect_buffer, offset, 1, sizeof(rhi::DrawIndexedArgs)); }
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override { draw_indirect_impl(indirect_buffer, offset, draw_count, stride); }
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override { draw_indexed_indirect_impl(indirect_buffer, offset, draw_count, stride); }
        /// Returns the draw indirect count for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override { draw_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride); }
        /// Returns the draw indexed indirect count for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override { draw_indexed_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride); }
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override { draw_mesh_tasks_indirect_impl(indirect_buffer, offset); }
        /// Returns the draw mesh tasks indirect count for this `VulkanRhiRenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override { draw_mesh_tasks_indirect_count_impl(indirect_buffer, indirect_offset, count_buffer, count_offset, max_draws, stride); }

        /// Returns the current or globally available finish value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        rhi::RhiExpected<rhi::RenderBundleHandle> finish() override {
            if (auto ended = record_.command_buffer.end(); !ended) {
                return VulkanRhiDeviceBridge::rhi_error_from_graphics(ended.error());
            }
            return bridge_.render_bundles_.insert(std::move(record_));
        }

      private:
        VulkanRhiDeviceBridge::RenderBundleRecord record_;
    };

    class VulkanRhiComputePassEncoder final : public rhi::ComputePassEncoder, private VulkanRhiEncoderCommon {
      public:
        /// Constructs a `VulkanRhiComputePassEncoder` from the supplied initialization values.
        ///
        /// @param bridge `bridge` value used by the operation.
        /// @param command_buffer Buffer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiComputePassEncoder(VulkanRhiDeviceBridge &bridge, VulkanCommandBuffer &command_buffer)
            : VulkanRhiEncoderCommon(bridge, command_buffer) {}

        /// Sets the pipeline for this `VulkanRhiComputePassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_pipeline(rhi::ComputePipelineHandle pipeline) override {
            auto *record = bridge_.compute_pipelines_.find(pipeline);
            if (record == nullptr) {
                return;
            }
            current_layout_ = pipeline_layout(record->layout);
            command_buffer_.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, record->pipeline.vk_handle());
        }
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override { bind_group_impl(VK_PIPELINE_BIND_POINT_COMPUTE, index, bind_group, dynamic_offsets); }
        /// Sets the push constants for this `VulkanRhiComputePassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override { set_push_constants_impl(stages, offset, data); }
        /// Dispatches the requested work.
        ///
        /// @param group_count_x `group_count_x` value used by the operation.
        /// @param group_count_y `group_count_y` value used by the operation.
        /// @param group_count_z `group_count_z` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) override { command_buffer_.dispatch(group_count_x, group_count_y, group_count_z); }
        /// Dispatches indirect.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void dispatch_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override {
            if (VulkanBuffer *record = buffer(indirect_buffer)) {
                command_buffer_.dispatch_indirect(record->vk_handle(), offset);
            }
        }
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end() override {}
    };

    class VulkanRhiCommandEncoder final : public rhi::CommandEncoder, private VulkanRhiEncoderCommon {
      public:
        /// Constructs a `VulkanRhiCommandEncoder` from the supplied initialization values.
        ///
        /// @param bridge `bridge` value used by the operation.
        /// @param record `record` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiCommandEncoder(VulkanRhiDeviceBridge &bridge, VulkanRhiDeviceBridge::CommandBufferRecord &&record)
            : VulkanRhiEncoderCommon(bridge, record_.command_buffer), record_(std::move(record)) {}


        /// Returns the current or globally available native Vulkan command buffer value.
        ///
        /// @return Returns the current native Vulkan command buffer value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandBuffer native_vk_command_buffer() const noexcept { return command_buffer_.vk_handle(); }

        /// Performs the begin render pass operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
        rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> begin_render_pass(const rhi::RenderPassDesc &desc) override {
            RenderingInfo rendering;
            rendering.set_render_area(to_vk_rect(desc.render_area)).set_view_mask(desc.view_mask);
            if (desc.allow_bundles) {


                rendering.add_flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
            }

            for (const rhi::ColorAttachment &attachment : desc.color_attachments) {
                VulkanImageView *view = bridge_.texture_views_.find(attachment.view);
                if (view == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "begin_render_pass: unknown color attachment view handle.");
                }
                ColorAttachment color{
                    .view = view->vk_handle(),
                    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .load_op = to_vk(attachment.load_op),
                    .store_op = to_vk(attachment.store_op),
                    .clear_color = to_vk_clear_color(attachment.clear_color),
                };
                if (attachment.resolve_view.is_valid()) {
                    VulkanImageView *resolve = bridge_.texture_views_.find(attachment.resolve_view);
                    if (resolve == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "begin_render_pass: unknown color resolve view handle.");
                    }
                    color.resolve_mode = VK_RESOLVE_MODE_AVERAGE_BIT;
                    color.resolve_view = resolve->vk_handle();
                    color.resolve_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                rendering.add_color(color);
            }

            if (desc.depth_stencil.view.is_valid()) {
                VulkanImageView *view = bridge_.texture_views_.find(desc.depth_stencil.view);
                if (view == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "begin_render_pass: unknown depth/stencil attachment view handle.");
                }
                const bool has_depth = view->format() == VK_FORMAT_D16_UNORM || view->format() == VK_FORMAT_D24_UNORM_S8_UINT ||
                                       view->format() == VK_FORMAT_D32_SFLOAT || view->format() == VK_FORMAT_D32_SFLOAT_S8_UINT;
                const bool has_stencil = view->format() == VK_FORMAT_D24_UNORM_S8_UINT || view->format() == VK_FORMAT_D32_SFLOAT_S8_UINT;
                if (has_depth) {
                    DepthAttachment depth{
                        .view = view->vk_handle(),
                        .layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        .load_op = to_vk(desc.depth_stencil.depth_load_op),
                        .store_op = to_vk(desc.depth_stencil.depth_store_op),
                        .clear_depth = desc.depth_stencil.clear_value.depth,
                    };
                    if (desc.depth_stencil.resolve_view.is_valid()) {
                        VulkanImageView *resolve = bridge_.texture_views_.find(desc.depth_stencil.resolve_view);
                        if (resolve == nullptr) {
                            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                                  "begin_render_pass: unknown depth resolve view handle.");
                        }
                        depth.resolve_mode = to_vk(desc.depth_stencil.depth_resolve_mode);
                        depth.resolve_view = resolve->vk_handle();
                        depth.resolve_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    }
                    rendering.set_depth(depth);
                }
                if (has_stencil) {
                    rendering.set_stencil(StencilAttachment{
                        .view = view->vk_handle(),
                        .layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
                        .load_op = to_vk(desc.depth_stencil.stencil_load_op),
                        .store_op = to_vk(desc.depth_stencil.stencil_store_op),
                        .clear_stencil = desc.depth_stencil.clear_value.stencil,
                    });
                }
            }

            const VkRenderingInfo info = rendering.build();
            command_buffer_.begin_rendering(info);
            return unique_ptr<rhi::RenderPassEncoder>(make_unique<VulkanRhiRenderPassEncoder>(bridge_, command_buffer_));
        }

        /// Performs the begin compute pass operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> begin_compute_pass(const rhi::ComputePassDesc &) override {
            return unique_ptr<rhi::ComputePassEncoder>(make_unique<VulkanRhiComputePassEncoder>(bridge_, command_buffer_));
        }

        /// Copies buffer to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst, const rhi::BufferCopy &region) override {
            VulkanBuffer *src_buffer = buffer(src);
            VulkanBuffer *dst_buffer = buffer(dst);
            if (src_buffer != nullptr && dst_buffer != nullptr) {
                command_buffer_.copy_buffer(src_buffer->vk_handle(), dst_buffer->vk_handle(), region.size, region.src_offset, region.dst_offset);
            }
        }
        /// Copies buffer to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst, const rhi::BufferTextureCopy &region) override {
            VulkanBuffer *src_buffer = buffer(src);
            auto *dst_texture = texture(dst);
            if (src_buffer != nullptr && dst_texture != nullptr) {
                const VkBufferImageCopy copy = to_vk_buffer_image_copy(region, aspect_for_format(dst_texture->format));
                command_buffer_.copy_buffer_to_image(src_buffer->vk_handle(), dst_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, span{&copy, 1});
            }
        }
        /// Copies texture to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst, const rhi::BufferTextureCopy &region) override {
            auto *src_texture = texture(src);
            VulkanBuffer *dst_buffer = buffer(dst);
            if (src_texture != nullptr && dst_buffer != nullptr) {
                const VkBufferImageCopy copy = to_vk_buffer_image_copy(region, aspect_for_format(src_texture->format));
                command_buffer_.copy_image_to_buffer(src_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buffer->vk_handle(), span{&copy, 1});
            }
        }
        /// Copies texture to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureCopy &region) override {
            auto *src_texture = texture(src);
            auto *dst_texture = texture(dst);
            if (src_texture != nullptr && dst_texture != nullptr) {
                const VkImageCopy copy{
                    .srcSubresource = to_vk_layers(region.src_subresource, aspect_for_format(src_texture->format)),
                    .srcOffset = to_vk_offset(region.src_offset),
                    .dstSubresource = to_vk_layers(region.dst_subresource, aspect_for_format(dst_texture->format)),
                    .dstOffset = to_vk_offset(region.dst_offset),
                    .extent = to_vk_extent(region.extent),
                };
                command_buffer_.copy_image(src_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           dst_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, span{&copy, 1});
            }
        }
        /// Performs the blit texture operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        /// @param filter `filter` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void blit_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureBlit &region, rhi::Filter filter) override {
            auto *src_texture = texture(src);
            auto *dst_texture = texture(dst);
            if (src_texture != nullptr && dst_texture != nullptr) {
                const VkImageBlit blit{
                    .srcSubresource = to_vk_layers(region.src_subresource, aspect_for_format(src_texture->format)),
                    .srcOffsets = {to_vk_offset(region.src_min), to_vk_offset(region.src_max)},
                    .dstSubresource = to_vk_layers(region.dst_subresource, aspect_for_format(dst_texture->format)),
                    .dstOffsets = {to_vk_offset(region.dst_min), to_vk_offset(region.dst_max)},
                };
                command_buffer_.blit_image(src_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           dst_texture->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           span{&blit, 1}, to_vk(filter));
            }
        }
        /// Fills buffer using the supplied arguments and current state.
        ///
        /// @param dst Destination value or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void fill_buffer(rhi::BufferHandle dst, u64 offset, u64 size, u32 value) override {
            if (VulkanBuffer *record = buffer(dst)) {
                command_buffer_.fill_buffer(record->vk_handle(), offset, size == 0 ? VK_WHOLE_SIZE : size, value);
            }
        }
        /// Updates buffer from the supplied values.
        ///
        /// @param dst Destination value or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update_buffer(rhi::BufferHandle dst, u64 offset, span<const std::byte> data) override {
            if (VulkanBuffer *record = buffer(dst); record != nullptr && !data.empty()) {
                command_buffer_.update_buffer(record->vk_handle(), offset, data.size(), data.data());
            }
        }
        /// Clears color texture.
        ///
        /// @param texture_handle Texture used or affected by the operation.
        /// @param color `color` value used by the operation.
        /// @param range Range of values to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void clear_color_texture(rhi::TextureHandle texture_handle, const rhi::ClearColor &color, const rhi::TextureSubresourceRange &range) override {
            auto *record = texture(texture_handle);
            if (record != nullptr) {
                const VkImageSubresourceRange vk_range = to_vk_range(range, aspect_for_format(record->format));
                const VkClearColorValue vk_color = to_vk_clear_color(color);
                command_buffer_.clear_color_image(record->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk_color, span{&vk_range, 1});
            }
        }
        /// Clears depth stencil texture.
        ///
        /// @param texture_handle Texture used or affected by the operation.
        /// @param value Value consumed by the operation.
        /// @param range Range of values to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void clear_depth_stencil_texture(rhi::TextureHandle texture_handle, const rhi::ClearDepthStencil &value, const rhi::TextureSubresourceRange &range) override {
            auto *record = texture(texture_handle);
            if (record != nullptr) {
                const VkImageSubresourceRange vk_range = to_vk_range(range, aspect_for_format(record->format));
                const VkClearDepthStencilValue vk_clear = to_vk_clear_depth_stencil(value);
                command_buffer_.clear_depth_stencil_image(record->image.vk_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk_clear, span{&vk_range, 1});
            }
        }
        /// Builds acceleration structures.
        ///
        /// @param builds `builds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_acceleration_structures(span<const rhi::AccelerationStructureBuildDesc> builds) override {
            vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
            vector<vector<VkAccelerationStructureGeometryKHR>> geometry_storage;
            vector<vector<VkAccelerationStructureBuildRangeInfoKHR>> range_storage;
            vector<const VkAccelerationStructureBuildRangeInfoKHR *> range_ptrs;
            build_infos.reserve(builds.size());
            geometry_storage.reserve(builds.size());
            range_storage.reserve(builds.size());
            range_ptrs.reserve(builds.size());

            for (const rhi::AccelerationStructureBuildDesc &build : builds) {
                auto *dst = acceleration_structure(build.dst);
                auto *src = acceleration_structure(build.src);
                VulkanBuffer *scratch = buffer(build.scratch_buffer);
                if (dst == nullptr || scratch == nullptr) {
                    continue;
                }

                vector<VkAccelerationStructureGeometryKHR> geometries;
                geometries.reserve(build.geometries.size());
                for (const rhi::AccelerationStructureGeometryDesc &geometry : build.geometries) {
                    VkAccelerationStructureGeometryKHR vk_geometry{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                        .geometry = {},
                        .flags = geometry_flags(geometry.flags),
                    };
                    switch (geometry.type) {
                        case rhi::AccelerationStructureGeometryType::Triangles: {
                            VulkanBuffer *vertices = buffer(geometry.triangles.vertex_buffer);
                            VulkanBuffer *indices = buffer(geometry.triangles.index_buffer);
                            VulkanBuffer *transform = buffer(geometry.triangles.transform_buffer);
                            vk_geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                            vk_geometry.geometry.triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                .vertexFormat = to_vk(geometry.triangles.vertex_format),
                                .vertexData = device_address_const(vertices ? vertices->device_address() + geometry.triangles.vertex_offset : 0),
                                .vertexStride = geometry.triangles.vertex_stride,
                                .maxVertex = geometry.triangles.max_vertex,
                                .indexType = indices ? to_vk(geometry.triangles.index_format) : VK_INDEX_TYPE_NONE_KHR,
                                .indexData = device_address_const(indices ? indices->device_address() + geometry.triangles.index_offset : 0),
                                .transformData = device_address_const(transform ? transform->device_address() + geometry.triangles.transform_offset : 0),
                            };
                            break;
                        }
                        case rhi::AccelerationStructureGeometryType::Aabbs: {
                            VulkanBuffer *aabbs = buffer(geometry.aabbs.buffer);
                            vk_geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                            vk_geometry.geometry.aabbs = VkAccelerationStructureGeometryAabbsDataKHR{
                                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                                .data = device_address_const(aabbs ? aabbs->device_address() + geometry.aabbs.offset : 0),
                                .stride = geometry.aabbs.stride,
                            };
                            break;
                        }
                        case rhi::AccelerationStructureGeometryType::Instances: {
                            VulkanBuffer *instances = buffer(geometry.instances.buffer);
                            vk_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                            vk_geometry.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{
                                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                .arrayOfPointers = geometry.instances.array_of_pointers ? VK_TRUE : VK_FALSE,
                                .data = device_address_const(instances ? instances->device_address() + geometry.instances.offset : 0),
                            };
                            break;
                        }
                    }
                    geometries.push_back(vk_geometry);
                }

                vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
                ranges.reserve(build.ranges.size());
                for (const rhi::AccelerationStructureBuildRangeInfo &range : build.ranges) {
                    ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{
                        .primitiveCount = range.primitive_count,
                        .primitiveOffset = range.primitive_offset,
                        .firstVertex = range.first_vertex,
                        .transformOffset = range.transform_offset,
                    });
                }

                geometry_storage.push_back(std::move(geometries));
                range_storage.push_back(std::move(ranges));
                range_ptrs.push_back(range_storage.back().empty() ? nullptr : range_storage.back().data());
                build_infos.push_back(VkAccelerationStructureBuildGeometryInfoKHR{
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                    .type = acceleration_structure_type(build.type),
                    .flags = build_flags(build.flags),
                    .mode = build.src.is_valid() ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                 : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                    .srcAccelerationStructure = src ? src->acceleration_structure.vk_handle() : VK_NULL_HANDLE,
                    .dstAccelerationStructure = dst->acceleration_structure.vk_handle(),
                    .geometryCount = static_cast<u32>(geometry_storage.back().size()),
                    .pGeometries = geometry_storage.back().empty() ? nullptr : geometry_storage.back().data(),
                    .scratchData = device_address(scratch->device_address() + build.scratch_offset),
                });
            }

            if (!build_infos.empty()) {
                command_buffer_.build_acceleration_structures(build_infos, range_ptrs);
            }
        }
        /// Copies acceleration structure to its destination.
        ///
        /// @param copy `copy` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) override {
            auto *src = acceleration_structure(copy.src);
            auto *dst = acceleration_structure(copy.dst);
            if (src == nullptr || dst == nullptr) {
                return;
            }
            const VkCopyAccelerationStructureInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                .src = src->acceleration_structure.vk_handle(),
                .dst = dst->acceleration_structure.vk_handle(),
                .mode = copy_mode(copy.mode),
            };
            command_buffer_.copy_acceleration_structure(info);
        }
        /// Traces rays using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void trace_rays(const rhi::TraceRaysDesc &desc) override {
            command_buffer_.trace_rays(shader_binding_table_region(desc.raygen),
                                       shader_binding_table_region(desc.miss),
                                       shader_binding_table_region(desc.hit),
                                       shader_binding_table_region(desc.callable),
                                       desc.width, desc.height, desc.depth);
        }
        /// Performs the barrier operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param global_barriers `global_barriers` value used by the operation.
        /// @param buffer_barriers Buffer used or affected by the operation.
        /// @param texture_barriers Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void barrier(span<const rhi::GlobalBarrier> global_barriers, span<const rhi::BufferBarrier> buffer_barriers, span<const rhi::TextureBarrier> texture_barriers) override {
            auto ownership_families = [&](const rhi::QueueOwnershipTransfer &ownership) noexcept {
                struct Families { u32 src = VK_QUEUE_FAMILY_IGNORED; u32 dst = VK_QUEUE_FAMILY_IGNORED; } families;
                if (!ownership.enabled) {
                    return families;
                }
                families.src = bridge_.queue_family_for_lane(ownership.src);
                families.dst = bridge_.queue_family_for_lane(ownership.dst);
                if (families.src == VK_QUEUE_FAMILY_IGNORED || families.dst == VK_QUEUE_FAMILY_IGNORED ||
                    families.src == families.dst) {
                    families.src = VK_QUEUE_FAMILY_IGNORED;
                    families.dst = VK_QUEUE_FAMILY_IGNORED;
                }
                return families;
            };

            vector<VkMemoryBarrier2> memory;
            vector<VkBufferMemoryBarrier2> buffers;
            vector<VkImageMemoryBarrier2> images;
            memory.reserve(global_barriers.size());
            buffers.reserve(buffer_barriers.size());
            images.reserve(texture_barriers.size());
            for (const rhi::GlobalBarrier &barrier : global_barriers) {
                memory.push_back(to_vk_memory_barrier(barrier));
            }
            for (const rhi::BufferBarrier &barrier : buffer_barriers) {
                if (VulkanBuffer *record = buffer(barrier.buffer)) {
                    const auto families = ownership_families(barrier.ownership);
                    buffers.push_back(to_vk_buffer_barrier(barrier, record->vk_handle(), families.src, families.dst));
                }
            }
            for (const rhi::TextureBarrier &barrier : texture_barriers) {
                if (auto *record = texture(barrier.texture)) {
                    const auto families = ownership_families(barrier.ownership);
                    images.push_back(to_vk_image_barrier(barrier, record->image.vk_handle(), record->format, families.src, families.dst));
                }
            }
            command_buffer_.pipeline_barrier2(memory, buffers, images);
        }
        /// Resets query set to its baseline state.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) override {
            if (VulkanQueryPool *pool = query_pool(query_set)) {
                command_buffer_.reset_query_pool(pool->vk_handle(), first, count);
            }
        }
        /// Writes timestamp to the associated destination.
        ///
        /// @param stage `stage` value used by the operation.
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set, u32 index) override {
            if (VulkanQueryPool *pool = query_pool(query_set)) {
                command_buffer_.write_timestamp2(static_cast<VkPipelineStageFlagBits2>(to_vk(stage)), pool->vk_handle(), index);
            }
        }
        /// Performs the begin pipeline statistics query operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) override {
            VulkanQueryPool *pool = query_pool(query_set);
            if (pool == nullptr) {
                active_pipeline_statistics_query_pool_ = VK_NULL_HANDLE;
                active_pipeline_statistics_query_index_ = 0;
                return;
            }
            active_pipeline_statistics_query_pool_ = pool->vk_handle();
            active_pipeline_statistics_query_index_ = index;
            command_buffer_.begin_query(active_pipeline_statistics_query_pool_, index);
        }
        /// Performs the end pipeline statistics query operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end_pipeline_statistics_query() override {
            if (active_pipeline_statistics_query_pool_ != VK_NULL_HANDLE) {
                command_buffer_.end_query(active_pipeline_statistics_query_pool_, active_pipeline_statistics_query_index_);
                active_pipeline_statistics_query_pool_ = VK_NULL_HANDLE;
                active_pipeline_statistics_query_index_ = 0;
            }
        }
        /// Resolves query set into the concrete value used by the engine.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        /// @param dst Destination value or resource.
        /// @param dst_offset Offset from the beginning of the relevant range or buffer.
        /// @param stride `stride` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        void resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count, rhi::BufferHandle dst, u64 dst_offset, u64 stride, rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override {
            VulkanQueryPool *pool = query_pool(query_set);
            VulkanBuffer *dst_buffer = buffer(dst);
            if (pool != nullptr && dst_buffer != nullptr) {
                command_buffer_.copy_query_pool_results(pool->vk_handle(), first, count, dst_buffer->vk_handle(), dst_offset, stride, to_vk(flags));
            }
        }
        /// Adds the supplied value to the end or work queue.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void push_debug_group(const char *label) override { command_buffer_.begin_debug_label(label != nullptr ? label : "RHI debug group"); }
        /// Removes and returns or discards the next value from the container or queue.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void pop_debug_group() override { command_buffer_.end_debug_label(); }

        /// Returns the current or globally available finish value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        rhi::RhiExpected<rhi::CommandBufferHandle> finish() override {
            if (auto ended = record_.command_buffer.end(); !ended) {
                return VulkanRhiDeviceBridge::rhi_error_from_graphics(ended.error());
            }
            return bridge_.command_buffers_.insert(std::move(record_));
        }

      private:
        /// Performs the acceleration structure type operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr VkAccelerationStructureTypeKHR acceleration_structure_type(
            rhi::AccelerationStructureType type) noexcept {
            switch (type) {
                case rhi::AccelerationStructureType::BottomLevel: return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                case rhi::AccelerationStructureType::TopLevel: return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            }
            return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        }

        /// Builds flags.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr VkBuildAccelerationStructureFlagsKHR build_flags(
            rhi::AccelerationStructureBuildFlags flags) noexcept {
            VkBuildAccelerationStructureFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowUpdate)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowCompaction)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastTrace)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastBuild)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::MinimizeMemory)) out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
            return out;
        }

        /// Performs the geometry flags operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr VkGeometryFlagsKHR geometry_flags(
            rhi::AccelerationStructureGeometryFlags flags) noexcept {
            VkGeometryFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::Opaque)) out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::NoDuplicateAnyHitInvocation)) out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            return out;
        }

        /// Copies mode to its destination.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr VkCopyAccelerationStructureModeKHR copy_mode(
            rhi::AccelerationStructureCopyMode mode) noexcept {
            switch (mode) {
                case rhi::AccelerationStructureCopyMode::Clone: return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
                case rhi::AccelerationStructureCopyMode::Compact: return VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                case rhi::AccelerationStructureCopyMode::Serialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
                case rhi::AccelerationStructureCopyMode::Deserialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
            }
            return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
        }

        /// Performs the device address const operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param address `address` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static VkDeviceOrHostAddressConstKHR device_address_const(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressConstKHR out{};
            out.deviceAddress = address;
            return out;
        }

        /// Performs the device address operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param address `address` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static VkDeviceOrHostAddressKHR device_address(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressKHR out{};
            out.deviceAddress = address;
            return out;
        }

        /// Performs the shader binding table region operation for `VulkanRhiCommandEncoder` using the supplied arguments.
        ///
        /// @param region `region` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkStridedDeviceAddressRegionKHR shader_binding_table_region(
            const rhi::ShaderBindingTableRegion &region) const noexcept {
            VulkanBuffer *record = buffer(region.buffer);
            return VkStridedDeviceAddressRegionKHR{
                .deviceAddress = record ? record->device_address() + region.offset : 0,
                .stride = region.stride,
                .size = region.size,
            };
        }

        VulkanRhiDeviceBridge::CommandBufferRecord record_;
        VkQueryPool active_pipeline_statistics_query_pool_ = VK_NULL_HANDLE;
        u32 active_pipeline_statistics_query_index_ = 0;
    };

    /// Performs the checkout command buffer operation for `Vulkan` using the supplied arguments.
    ///
    /// @param family_index Zero-based index of the target element or entry.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    std::optional<VulkanRhiDeviceBridge::CommandBufferRecord> VulkanRhiDeviceBridge::checkout_command_buffer(
        u32 family_index) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::checkout_command_buffer");
        auto guard = command_buffer_free_list_.lock();
        auto it = guard->find(family_index);
        if (it == guard->end() || it->second.empty()) {
            return std::nullopt;
        }
        CommandBufferRecord record = std::move(it->second.back());
        it->second.pop_back();
        return record;
    }

    /// Performs the return command buffer operation for `Vulkan` using the supplied arguments.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::return_command_buffer(CommandBufferRecord &&record) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::return_command_buffer");
        const u32 family_index = record.pool.family_index();


        if (auto reset = record.pool.reset(); !reset) {
            Foundation::log_warn("Command pool reset failed while returning it to the free list ({}) -- dropping it instead of reusing.",
                                 reset.error().message);
            return;
        }
        auto guard = command_buffer_free_list_.lock();
        (*guard)[family_index].push_back(std::move(record));
    }

    /// Creates a command encoder from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> VulkanRhiDeviceBridge::create_command_encoder(
        const rhi::CommandEncoderDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_command_encoder");
        if (logical_device_ == nullptr || graphics_queue_ == nullptr) {
            return device_not_ready<unique_ptr<rhi::CommandEncoder>>("create_command_encoder");
        }
        if (desc.queue.queue != rhi::QueueClass::Graphics && desc.queue.queue != rhi::QueueClass::Compute &&
            desc.queue.queue != rhi::QueueClass::Transfer) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_command_encoder: Vulkan sparse/video queues do not support RHI command-buffer recording yet.");
        }
        if (auto queue_valid = validate_queue_lane(desc.queue, "create_command_encoder"); !queue_valid.has_value()) {
            return rhi::rhi_error(queue_valid.error().code, queue_valid.error().message);
        }
        VulkanQueue *queue = queue_for_lane(desc.queue);
        if (queue == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_command_encoder: queue lane is not available.");
        }


        CommandBufferRecord record;
        if (std::optional<CommandBufferRecord> reused = checkout_command_buffer(queue->family_index())) {
            record = std::move(*reused);
        } else {
            auto pool = VulkanCommandPool::create(logical_device_->vk_handle(), queue->family_index());
            if (!pool) {
                return rhi_error_from_graphics(pool.error());
            }
            auto command_buffer = VulkanCommandBuffer::allocate(logical_device_->vk_handle(), pool->vk_handle());
            if (!command_buffer) {
                return rhi_error_from_graphics(command_buffer.error());
            }
            record.pool = std::move(*pool);
            record.command_buffer = std::move(*command_buffer);
        }
        record.queue = desc.queue;

        const VkCommandBufferUsageFlags flags = desc.usage == rhi::CommandBufferUsage::OneTimeSubmit
            ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
            : VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if (auto began = record.command_buffer.begin(flags); !began) {
            return rhi_error_from_graphics(began.error());
        }

        return unique_ptr<rhi::CommandEncoder>(make_unique<VulkanRhiCommandEncoder>(*this, std::move(record)));
    }

    /// Destroys the command buffer identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_command_buffer");
        CommandBufferRecord *record = command_buffers_.find(handle);
        if (record != nullptr) {
            return_command_buffer(std::move(*record));
        }
        command_buffers_.erase(handle);
    }

    /// Creates a render bundle encoder from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> VulkanRhiDeviceBridge::create_render_bundle_encoder(
        const rhi::RenderBundleDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_render_bundle_encoder");
        if (logical_device_ == nullptr || graphics_queue_ == nullptr) {
            return device_not_ready<unique_ptr<rhi::RenderBundleEncoder>>("create_render_bundle_encoder");
        }

        auto pool = VulkanCommandPool::create(logical_device_->vk_handle(), graphics_queue_->family_index());
        if (!pool) {
            return rhi_error_from_graphics(pool.error());
        }
        auto command_buffer = VulkanCommandBuffer::allocate(logical_device_->vk_handle(), pool->vk_handle(), VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        if (!command_buffer) {
            return rhi_error_from_graphics(command_buffer.error());
        }

        vector<VkFormat> color_formats;
        color_formats.reserve(desc.color_formats.size());
        for (const rhi::Format format : desc.color_formats) {
            color_formats.push_back(to_vk(format));
        }
        const VkCommandBufferInheritanceRenderingInfo rendering_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .flags = 0,
            .viewMask = desc.view_mask,
            .colorAttachmentCount = static_cast<u32>(color_formats.size()),
            .pColorAttachmentFormats = color_formats.empty() ? nullptr : color_formats.data(),
            .depthAttachmentFormat = rhi::format_has_depth(desc.depth_stencil_format) ? to_vk(desc.depth_stencil_format) : VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = rhi::format_has_stencil(desc.depth_stencil_format) ? to_vk(desc.depth_stencil_format) : VK_FORMAT_UNDEFINED,
            .rasterizationSamples = to_vk(desc.samples),
        };
        const VkCommandBufferInheritanceInfo inheritance{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .pNext = &rendering_info,
        };
        if (auto began = command_buffer->begin_inherited(VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
                                                         &inheritance); !began) {
            return rhi_error_from_graphics(began.error());
        }

        RenderBundleRecord record{std::move(*pool), std::move(*command_buffer)};
        return unique_ptr<rhi::RenderBundleEncoder>(make_unique<VulkanRhiRenderBundleEncoder>(*this, std::move(record)));
    }

    /// Destroys the render bundle identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_render_bundle");
        render_bundles_.erase(handle);
    }


    /// Performs the native command buffer operation for `Vulkan` using the supplied arguments.
    ///
    /// @param encoder `encoder` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    VkCommandBuffer VulkanNativeAccessExtension::native_command_buffer(const rhi::CommandEncoder &encoder) const noexcept {
        ZoneScopedN("VulkanNativeAccessExtension::native_command_buffer");
        if (const auto *vulkan_encoder = dynamic_cast<const VulkanRhiCommandEncoder *>(&encoder)) {
            return vulkan_encoder->native_vk_command_buffer();
        }
        return VK_NULL_HANDLE;
    }

} // namespace SFT::Core::Vulkan
