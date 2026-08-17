#include "VulkanQueryPool.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanQueryPool::~VulkanQueryPool() { destroy(); }

/// Performs the vulkan query pool operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanQueryPool::VulkanQueryPool(VulkanQueryPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_),
              query_type_(o.query_type_), query_count_(o.query_count_) {
            ZoneScopedN("VulkanQueryPool::VulkanQueryPool");
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanQueryPool &VulkanQueryPool::operator=(VulkanQueryPool &&o) noexcept {
            ZoneScopedN("VulkanQueryPool::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param type Type value to inspect, select, or convert.
/// @param count Number of elements or operations to process.
/// @param pipeline_stats Pipeline used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanQueryPool> VulkanQueryPool::create(
            VkDevice device,
            VkQueryType type,
            u32 count,
            VkQueryPipelineStatisticFlags pipeline_stats) noexcept {
            ZoneScopedN("VulkanQueryPool::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueryPool VulkanQueryPool::vk_handle() const noexcept { return pool_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanQueryPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

/// Queries type from the active backend or runtime state.
///
/// @return Returns the current query type value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueryType VulkanQueryPool::query_type() const noexcept { return query_type_; }

/// Queries count from the active backend or runtime state.
///
/// @return Returns the current query count value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanQueryPool::query_count() const noexcept { return query_count_; }

/// Returns the results associated with this `Vulkan`.
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
[[nodiscard]] RendererResult VulkanQueryPool::get_results(
            u32 first_query,
            u32 count,
            span<u8> data,
            VkDeviceSize stride,
            VkQueryResultFlags flags) noexcept {
            ZoneScopedN("VulkanQueryPool::get_results");
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetQueryPoolResults failed.");
            return {};
        }

/// Resets the object to its baseline state.
///
/// @param first_query `first_query` value used by the operation.
/// @param count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanQueryPool::reset(u32 first_query, u32 count) noexcept {
            ZoneScopedN("VulkanQueryPool::reset");
            vkResetQueryPool(device_, pool_, first_query, count == 0 ? query_count_ : count);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanQueryPool::destroy() noexcept {
            ZoneScopedN("VulkanQueryPool::destroy");
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyQueryPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
