module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanCommandBuffer;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

export namespace SFT::Core::Vulkan {

    class VulkanCommandBuffer {
      public:
        VulkanCommandBuffer() = default;
        ~VulkanCommandBuffer() { destroy(); }

        VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;
        VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;

        VulkanCommandBuffer(VulkanCommandBuffer &&o) noexcept
            : device_(o.device_), command_pool_(o.command_pool_), buffer_(o.buffer_), level_(o.level_) {
            o.device_ = VK_NULL_HANDLE;
            o.command_pool_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

        VulkanCommandBuffer &operator=(VulkanCommandBuffer &&o) noexcept {
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

        [[nodiscard]] static RendererExpected<VulkanCommandBuffer> allocate(
            VkDevice device,
            VkCommandPool command_pool,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) noexcept {
            VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = command_pool,
                .level = level,
                .commandBufferCount = 1,
            };

            VkCommandBuffer buffer = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(device, &info, &buffer) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");

            VulkanCommandBuffer out;
            out.device_ = device;
            out.command_pool_ = command_pool;
            out.buffer_ = buffer;
            out.level_ = level;
            return out;
        }

        [[nodiscard]] VkCommandBuffer vk_handle() const noexcept { return buffer_; }

        [[nodiscard]] VkCommandBufferSubmitInfo submit_info() const noexcept {
            return VkCommandBufferSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = buffer_,
                .deviceMask = 0,
            };
        }
        [[nodiscard]] bool is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkCommandPool command_pool() const noexcept { return command_pool_; }
        [[nodiscard]] VkCommandBufferLevel level() const noexcept { return level_; }

        [[nodiscard]] RendererResult begin(VkCommandBufferUsageFlags flags = 0) noexcept {
            VkCommandBufferBeginInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = flags,
                .pInheritanceInfo = nullptr,
            };
            if (vkBeginCommandBuffer(buffer_, &info) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkBeginCommandBuffer failed.");
            return {};
        }

        [[nodiscard]] RendererResult end() noexcept {
            if (vkEndCommandBuffer(buffer_) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkEndCommandBuffer failed.");
            return {};
        }

        [[nodiscard]] RendererResult reset(VkCommandBufferResetFlags flags = 0) noexcept {
            if (vkResetCommandBuffer(buffer_, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetCommandBuffer failed.");
            return {};
        }

        // Records an image-memory-barrier-only synchronization2 dependency.
        void pipeline_barrier2(const vector<VkImageMemoryBarrier2> &image_barriers) const noexcept {
            VkDependencyInfo dependency_info{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
                .pImageMemoryBarriers = image_barriers.data(),
            };
            vkCmdPipelineBarrier2(buffer_, &dependency_info);
        }

        void begin_rendering(const VkRenderingInfo &info) const noexcept {
            vkCmdBeginRendering(buffer_, &info);
        }

        void end_rendering() const noexcept {
            vkCmdEndRendering(buffer_);
        }

        void set_viewport(const VkViewport &viewport) const noexcept {
            vkCmdSetViewport(buffer_, 0, 1, &viewport);
        }

        void set_scissor(const VkRect2D &scissor) const noexcept {
            vkCmdSetScissor(buffer_, 0, 1, &scissor);
        }

        void bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const noexcept {
            vkCmdBindPipeline(buffer_, bind_point, pipeline);
        }

        void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 first_instance = 0) const noexcept {
            vkCmdDraw(buffer_, vertex_count, instance_count, first_vertex, first_instance);
        }

        void destroy() noexcept {
            if (buffer_ == VK_NULL_HANDLE)
                return;
            vkFreeCommandBuffers(device_, command_pool_, 1, &buffer_);
            buffer_ = VK_NULL_HANDLE;
            command_pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        VkCommandBuffer buffer_ = VK_NULL_HANDLE;
        VkCommandBufferLevel level_ = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    };

} // namespace SFT::Core::Vulkan
