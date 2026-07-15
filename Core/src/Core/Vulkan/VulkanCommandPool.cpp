#include "VulkanCommandPool.hpp"

namespace SFT::Core::Vulkan {

VulkanCommandPool::~VulkanCommandPool() { destroy(); }

VulkanCommandPool::VulkanCommandPool(VulkanCommandPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), family_index_(o.family_index_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
            o.family_index_ = 0;
        }

VulkanCommandPool &VulkanCommandPool::operator=(VulkanCommandPool &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                family_index_ = o.family_index_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
                o.family_index_ = 0;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanCommandPool> VulkanCommandPool::create(
            VkDevice device,
            u32 family_index,
            VkCommandPoolCreateFlags flags) noexcept {
            VkCommandPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .queueFamilyIndex = family_index,
            };
            VkCommandPool pool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateCommandPool failed.");
            VulkanCommandPool out;
            out.device_ = device;
            out.pool_ = pool;
            out.family_index_ = family_index;
            return out;
        }

[[nodiscard]] VkCommandPool VulkanCommandPool::vk_handle() const noexcept { return pool_; }

[[nodiscard]] bool VulkanCommandPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

[[nodiscard]] u32 VulkanCommandPool::family_index() const noexcept { return family_index_; }

[[nodiscard]] RendererExpected<vector<VkCommandBuffer>> VulkanCommandPool::allocate(
            u32 count,
            VkCommandBufferLevel level) const {
            VkCommandBufferAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = pool_,
                .level = level,
                .commandBufferCount = count,
            };
            vector<VkCommandBuffer> buffers(count, VK_NULL_HANDLE);
            if (vkAllocateCommandBuffers(device_, &info, buffers.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");
            return buffers;
        }

void VulkanCommandPool::free(vector<VkCommandBuffer> &buffers) noexcept {
            if (buffers.empty())
                return;
            vkFreeCommandBuffers(device_, pool_, static_cast<u32>(buffers.size()), buffers.data());
            buffers.clear();
        }

[[nodiscard]] RendererResult VulkanCommandPool::reset(VkCommandPoolResetFlags flags) noexcept {
            if (vkResetCommandPool(device_, pool_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandPool failed.");
            return {};
        }

void VulkanCommandPool::trim(VkCommandPoolTrimFlags flags) noexcept {
            vkTrimCommandPool(device_, pool_, flags);
        }

void VulkanCommandPool::destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyCommandPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            family_index_ = 0;
        }

} // namespace SFT::Core::Vulkan
