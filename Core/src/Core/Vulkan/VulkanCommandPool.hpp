#pragma once

#include <Foundation/src/Foundation.hpp>
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
        /// Constructs a `VulkanCommandPool` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanCommandPool() = default;
        /// Destroys the `VulkanCommandPool` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanCommandPool();

        /// Disables this construction form for `VulkanCommandPool`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanCommandPool(const VulkanCommandPool &) = delete;
        /// Assigns a new value to this `VulkanCommandPool`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanCommandPool &operator=(const VulkanCommandPool &) = delete;

        /// Constructs a `VulkanCommandPool` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanCommandPool(VulkanCommandPool &&o) noexcept;
        /// Assigns a new value to this `VulkanCommandPool`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanCommandPool &operator=(VulkanCommandPool &&o) noexcept;

        /// Creates a `VulkanCommandPool` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param family_index Zero-based index of the target element or entry.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanCommandPool> create(
            VkDevice device,
            u32 family_index,
            VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanCommandPool`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandPool vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanCommandPool`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Computes the family index required by the supplied values.
        ///
        /// @return Returns the current family index value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 family_index() const noexcept;

        /// Allocates storage or a resource.
        ///
        /// @param count Number of elements or operations to process.
        /// @param level `level` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate(
            u32 count,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) const;

        /// Releases previously allocated storage or resources.
        ///
        /// @param buffers Buffer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void free(vector<VkCommandBuffer> &buffers) noexcept;


        /// Resets the object to its baseline state.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset(VkCommandPoolResetFlags flags = 0) noexcept;


        /// Performs the trim operation for `VulkanCommandPool` using the supplied arguments.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note This function does not throw exceptions.
        void trim(VkCommandPoolTrimFlags flags = 0) noexcept;

        /// Destroys or releases the `VulkanCommandPool` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkCommandPool pool_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;
    };

} // namespace SFT::Core::Vulkan
