#define VMA_IMPLEMENTATION
#include <Core/Vulkan/VulkanAllocator.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanAllocator::~VulkanAllocator() { destroy(); }

/// Performs the vulkan allocator operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanAllocator::VulkanAllocator(VulkanAllocator &&o) noexcept
            : allocator_(o.allocator_) {
            ZoneScopedN("VulkanAllocator::VulkanAllocator");
            o.allocator_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanAllocator &VulkanAllocator::operator=(VulkanAllocator &&o) noexcept {
            ZoneScopedN("VulkanAllocator::operator=");
            if (this != &o) {
                destroy();
                allocator_ = o.allocator_;
                o.allocator_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanAllocator> VulkanAllocator::create(const VulkanAllocator::CreateDesc &desc) noexcept {
            ZoneScopedN("VulkanAllocator::create");
            VmaVulkanFunctions functions{};
            VmaAllocatorCreateInfo info{
                .flags = desc.flags,
                .physicalDevice = desc.physical_device,
                .device = desc.device,
                .pVulkanFunctions = &functions,
                .instance = desc.instance,
                .vulkanApiVersion = desc.api_version,
            };
            vmaImportVulkanFunctionsFromVolk(&info, &functions);

            VmaAllocator allocator = VK_NULL_HANDLE;
            if (vmaCreateAllocator(&info, &allocator) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "vmaCreateAllocator failed.");

            VulkanAllocator out;
            out.allocator_ = allocator;
            return out;
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VmaAllocator VulkanAllocator::vk_handle() const noexcept { return allocator_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanAllocator::is_valid() const noexcept { return allocator_ != VK_NULL_HANDLE; }

/// Creates a image from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param image_info Description of the resource or operation to perform.
/// @param allocation_info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanImage> VulkanAllocator::create_image(
            VkDevice device,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept {
            ZoneScopedN("VulkanAllocator::create_image");
            return VulkanImage::create(device, allocator_, image_info, allocation_info);
        }

/// Creates a buffer from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param buffer_info Description of the resource or operation to perform.
/// @param allocation_info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanBuffer> VulkanAllocator::create_buffer(
            VkDevice device,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept {
            ZoneScopedN("VulkanAllocator::create_buffer");
            return VulkanBuffer::create(device, allocator_, buffer_info, allocation_info);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanAllocator::destroy() noexcept {
            ZoneScopedN("VulkanAllocator::destroy");
            if (allocator_ == VK_NULL_HANDLE)
                return;
            vmaDestroyAllocator(allocator_);
            allocator_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
