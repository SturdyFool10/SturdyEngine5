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

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {


    class VulkanImageView {
      public:
        /// Constructs a `VulkanImageView` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanImageView() = default;
        /// Destroys the `VulkanImageView` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanImageView();

        /// Disables this construction form for `VulkanImageView`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanImageView(const VulkanImageView &) = delete;
        /// Assigns a new value to this `VulkanImageView`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanImageView &operator=(const VulkanImageView &) = delete;

        /// Constructs a `VulkanImageView` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanImageView(VulkanImageView &&o) noexcept;
        /// Assigns a new value to this `VulkanImageView`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanImageView &operator=(VulkanImageView &&o) noexcept;

        /// Creates a `VulkanImageView` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanImageView> create(
            VkDevice device,
            const VkImageViewCreateInfo &info) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanImageView`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageView vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanImageView`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the current format value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkFormat format() const noexcept;
        /// Returns the current or globally available view type value.
        ///
        /// @return Returns the current view type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageViewType view_type() const noexcept;

        /// Destroys or releases the `VulkanImageView` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkImageView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkImageViewType view_type_ = VK_IMAGE_VIEW_TYPE_2D;
    };


    class VulkanImage {
      public:
        /// Constructs a `VulkanImage` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanImage() = default;
        /// Destroys the `VulkanImage` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanImage();

        /// Disables this construction form for `VulkanImage`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanImage(const VulkanImage &) = delete;
        /// Assigns a new value to this `VulkanImage`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanImage &operator=(const VulkanImage &) = delete;

        /// Constructs a `VulkanImage` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanImage(VulkanImage &&o) noexcept;
        /// Assigns a new value to this `VulkanImage`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanImage &operator=(VulkanImage &&o) noexcept;

        /// Creates a `VulkanImage` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            const VkImageCreateInfo &info) noexcept;

        /// Performs the borrow operation for `VulkanImage` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        /// @param image `image` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param extent `extent` value used by the operation.
        /// @param usage Usage flags or category applied to the resource.
        /// @param mip_levels `mip_levels` value used by the operation.
        /// @param array_layers `array_layers` value used by the operation.
        /// @param image_type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static VulkanImage borrow(
            VkDevice device,
            VkImage image,
            VkFormat format,
            VkExtent3D extent,
            VkImageUsageFlags usage,
            u32 mip_levels = 1,
            u32 array_layers = 1,
            VkImageType image_type = VK_IMAGE_TYPE_2D) noexcept;

        /// Creates a `VulkanImage` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param image_info Description of the resource or operation to perform.
        /// @param allocation_info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanImage> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanImage`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImage vk_handle() const noexcept;
        /// Returns the current or globally available allocation value.
        ///
        /// @return Returns the current allocation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VmaAllocation allocation() const noexcept;
        /// Reports whether valid holds for this `VulkanImage`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the current or globally available owns allocation value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool owns_allocation() const noexcept;
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the current format value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkFormat format() const noexcept;
        /// Returns the current or globally available extent value.
        ///
        /// @return Returns the current extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkExtent3D extent() const noexcept;
        /// Returns the current or globally available mip levels value.
        ///
        /// @return Returns the current mip levels value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 mip_levels() const noexcept;
        /// Returns the current or globally available array layers value.
        ///
        /// @return Returns the current array layers value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 array_layers() const noexcept;
        /// Returns the current or globally available image type value.
        ///
        /// @return Returns the current image type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageType image_type() const noexcept;
        /// Returns the current or globally available usage value.
        ///
        /// @return Returns the current usage value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageUsageFlags usage() const noexcept;

        /// Returns the current or globally available memory requirements value.
        ///
        /// @return Returns the current memory requirements value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept;

        /// Returns the current or globally available memory requirements2 value.
        ///
        /// @return Returns the current memory requirements2 value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept;

        /// Binds memory for subsequent operations.
        ///
        /// @param memory `memory` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept;

        /// Performs the subresource layout operation for `VulkanImage` using the supplied arguments.
        ///
        /// @param subresource `subresource` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSubresourceLayout subresource_layout(
            const VkImageSubresource &subresource) const noexcept;


        /// Creates a view from the supplied parameters.
        ///
        /// @param aspect `aspect` value used by the operation.
        /// @param view_type Type value to inspect, select, or convert.
        /// @param base_mip `base_mip` value used by the operation.
        /// @param mip_count Number of elements or operations to process.
        /// @param base_layer `base_layer` value used by the operation.
        /// @param layer_count Number of elements or operations to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanImageView> create_view(
            VkImageAspectFlags aspect,
            VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
            u32 base_mip = 0,
            u32 mip_count = VK_REMAINING_MIP_LEVELS,
            u32 base_layer = 0,
            u32 layer_count = VK_REMAINING_ARRAY_LAYERS) const noexcept;

        /// Destroys or releases the `VulkanImage` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkExtent3D extent_ = {};
        u32 mip_levels_ = 1;
        u32 array_layers_ = 1;
        VkImageType image_type_ = VK_IMAGE_TYPE_2D;
        VkImageUsageFlags usage_ = 0;
        bool owns_image_ = true;
    };

} // namespace SFT::Core::Vulkan
