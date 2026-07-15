#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

namespace SFT::Core::Vulkan {

    class VulkanCommandPool {
      public:
        VulkanCommandPool() = default;
        ~VulkanCommandPool();

        VulkanCommandPool(const VulkanCommandPool &) = delete;
        VulkanCommandPool &operator=(const VulkanCommandPool &) = delete;

        VulkanCommandPool(VulkanCommandPool &&o) noexcept;
        VulkanCommandPool &operator=(VulkanCommandPool &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanCommandPool> create(
            VkDevice device,
            u32 family_index,
            VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) noexcept;

        [[nodiscard]] VkCommandPool vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] u32 family_index() const noexcept;

        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate(
            u32 count,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) const;

        void free(vector<VkCommandBuffer> &buffers) noexcept;

        // Recycles all command buffers allocated from this pool back to the pool.
        [[nodiscard]] RendererResult reset(VkCommandPoolResetFlags flags = 0) noexcept;

        // Returns unused memory from the pool to the system allocator (Vulkan 1.1+).
        void trim(VkCommandPoolTrimFlags flags = 0) noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool pool_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;
    };

} // namespace SFT::Core::Vulkan
