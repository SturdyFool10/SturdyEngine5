module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

export module Sturdy.Core:VulkanAllocator;

import :RendererError;
import :VulkanImage;
import Sturdy.Foundation;

using SFT::Core::RendererExpected;

export namespace SFT::Core::Vulkan {

    // Owns a VmaAllocator. Move-only; destroyed via destroy() or the destructor (whichever
    // comes first).
    class VulkanAllocator {
      public:
        struct CreateDesc {
            VkPhysicalDevice physical_device = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            VkInstance instance = VK_NULL_HANDLE;
            u32 api_version = 0;
            VmaAllocatorCreateFlags flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        };

        VulkanAllocator() = default;

        ~VulkanAllocator() { destroy(); }

        VulkanAllocator(const VulkanAllocator &) = delete;
        VulkanAllocator &operator=(const VulkanAllocator &) = delete;

        VulkanAllocator(VulkanAllocator &&o) noexcept
            : allocator_(o.allocator_) {
            o.allocator_ = VK_NULL_HANDLE;
        }

        VulkanAllocator &operator=(VulkanAllocator &&o) noexcept {
            if (this != &o) {
                destroy();
                allocator_ = o.allocator_;
                o.allocator_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // Imports Vulkan entry points from volk, then creates the allocator.
        [[nodiscard]] static RendererExpected<VulkanAllocator> create(const CreateDesc &desc) noexcept {
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
                return renderer_error(RendererErrorCode::InitializationFailed, "vmaCreateAllocator failed.");

            VulkanAllocator out;
            out.allocator_ = allocator;
            return out;
        }

        [[nodiscard]] VmaAllocator vk_handle() const noexcept { return allocator_; }
        [[nodiscard]] bool is_valid() const noexcept { return allocator_ != VK_NULL_HANDLE; }

        [[nodiscard]] RendererExpected<VulkanImage> create_image(
            VkDevice device,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept {
            return VulkanImage::create(device, allocator_, image_info, allocation_info);
        }

        void destroy() noexcept {
            if (allocator_ == VK_NULL_HANDLE)
                return;
            vmaDestroyAllocator(allocator_);
            allocator_ = VK_NULL_HANDLE;
        }

      private:
        VmaAllocator allocator_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
