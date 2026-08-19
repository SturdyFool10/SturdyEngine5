#include <Core/Vulkan/VulkanCommandPool.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanCommandPool::~VulkanCommandPool() { destroy(); }

/// Performs the vulkan command pool operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanCommandPool::VulkanCommandPool(VulkanCommandPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_), family_index_(o.family_index_) {
            ZoneScopedN("VulkanCommandPool::VulkanCommandPool");
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
            o.family_index_ = 0;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanCommandPool &VulkanCommandPool::operator=(VulkanCommandPool &&o) noexcept {
            ZoneScopedN("VulkanCommandPool::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param family_index Zero-based index of the target element or entry.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanCommandPool> VulkanCommandPool::create(
            VkDevice device,
            u32 family_index,
            VkCommandPoolCreateFlags flags) noexcept {
            ZoneScopedN("VulkanCommandPool::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkCommandPool VulkanCommandPool::vk_handle() const noexcept { return pool_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanCommandPool::is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

/// Computes the family index required by the supplied values.
///
/// @return Returns the current family index value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanCommandPool::family_index() const noexcept { return family_index_; }

/// Allocates storage or a resource.
///
/// @param count Number of elements or operations to process.
/// @param level `level` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
[[nodiscard]] RendererExpected<vector<VkCommandBuffer>> VulkanCommandPool::allocate(
            u32 count,
            VkCommandBufferLevel level) const {
            ZoneScopedN("VulkanCommandPool::allocate");
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

/// Releases previously allocated storage or resources.
///
/// @param buffers Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandPool::free(vector<VkCommandBuffer> &buffers) noexcept {
            ZoneScopedN("VulkanCommandPool::free");
            if (buffers.empty())
                return;
            vkFreeCommandBuffers(device_, pool_, static_cast<u32>(buffers.size()), buffers.data());
            buffers.clear();
        }

/// Resets the object to its baseline state.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanCommandPool::reset(VkCommandPoolResetFlags flags) noexcept {
            ZoneScopedN("VulkanCommandPool::reset");
            if (vkResetCommandPool(device_, pool_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandPool failed.");
            return {};
        }

/// Performs the trim operation for `Vulkan` using the supplied arguments.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanCommandPool::trim(VkCommandPoolTrimFlags flags) noexcept {
            ZoneScopedN("VulkanCommandPool::trim");
            vkTrimCommandPool(device_, pool_, flags);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanCommandPool::destroy() noexcept {
            ZoneScopedN("VulkanCommandPool::destroy");
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyCommandPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            family_index_ = 0;
        }

} // namespace SFT::Core::Vulkan
