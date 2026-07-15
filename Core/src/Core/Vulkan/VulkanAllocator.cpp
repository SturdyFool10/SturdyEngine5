#include "VulkanAllocator.hpp"

namespace SFT::Core::Vulkan {

VulkanAllocator::~VulkanAllocator() { destroy(); }

VulkanAllocator::VulkanAllocator(VulkanAllocator &&o) noexcept
            : allocator_(o.allocator_) {
            o.allocator_ = VK_NULL_HANDLE;
        }

VulkanAllocator &VulkanAllocator::operator=(VulkanAllocator &&o) noexcept {
            if (this != &o) {
                destroy();
                allocator_ = o.allocator_;
                o.allocator_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanAllocator> VulkanAllocator::create(const VulkanAllocator::CreateDesc &desc) noexcept {
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

[[nodiscard]] VmaAllocator VulkanAllocator::vk_handle() const noexcept { return allocator_; }

[[nodiscard]] bool VulkanAllocator::is_valid() const noexcept { return allocator_ != VK_NULL_HANDLE; }

[[nodiscard]] RendererExpected<VulkanImage> VulkanAllocator::create_image(
            VkDevice device,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept {
            return VulkanImage::create(device, allocator_, image_info, allocation_info);
        }

[[nodiscard]] RendererExpected<VulkanBuffer> VulkanAllocator::create_buffer(
            VkDevice device,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept {
            return VulkanBuffer::create(device, allocator_, buffer_info, allocation_info);
        }

void VulkanAllocator::destroy() noexcept {
            if (allocator_ == VK_NULL_HANDLE)
                return;
            vmaDestroyAllocator(allocator_);
            allocator_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
