#pragma once

#include <Foundation/Foundation.hpp>
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


    class VulkanAllocator {
      public:
        struct CreateDesc {
            VkPhysicalDevice physical_device = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            VkInstance instance = VK_NULL_HANDLE;
            u32 api_version = 0;


            VmaAllocatorCreateFlags flags = 0;
        };

        /// Constructs a `VulkanAllocator` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanAllocator() = default;

        /// Destroys the `VulkanAllocator` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanAllocator();

        /// Disables this construction form for `VulkanAllocator`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAllocator(const VulkanAllocator &) = delete;
        /// Assigns a new value to this `VulkanAllocator`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAllocator &operator=(const VulkanAllocator &) = delete;

        /// Constructs a `VulkanAllocator` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanAllocator(VulkanAllocator &&o) noexcept;

        /// Assigns a new value to this `VulkanAllocator`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanAllocator &operator=(VulkanAllocator &&o) noexcept;


        /// Creates a `VulkanAllocator` resource or value from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanAllocator> create(const CreateDesc &desc) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanAllocator`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VmaAllocator vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanAllocator`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Creates a image from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param image_info Description of the resource or operation to perform.
        /// @param allocation_info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanImage> create_image(
            VkDevice device,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept;

        /// Creates a buffer from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param buffer_info Description of the resource or operation to perform.
        /// @param allocation_info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanBuffer> create_buffer(
            VkDevice device,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) const noexcept;

        /// Destroys or releases the `VulkanAllocator` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VmaAllocator allocator_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
