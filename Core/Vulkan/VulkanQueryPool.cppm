module;
#pragma region Imports
#include "volk.h"
#include <span>
#pragma endregion

export module Sturdy.Core:VulkanQueryPool;

import :GraphicsBackendError;
import Sturdy.Foundation;

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;

export namespace SFT::Core::Vulkan {

    class VulkanQueryPool {
      public:
        VulkanQueryPool() = default;
        ~VulkanQueryPool() { destroy(); }

        VulkanQueryPool(const VulkanQueryPool &) = delete;
        VulkanQueryPool &operator=(const VulkanQueryPool &) = delete;

        VulkanQueryPool(VulkanQueryPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_),
              query_type_(o.query_type_), query_count_(o.query_count_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }
        VulkanQueryPool &operator=(VulkanQueryPool &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                query_type_ = o.query_type_;
                query_count_ = o.query_count_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanQueryPool> create(
            VkDevice device,
            VkQueryType type,
            u32 count,
            VkQueryPipelineStatisticFlags pipeline_stats = 0) noexcept {
            VkQueryPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = type,
                .queryCount = count,
                .pipelineStatistics = pipeline_stats,
            };
            VkQueryPool pool = VK_NULL_HANDLE;
            if (vkCreateQueryPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateQueryPool failed.");
            VulkanQueryPool out;
            out.device_ = device;
            out.pool_ = pool;
            out.query_type_ = type;
            out.query_count_ = count;
            return out;
        }

        [[nodiscard]] VkQueryPool vk_handle() const noexcept { return pool_; }
        [[nodiscard]] bool is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkQueryType query_type() const noexcept { return query_type_; }
        [[nodiscard]] u32 query_count() const noexcept { return query_count_; }

        // VK_NOT_READY is not treated as an error — use VK_QUERY_RESULT_WAIT_BIT to block.
        [[nodiscard]] RendererResult get_results(
            u32 first_query,
            u32 count,
            span<u8> data,
            VkDeviceSize stride,
            VkQueryResultFlags flags) noexcept {
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetQueryPoolResults failed.");
            return {};
        }

        // Host-side reset (Vulkan 1.2+). GPU-side reset uses vkCmdResetQueryPool in a command buffer.
        void reset(u32 first_query = 0, u32 count = 0) noexcept {
            vkResetQueryPool(device_, pool_, first_query, count == 0 ? query_count_ : count);
        }

        void destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyQueryPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueryPool pool_ = VK_NULL_HANDLE;
        VkQueryType query_type_ = VK_QUERY_TYPE_TIMESTAMP;
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
