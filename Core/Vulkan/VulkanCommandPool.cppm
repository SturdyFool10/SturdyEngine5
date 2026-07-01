module;
#include "volk.h"
#include <vector>

export module Sturdy.Core:VulkanCommandPool;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

export namespace SFT::Core::Vulkan {

    class VulkanCommandPool {
      public:
        VulkanCommandPool() = default;
        ~VulkanCommandPool() { destroy(); }

        VulkanCommandPool(const VulkanCommandPool &) = delete;
        VulkanCommandPool &operator=(const VulkanCommandPool &) = delete;

        VulkanCommandPool(VulkanCommandPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), family_index_(o.family_index_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
            o.family_index_ = 0;
        }
        VulkanCommandPool &operator=(VulkanCommandPool &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                family_index_ = o.family_index_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
                o.family_index_ = 0;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanCommandPool> create(
            VkDevice device,
            u32 family_index,
            VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) noexcept {
            VkCommandPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .queueFamilyIndex = family_index,
            };
            VkCommandPool pool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateCommandPool failed.");
            VulkanCommandPool out;
            out.device_ = device;
            out.pool_ = pool;
            out.family_index_ = family_index;
            return out;
        }

        [[nodiscard]] VkCommandPool vk_handle() const noexcept { return pool_; }
        [[nodiscard]] bool is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }
        [[nodiscard]] u32 family_index() const noexcept { return family_index_; }

        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate(
            u32 count,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) const {
            VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = pool_,
                .level = level,
                .commandBufferCount = count,
            };
            vector<VkCommandBuffer> buffers(count, VK_NULL_HANDLE);
            if (vkAllocateCommandBuffers(device_, &info, buffers.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");
            return buffers;
        }

        void free(vector<VkCommandBuffer> &buffers) noexcept {
            if (buffers.empty())
                return;
            vkFreeCommandBuffers(device_, pool_, static_cast<u32>(buffers.size()), buffers.data());
            buffers.clear();
        }

        // Recycles all command buffers allocated from this pool back to the pool.
        [[nodiscard]] RendererResult reset(VkCommandPoolResetFlags flags = 0) noexcept {
            if (vkResetCommandPool(device_, pool_, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetCommandPool failed.");
            return {};
        }

        // Returns unused memory from the pool to the system allocator (Vulkan 1.1+).
        void trim(VkCommandPoolTrimFlags flags = 0) noexcept {
            vkTrimCommandPool(device_, pool_, flags);
        }

        void destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyCommandPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            family_index_ = 0;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool pool_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;
    };

} // namespace SFT::Core::Vulkan
