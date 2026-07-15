#include "VulkanTimestamp.hpp"

namespace SFT::Core::Vulkan {

RendererExpected<vector<VkTimeDomainKHR>> get_calibrateable_time_domains(
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

RendererExpected<CalibratedClocks> get_calibrated_clocks(
        VkDevice device,
        VkTimeDomainKHR cpu_domain) noexcept {
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

f64 gpu_ticks_to_ns(u64 ticks, f32 timestamp_period) noexcept {
        return static_cast<f64>(ticks) * static_cast<f64>(timestamp_period);
    }

f64 gpu_ticks_to_ms(u64 ticks, f32 timestamp_period) noexcept {
        return gpu_ticks_to_ns(ticks, timestamp_period) / 1'000'000.0;
    }

VulkanTimestampPool::~VulkanTimestampPool() { destroy(); }

VulkanTimestampPool::VulkanTimestampPool(VulkanTimestampPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), query_count_(o.query_count_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }

VulkanTimestampPool &VulkanTimestampPool::operator=(VulkanTimestampPool &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanTimestampPool> VulkanTimestampPool::create(
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

[[nodiscard]] VkQueryPool VulkanTimestampPool::vk_handle() const noexcept { return pool_; }

[[nodiscard]] bool VulkanTimestampPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

[[nodiscard]] u32 VulkanTimestampPool::query_count() const noexcept { return query_count_; }

[[nodiscard]] RendererExpected<vector<u64>> VulkanTimestampPool::resolve(
            u32 first_query,
            u32 count,
            VkQueryResultFlags flags) const {
            if (count == 0)
                count = query_count_;
            vector<u64> ticks(count, 0);
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, ticks.size() * sizeof(u64), ticks.data(), sizeof(u64), flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetQueryPoolResults (timestamp) failed.");
            return ticks;
        }

void VulkanTimestampPool::reset(u32 first_query, u32 count) noexcept {
            vkResetQueryPool(device_, pool_, first_query, count == 0 ? query_count_ : count);
        }

void VulkanTimestampPool::destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyQueryPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
