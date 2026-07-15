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

    // Result of a calibrated clock query — correlates GPU ticks with a CPU clock sample.
    // max_deviation_ns is an upper bound on the synchronisation error between the two domains.
    struct CalibratedClocks {
        u64 gpu_ticks = 0;
        u64 cpu_ticks = 0;
        u64 max_deviation_ns = 0;
        VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    };

    // ─── Free functions ───────────────────────────────────────────────────────────

    // Returns the time domains supported for calibrated timestamps on this device.
    // Call before logical device creation to pick the best cpu_domain for your OS:
    //   Linux   → VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR (preferred) or CLOCK_MONOTONIC
    //   Windows → VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR
    [[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> get_calibrateable_time_domains(
        VkPhysicalDevice physical) noexcept;

    // Samples the GPU clock and a CPU clock simultaneously.
    // cpu_domain must have been confirmed present via get_calibrateable_time_domains().
    [[nodiscard]] RendererExpected<CalibratedClocks> get_calibrated_clocks(
        VkDevice device,
        VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR) noexcept;

    // Converts raw GPU timestamp ticks to nanoseconds using the device's timestampPeriod
    // (nanoseconds per tick, from VkPhysicalDeviceLimits::timestampPeriod).
    [[nodiscard]] f64 gpu_ticks_to_ns(u64 ticks, f32 timestamp_period) noexcept;

    // Converts a GPU tick delta to milliseconds.
    [[nodiscard]] f64 gpu_ticks_to_ms(u64 ticks, f32 timestamp_period) noexcept;

    // ─── VulkanTimestampPool ──────────────────────────────────────────────────────

    // Wraps a VK_QUERY_TYPE_TIMESTAMP query pool.
    //
    // Usage per frame:
    //   1. Call reset() (host-side via vkResetQueryPool, Vulkan 1.2+).
    //   2. Insert vkCmdWriteTimestamp2 calls at desired points in the command buffer.
    //   3. After the GPU has finished, call resolve() to read back raw ticks.
    //   4. Subtract pairs of ticks and pass to gpu_ticks_to_ms() for durations.
    //
    // Pair with get_calibrated_clocks() once per frame to anchor GPU time to wall clock.
    class VulkanTimestampPool {
      public:
        VulkanTimestampPool() = default;
        ~VulkanTimestampPool();

        VulkanTimestampPool(const VulkanTimestampPool &) = delete;
        VulkanTimestampPool &operator=(const VulkanTimestampPool &) = delete;

        VulkanTimestampPool(VulkanTimestampPool &&o) noexcept;
        VulkanTimestampPool &operator=(VulkanTimestampPool &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanTimestampPool> create(
            VkDevice device,
            u32 query_count) noexcept;

        [[nodiscard]] VkQueryPool vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] u32 query_count() const noexcept;

        // Read back raw GPU ticks for [first_query, first_query + count).
        // Results are available once the recording command buffer has finished executing.
        // Use VK_QUERY_RESULT_WAIT_BIT to block until results are ready.
        [[nodiscard]] RendererExpected<vector<u64>> resolve(
            u32 first_query = 0,
            u32 count = 0,
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) const;

        // Host-side reset — must be called before reusing queries in a new frame.
        void reset(u32 first_query = 0, u32 count = 0) noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueryPool pool_ = VK_NULL_HANDLE;
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
