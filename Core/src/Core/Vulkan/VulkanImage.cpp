#include "VulkanImage.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanImageView::~VulkanImageView() { destroy(); }

/// Performs the vulkan image view operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanImageView::VulkanImageView(VulkanImageView &&o) noexcept
            : device_(o.device_), view_(o.view_), format_(o.format_), view_type_(o.view_type_) {
            ZoneScopedN("VulkanImageView::VulkanImageView");
            o.device_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanImageView &VulkanImageView::operator=(VulkanImageView &&o) noexcept {
            ZoneScopedN("VulkanImageView::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                view_ = o.view_;
                format_ = o.format_;
                view_type_ = o.view_type_;
                o.device_ = VK_NULL_HANDLE;
                o.view_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanImageView> VulkanImageView::create(
            VkDevice device,
            const VkImageViewCreateInfo &info) noexcept {
            ZoneScopedN("VulkanImageView::create");
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImageView failed.");
            VulkanImageView out;
            out.device_ = device;
            out.view_ = view;
            out.format_ = info.format;
            out.view_type_ = info.viewType;
            return out;
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageView VulkanImageView::vk_handle() const noexcept { return view_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanImageView::is_valid() const noexcept { return view_ != VK_NULL_HANDLE; }

/// Formats the supplied value into the provided formatting context.
///
/// @return Returns the current format value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkFormat VulkanImageView::format() const noexcept { return format_; }

/// Returns the current or globally available view type value.
///
/// @return Returns the current view type value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageViewType VulkanImageView::view_type() const noexcept { return view_type_; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanImageView::destroy() noexcept {
            ZoneScopedN("VulkanImageView::destroy");
            if (view_ == VK_NULL_HANDLE)
                return;
            vkDestroyImageView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanImage::~VulkanImage() { destroy(); }

/// Performs the vulkan image operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanImage::VulkanImage(VulkanImage &&o) noexcept
            : device_(o.device_), allocator_(o.allocator_), image_(o.image_), allocation_(o.allocation_),
              format_(o.format_), extent_(o.extent_), mip_levels_(o.mip_levels_),
              array_layers_(o.array_layers_), image_type_(o.image_type_),
              usage_(o.usage_), owns_image_(o.owns_image_) {
            ZoneScopedN("VulkanImage::VulkanImage");
            o.device_ = VK_NULL_HANDLE;
            o.allocator_ = VK_NULL_HANDLE;
            o.image_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanImage &VulkanImage::operator=(VulkanImage &&o) noexcept {
            ZoneScopedN("VulkanImage::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                allocator_ = o.allocator_;
                image_ = o.image_;
                allocation_ = o.allocation_;
                format_ = o.format_;
                extent_ = o.extent_;
                mip_levels_ = o.mip_levels_;
                array_layers_ = o.array_layers_;
                image_type_ = o.image_type_;
                usage_ = o.usage_;
                owns_image_ = o.owns_image_;
                o.device_ = VK_NULL_HANDLE;
                o.allocator_ = VK_NULL_HANDLE;
                o.image_ = VK_NULL_HANDLE;
                o.allocation_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanImage> VulkanImage::create(
            VkDevice device,
            const VkImageCreateInfo &info) noexcept {
            ZoneScopedN("VulkanImage::create");
            VkImage image = VK_NULL_HANDLE;
            if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImage failed.");
            VulkanImage out;
            out.device_ = device;
            out.image_ = image;
            out.format_ = info.format;
            out.extent_ = info.extent;
            out.mip_levels_ = info.mipLevels;
            out.array_layers_ = info.arrayLayers;
            out.image_type_ = info.imageType;
            out.usage_ = info.usage;
            return out;
        }

/// Performs the borrow operation for `Vulkan` using the supplied arguments.
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
[[nodiscard]] VulkanImage VulkanImage::borrow(
            VkDevice device,
            VkImage image,
            VkFormat format,
            VkExtent3D extent,
            VkImageUsageFlags usage,
            u32 mip_levels,
            u32 array_layers,
            VkImageType image_type) noexcept {
            ZoneScopedN("VulkanImage::borrow");
            VulkanImage out;
            out.device_ = device;
            out.image_ = image;
            out.format_ = format;
            out.extent_ = extent;
            out.mip_levels_ = mip_levels;
            out.array_layers_ = array_layers;
            out.image_type_ = image_type;
            out.usage_ = usage;
            out.owns_image_ = false;
            return out;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param allocator Allocator used for storage owned by the operation.
/// @param image_info Description of the resource or operation to perform.
/// @param allocation_info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanImage> VulkanImage::create(
            VkDevice device,
            VmaAllocator allocator,
            const VkImageCreateInfo &image_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept {
            ZoneScopedN("VulkanImage::create");
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            if (vmaCreateImage(allocator, &image_info, &allocation_info, &image, &allocation, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaCreateImage failed.");
            VulkanImage out;
            out.device_ = device;
            out.allocator_ = allocator;
            out.image_ = image;
            out.allocation_ = allocation;
            out.format_ = image_info.format;
            out.extent_ = image_info.extent;
            out.mip_levels_ = image_info.mipLevels;
            out.array_layers_ = image_info.arrayLayers;
            out.image_type_ = image_info.imageType;
            out.usage_ = image_info.usage;
            return out;
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImage VulkanImage::vk_handle() const noexcept { return image_; }

/// Returns the current or globally available allocation value.
///
/// @return Returns the current allocation value.
/// @note This function does not throw exceptions.
[[nodiscard]] VmaAllocation VulkanImage::allocation() const noexcept { return allocation_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanImage::is_valid() const noexcept { return image_ != VK_NULL_HANDLE; }

/// Returns the current or globally available owns allocation value.
///
/// @return Returns the current owns allocation value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanImage::owns_allocation() const noexcept { return allocation_ != VK_NULL_HANDLE; }

/// Formats the supplied value into the provided formatting context.
///
/// @return Returns the current format value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkFormat VulkanImage::format() const noexcept { return format_; }

/// Returns the current or globally available extent value.
///
/// @return Returns the current extent value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkExtent3D VulkanImage::extent() const noexcept { return extent_; }

/// Returns the current or globally available mip levels value.
///
/// @return Returns the current mip levels value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanImage::mip_levels() const noexcept { return mip_levels_; }

/// Returns the current or globally available array layers value.
///
/// @return Returns the current array layers value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanImage::array_layers() const noexcept { return array_layers_; }

/// Returns the current or globally available image type value.
///
/// @return Returns the current image type value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageType VulkanImage::image_type() const noexcept { return image_type_; }

/// Returns the current or globally available usage value.
///
/// @return Returns the current usage value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageUsageFlags VulkanImage::usage() const noexcept { return usage_; }

/// Returns the current or globally available memory requirements value.
///
/// @return Returns the current memory requirements value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements VulkanImage::memory_requirements() const noexcept {
            ZoneScopedN("VulkanImage::memory_requirements");
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, image_, &req);
            return req;
        }

/// Returns the current or globally available memory requirements2 value.
///
/// @return Returns the current memory requirements2 value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements2 VulkanImage::memory_requirements2() const noexcept {
            ZoneScopedN("VulkanImage::memory_requirements2");
            VkImageMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = image_,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetImageMemoryRequirements2(device_, &query, &req);
            return req;
        }

/// Binds memory for subsequent operations.
///
/// @param memory `memory` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanImage::bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanImage::bind_memory");
            if (vkBindImageMemory(device_, image_, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindImageMemory failed.");
            return {};
        }

/// Performs the subresource layout operation for `Vulkan` using the supplied arguments.
///
/// @param subresource `subresource` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSubresourceLayout VulkanImage::subresource_layout(
            const VkImageSubresource &subresource) const noexcept {
            ZoneScopedN("VulkanImage::subresource_layout");
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, image_, &subresource, &layout);
            return layout;
        }

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
[[nodiscard]] RendererExpected<VulkanImageView> VulkanImage::create_view(
            VkImageAspectFlags aspect,
            VkImageViewType view_type,
            u32 base_mip,
            u32 mip_count,
            u32 base_layer,
            u32 layer_count) const noexcept {
            ZoneScopedN("VulkanImage::create_view");
            VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = image_,
                .viewType = view_type,
                .format = format_,
                .components = {
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = aspect,
                    .baseMipLevel = base_mip,
                    .levelCount = mip_count,
                    .baseArrayLayer = base_layer,
                    .layerCount = layer_count,
                },
            };
            return VulkanImageView::create(device_, info);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanImage::destroy() noexcept {
            ZoneScopedN("VulkanImage::destroy");
            if (image_ == VK_NULL_HANDLE)
                return;

            if (allocation_ != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator_, image_, allocation_);
                allocation_ = VK_NULL_HANDLE;
                allocator_ = VK_NULL_HANDLE;
            } else if (owns_image_) {
                vkDestroyImage(device_, image_, nullptr);
            }

            image_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            format_ = VK_FORMAT_UNDEFINED;
            extent_ = {};
            mip_levels_ = 1;
            array_layers_ = 1;
            image_type_ = VK_IMAGE_TYPE_2D;
            usage_ = 0;
            owns_image_ = true;
        }

} // namespace SFT::Core::Vulkan
