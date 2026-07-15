#include "VulkanQueryPool.hpp"

namespace SFT::Core::Vulkan {

VulkanQueryPool::~VulkanQueryPool() { destroy(); }

VulkanQueryPool::VulkanQueryPool(VulkanQueryPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_),
              query_type_(o.query_type_), query_count_(o.query_count_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }

VulkanQueryPool &VulkanQueryPool::operator=(VulkanQueryPool &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanQueryPool> VulkanQueryPool::create(
            VkDevice device,
            VkQueryType type,
            u32 count,
            VkQueryPipelineStatisticFlags pipeline_stats) noexcept {
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

[[nodiscard]] VkQueryPool VulkanQueryPool::vk_handle() const noexcept { return pool_; }

[[nodiscard]] bool VulkanQueryPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

[[nodiscard]] VkQueryType VulkanQueryPool::query_type() const noexcept { return query_type_; }

[[nodiscard]] u32 VulkanQueryPool::query_count() const noexcept { return query_count_; }

[[nodiscard]] RendererResult VulkanQueryPool::get_results(
            u32 first_query,
            u32 count,
            span<u8> data,
            VkDeviceSize stride,
            VkQueryResultFlags flags) noexcept {
            VkResult res = vkGetQueryPoolResults(device_, pool_, first_query, count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetQueryPoolResults failed.");
            return {};
        }

void VulkanQueryPool::reset(u32 first_query, u32 count) noexcept {
            vkResetQueryPool(device_, pool_, first_query, count == 0 ? query_count_ : count);
        }

void VulkanQueryPool::destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyQueryPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
