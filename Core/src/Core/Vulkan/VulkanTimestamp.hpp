#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
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


    struct CalibratedClocks {
        u64 gpu_ticks = 0;
        u64 cpu_ticks = 0;
        u64 max_deviation_ns = 0;
        VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    };


    /// Returns the requested calibrateable time domains.
    ///
    /// @param physical `physical` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> get_calibrateable_time_domains(
        VkPhysicalDevice physical) noexcept;


    /// Returns the requested calibrated clocks.
    ///
    /// @param device Device used or affected by the operation.
    /// @param cpu_domain `cpu_domain` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RendererExpected<CalibratedClocks> get_calibrated_clocks(
        VkDevice device,
        VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR) noexcept;


    /// Performs the GPU ticks to ns operation using the supplied arguments.
    ///
    /// @param ticks `ticks` value used by the operation.
    /// @param timestamp_period `timestamp_period` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f64 gpu_ticks_to_ns(u64 ticks, f32 timestamp_period) noexcept;


    /// Performs the GPU ticks to ms operation using the supplied arguments.
    ///
    /// @param ticks `ticks` value used by the operation.
    /// @param timestamp_period `timestamp_period` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f64 gpu_ticks_to_ms(u64 ticks, f32 timestamp_period) noexcept;


    class VulkanTimestampPool {
      public:
        /// Constructs a `VulkanTimestampPool` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanTimestampPool() = default;
        /// Destroys the `VulkanTimestampPool` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanTimestampPool();

        /// Disables this construction form for `VulkanTimestampPool`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanTimestampPool(const VulkanTimestampPool &) = delete;
        /// Assigns a new value to this `VulkanTimestampPool`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanTimestampPool &operator=(const VulkanTimestampPool &) = delete;

        /// Constructs a `VulkanTimestampPool` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanTimestampPool(VulkanTimestampPool &&o) noexcept;
        /// Assigns a new value to this `VulkanTimestampPool`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanTimestampPool &operator=(VulkanTimestampPool &&o) noexcept;

        /// Creates a `VulkanTimestampPool` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param query_count Number of elements or operations to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanTimestampPool> create(
            VkDevice device,
            u32 query_count) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanTimestampPool`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueryPool vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanTimestampPool`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Queries count from the active backend or runtime state.
        ///
        /// @return Returns the current query count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 query_count() const noexcept;


        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param first_query `first_query` value used by the operation.
        /// @param count Number of elements or operations to process.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] RendererExpected<vector<u64>> resolve(
            u32 first_query = 0,
            u32 count = 0,
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) const;


        /// Resets the object to its baseline state.
        ///
        /// @param first_query `first_query` value used by the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        void reset(u32 first_query = 0, u32 count = 0) noexcept;

        /// Destroys or releases the `VulkanTimestampPool` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueryPool pool_ = VK_NULL_HANDLE;
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
