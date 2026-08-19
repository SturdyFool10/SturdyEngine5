#include <Core/Vulkan/VulkanTimestamp.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Returns the calibrateable time domains associated with this `Vulkan`.
///
/// @param physical `physical` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
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

/// Returns the calibrated clocks associated with this `Vulkan`.
///
/// @param device Device used or affected by the operation.
/// @param cpu_domain `cpu_domain` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
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

/// Performs the GPU ticks to ns operation for `Vulkan` using the supplied arguments.
///
/// @param ticks `ticks` value used by the operation.
/// @param timestamp_period `timestamp_period` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
f64 gpu_ticks_to_ns(u64 ticks, f32 timestamp_period) noexcept {
        return static_cast<f64>(ticks) * static_cast<f64>(timestamp_period);
    }

/// Performs the GPU ticks to ms operation for `Vulkan` using the supplied arguments.
///
/// @param ticks `ticks` value used by the operation.
/// @param timestamp_period `timestamp_period` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
f64 gpu_ticks_to_ms(u64 ticks, f32 timestamp_period) noexcept {
        return gpu_ticks_to_ns(ticks, timestamp_period) / 1'000'000.0;
    }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanTimestampPool::~VulkanTimestampPool() { destroy(); }

/// Performs the vulkan timestamp pool operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanTimestampPool::VulkanTimestampPool(VulkanTimestampPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), query_count_(o.query_count_) {
            ZoneScopedN("VulkanTimestampPool::VulkanTimestampPool");
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanTimestampPool &VulkanTimestampPool::operator=(VulkanTimestampPool &&o) noexcept {
            ZoneScopedN("VulkanTimestampPool::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param query_count Number of elements or operations to process.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanTimestampPool> VulkanTimestampPool::create(
            VkDevice device,
            u32 query_count) noexcept {
            ZoneScopedN("VulkanTimestampPool::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueryPool VulkanTimestampPool::vk_handle() const noexcept { return pool_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanTimestampPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

/// Queries count from the active backend or runtime state.
///
/// @return Returns the current query count value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanTimestampPool::query_count() const noexcept { return query_count_; }

/// Resolves the requested value into the concrete value used by the engine.
///
/// @param first_query `first_query` value used by the operation.
/// @param count Number of elements or operations to process.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<u64>> VulkanTimestampPool::resolve(
            u32 first_query,
            u32 count,
            VkQueryResultFlags flags) const {
            ZoneScopedN("VulkanTimestampPool::resolve");
            if (count == 0)
                count = query_count_;
            vector<u64> ticks(count, 0);
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, ticks.size() * sizeof(u64), ticks.data(), sizeof(u64), flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetQueryPoolResults (timestamp) failed.");
            return ticks;
        }

/// Resets the object to its baseline state.
///
/// @param first_query `first_query` value used by the operation.
/// @param count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanTimestampPool::reset(u32 first_query, u32 count) noexcept {
            ZoneScopedN("VulkanTimestampPool::reset");
            vkResetQueryPool(device_, pool_, first_query, count == 0 ? query_count_ : count);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanTimestampPool::destroy() noexcept {
            ZoneScopedN("VulkanTimestampPool::destroy");
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyQueryPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
