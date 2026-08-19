#pragma once

#include <Foundation/Foundation.hpp>
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
        /// Constructs a `VulkanQueryPool` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanQueryPool() = default;
        /// Destroys the `VulkanQueryPool` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanQueryPool();

        /// Disables this construction form for `VulkanQueryPool`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanQueryPool(const VulkanQueryPool &) = delete;
        /// Assigns a new value to this `VulkanQueryPool`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanQueryPool &operator=(const VulkanQueryPool &) = delete;

        /// Constructs a `VulkanQueryPool` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanQueryPool(VulkanQueryPool &&o) noexcept;
        /// Assigns a new value to this `VulkanQueryPool`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanQueryPool &operator=(VulkanQueryPool &&o) noexcept;

        /// Creates a `VulkanQueryPool` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param count Number of elements or operations to process.
        /// @param pipeline_stats Pipeline used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanQueryPool> create(
            VkDevice device,
            VkQueryType type,
            u32 count,
            VkQueryPipelineStatisticFlags pipeline_stats = 0) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanQueryPool`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueryPool vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanQueryPool`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Queries type from the active backend or runtime state.
        ///
        /// @return Returns the current query type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueryType query_type() const noexcept;
        /// Queries count from the active backend or runtime state.
        ///
        /// @return Returns the current query count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 query_count() const noexcept;


        /// Returns the results associated with this `VulkanQueryPool`.
        ///
        /// @param first_query `first_query` value used by the operation.
        /// @param count Number of elements or operations to process.
        /// @param data Data consumed or referenced by the operation.
        /// @param stride `stride` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult get_results(
            u32 first_query,
            u32 count,
            span<u8> data,
            VkDeviceSize stride,
            VkQueryResultFlags flags) noexcept;


        /// Resets the object to its baseline state.
        ///
        /// @param first_query `first_query` value used by the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        void reset(u32 first_query = 0, u32 count = 0) noexcept;

        /// Destroys or releases the `VulkanQueryPool` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueryPool pool_ = VK_NULL_HANDLE;
        VkQueryType query_type_ = VK_QUERY_TYPE_TIMESTAMP;
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
