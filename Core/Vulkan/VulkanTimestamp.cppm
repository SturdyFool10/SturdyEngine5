module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanTimestamp;

import :GraphicsBackendError;
import Sturdy.Foundation;

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

export namespace SFT::Core::Vulkan {

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
    [[nodiscard]] inline RendererExpected<vector<VkTimeDomainKHR>> get_calibrateable_time_domains(
        VkPhysicalDevice physical) noexcept {
        u32 count = 0;
        if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(physical, &count, nullptr) != VK_SUCCESS)
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                  "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (count) failed.");
        vector<VkTimeDomainKHR> domains(count);
        if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(physical, &count, domains.data()) != VK_SUCCESS)
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                  "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (populate) failed.");
        return domains;
    }

    // Samples the GPU clock and a CPU clock simultaneously.
    // cpu_domain must have been confirmed present via get_calibrateable_time_domains().
    [[nodiscard]] inline RendererExpected<CalibratedClocks> get_calibrated_clocks(
        VkDevice device,
        VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR) noexcept {
        VkCalibratedTimestampInfoKHR infos[2]{
            {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
             .pNext = nullptr,
             .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
            {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
             .pNext = nullptr,
             .timeDomain = cpu_domain},
        };
        u64 timestamps[2]{};
        u64 max_deviation = 0;
        if (vkGetCalibratedTimestampsKHR(device, 2, infos, timestamps, &max_deviation) != VK_SUCCESS)
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                  "vkGetCalibratedTimestampsKHR failed.");
        return CalibratedClocks{
            .gpu_ticks = timestamps[0],
            .cpu_ticks = timestamps[1],
            .max_deviation_ns = max_deviation,
            .cpu_domain = cpu_domain,
        };
    }

    // Converts raw GPU timestamp ticks to nanoseconds using the device's timestampPeriod
    // (nanoseconds per tick, from VkPhysicalDeviceLimits::timestampPeriod).
    [[nodiscard]] inline f64 gpu_ticks_to_ns(u64 ticks, f32 timestamp_period) noexcept {
        return static_cast<f64>(ticks) * static_cast<f64>(timestamp_period);
    }

    // Converts a GPU tick delta to milliseconds.
    [[nodiscard]] inline f64 gpu_ticks_to_ms(u64 ticks, f32 timestamp_period) noexcept {
        return gpu_ticks_to_ns(ticks, timestamp_period) / 1'000'000.0;
    }

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
        ~VulkanTimestampPool() { destroy(); }

        VulkanTimestampPool(const VulkanTimestampPool &) = delete;
        VulkanTimestampPool &operator=(const VulkanTimestampPool &) = delete;

        VulkanTimestampPool(VulkanTimestampPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), query_count_(o.query_count_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }
        VulkanTimestampPool &operator=(VulkanTimestampPool &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                query_count_ = o.query_count_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanTimestampPool> create(
            VkDevice device,
            u32 query_count) noexcept {
            VkQueryPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = query_count,
            };
            VkQueryPool pool = VK_NULL_HANDLE;
            if (vkCreateQueryPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkCreateQueryPool (timestamp) failed.");
            VulkanTimestampPool out;
            out.device_ = device;
            out.pool_ = pool;
            out.query_count_ = query_count;
            return out;
        }

        [[nodiscard]] VkQueryPool vk_handle() const noexcept { return pool_; }
        [[nodiscard]] bool is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }
        [[nodiscard]] u32 query_count() const noexcept { return query_count_; }

        // Read back raw GPU ticks for [first_query, first_query + count).
        // Results are available once the recording command buffer has finished executing.
        // Use VK_QUERY_RESULT_WAIT_BIT to block until results are ready.
        [[nodiscard]] RendererExpected<vector<u64>> resolve(
            u32 first_query = 0,
            u32 count = 0,
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) const {
            if (count == 0)
                count = query_count_;
            vector<u64> ticks(count, 0);
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, ticks.size() * sizeof(u64), ticks.data(), sizeof(u64), flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetQueryPoolResults (timestamp) failed.");
            return ticks;
        }

        // Host-side reset — must be called before reusing queries in a new frame.
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
        u32 query_count_ = 0;
    };

} // namespace SFT::Core::Vulkan
