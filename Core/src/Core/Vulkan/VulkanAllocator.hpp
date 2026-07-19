#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
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
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanImage.hpp>

using SFT::Core::RendererExpected;

namespace SFT::Core::Vulkan {

    // Owns a VmaAllocator. Move-only; destroyed via destroy() or the destructor (whichever
    // comes first).
    class VulkanAllocator {
      public:
        struct CreateDesc {
            VkPhysicalDevice physical_device = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            VkInstance instance = VK_NULL_HANDLE;
            u32 api_version = 0;
            // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT is deliberately not on by default: it
            // makes VMA tag *every* allocation with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, which is a
            // validation error unless the bufferDeviceAddress physical device feature is also
            // enabled at device-creation time (see VulkanBackendDevice.cpp's createDevice()) — only
            // pass it here once something actually requests
            // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and that feature has been enabled to match.
            VmaAllocatorCreateFlags flags = 0;
        };

        VulkanAllocator() = default;

        ~VulkanAllocator();

        VulkanAllocator(const VulkanAllocator &) = delete;
        VulkanAllocator &operator=(const VulkanAllocator &) = delete;

        VulkanAllocator(VulkanAllocator &&o) noexcept;

        VulkanAllocator &operator=(VulkanAllocator &&o) noexcept;

        // Imports Vulkan entry points from volk, then creates the allocator.
        [[nodiscard]] static RendererExpected<VulkanAllocator> create(const CreateDesc &desc) noexcept;

        [[nodiscard]] VmaAllocator vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] RendererExpected<VulkanImage> create_image(
            VkDevice device,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept;

        [[nodiscard]] RendererExpected<VulkanBuffer> create_buffer(
            VkDevice device,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept;

        void destroy() noexcept;

      private:
        VmaAllocator allocator_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
