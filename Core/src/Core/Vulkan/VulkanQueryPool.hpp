#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <span>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;

namespace SFT::Core::Vulkan {

    class VulkanQueryPool {
      public:
        VulkanQueryPool() = default;
        ~VulkanQueryPool();

        VulkanQueryPool(const VulkanQueryPool &) = delete;
        VulkanQueryPool &operator=(const VulkanQueryPool &) = delete;

        VulkanQueryPool(VulkanQueryPool &&o) noexcept;
        VulkanQueryPool &operator=(VulkanQueryPool &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanQueryPool> create(
            VkDevice device,
            VkQueryType type,
            u32 count,
            VkQueryPipelineStatisticFlags pipeline_stats = 0) noexcept;

        [[nodiscard]] VkQueryPool vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkQueryType query_type() const noexcept;
        [[nodiscard]] u32 query_count() const noexcept;

        // VK_NOT_READY is not treated as an error — use VK_QUERY_RESULT_WAIT_BIT to block.
        [[nodiscard]] RendererResult get_results(
            u32 first_query,
            u32 count,
            span<u8> data,
            VkDeviceSize stride,
            VkQueryResultFlags flags) noexcept;

        // Host-side reset (Vulkan 1.2+). GPU-side reset uses vkCmdResetQueryPool in a command buffer.
        void reset(u32 first_query = 0, u32 count = 0) noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueryPool pool_ = VK_NULL_HANDLE;
        VkQueryType query_type_ = VK_QUERY_TYPE_TIMESTAMP;
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
