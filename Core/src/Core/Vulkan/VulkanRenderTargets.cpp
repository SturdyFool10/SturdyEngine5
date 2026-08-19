#include <Core/Vulkan/VulkanRenderTargets.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @param image `image` value used by the operation.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageViewCreateInfo VulkanImageViewDesc::to_vk(VkImage image) const noexcept {
            ZoneScopedN("VulkanImageViewDesc::to_vk");
            return VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = image,
                .viewType = type,
                .format = format,
                .components = components,
                .subresourceRange = {
                    .aspectMask = aspects,
                    .baseMipLevel = base_mip,
                    .levelCount = mip_count,
                    .baseArrayLayer = base_layer,
                    .layerCount = layer_count,
                },
            };
        }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanAttachmentRef::is_valid() const noexcept {
            ZoneScopedN("VulkanAttachmentRef::is_valid");
            return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
        }

/// Returns the current or globally available extent 2d value.
///
/// @return Returns the current extent 2d value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkExtent2D VulkanAttachmentRef::extent_2d() const noexcept {
            ZoneScopedN("VulkanAttachmentRef::extent_2d");
            return VkExtent2D{.width = extent.width, .height = extent.height};
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param allocator Allocator used for storage owned by the operation.
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanAttachmentImage> VulkanAttachmentImage::create(
            VkDevice device,
            VmaAllocator allocator,
            const VulkanAttachmentImageDesc &desc) noexcept {
            ZoneScopedN("VulkanAttachmentImage::create");
            const VkImageAspectFlags aspects = desc.aspects != 0 ? desc.aspects : default_aspect_for_format(desc.format);
            VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = desc.image_pnext,
                .flags = desc.image_flags,
                .imageType = desc.image_type,
                .format = desc.format,
                .extent = desc.extent,
                .mipLevels = desc.mip_levels,
                .arrayLayers = desc.array_layers,
                .samples = desc.samples,
                .tiling = desc.tiling,
                .usage = desc.usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = desc.initial_layout,
            };
            VmaAllocationCreateInfo allocation_info{
                .flags = desc.allocation_flags,
                .usage = desc.memory_usage,
            };

            auto image_result = VulkanImage::create(device, allocator, image_info, allocation_info);
            if (!image_result.has_value()) [[unlikely]] {
                return graphics_backend_error(image_result.error().code, image_result.error().message);
            }

            VulkanImage image = std::move(*image_result);
            VulkanImageViewDesc view_desc{
                .type = desc.view_type,
                .format = desc.format,
                .aspects = aspects,
                .mip_count = desc.mip_levels,
                .layer_count = desc.array_layers,
            };
            auto view_info = view_desc.to_vk(image.vk_handle());
            view_info.pNext = desc.view_pnext;
            auto view_result = VulkanImageView::create(device, view_info);
            if (!view_result.has_value()) [[unlikely]] {
                return graphics_backend_error(view_result.error().code, view_result.error().message);
            }

            VulkanAttachmentImage out;
            out.image_ = std::move(image);
            out.view_ = std::move(*view_result);
            out.ref_ = VulkanAttachmentRef{
                .image = out.image_.vk_handle(),
                .view = out.view_.vk_handle(),
                .format = desc.format,
                .extent = desc.extent,
                .usage = desc.usage,
                .aspects = aspects,
                .samples = desc.samples,
                .mip_levels = desc.mip_levels,
                .array_layers = desc.array_layers,
            };
            return out;
        }

/// Creates a color from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param allocator Allocator used for storage owned by the operation.
/// @param format Format used for the resource, render target, or conversion.
/// @param extent `extent` value used by the operation.
/// @param extra_usage Usage flags or category applied to the resource.
/// @param samples `samples` value used by the operation.
/// @param mip_levels `mip_levels` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanAttachmentImage> VulkanAttachmentImage::create_color(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage,
            VkSampleCountFlagBits samples,
            u32 mip_levels) noexcept {
            ZoneScopedN("VulkanAttachmentImage::create_color");
            return create(device, allocator, VulkanAttachmentImageDesc{
                .format = format,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra_usage,
                .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
                .samples = samples,
                .mip_levels = mip_levels,
            });
        }

/// Returns the current or globally available image value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanImage &VulkanAttachmentImage::image() const noexcept { return image_; }

/// Returns the current or globally available view value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanImageView &VulkanAttachmentImage::view() const noexcept { return view_; }

/// Returns the current or globally available ref value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanAttachmentRef &VulkanAttachmentImage::ref() const noexcept { return ref_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanAttachmentImage::is_valid() const noexcept { return ref_.is_valid(); }

/// Sets the extent for this `Vulkan`.
///
/// @param extent `extent` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanRenderTarget &VulkanRenderTarget::set_extent(VkExtent2D extent) noexcept {
            ZoneScopedN("VulkanRenderTarget::set_extent");
            extent_ = extent;
            return *this;
        }

/// Sets the samples for this `Vulkan`.
///
/// @param samples `samples` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanRenderTarget &VulkanRenderTarget::set_samples(VkSampleCountFlagBits samples) noexcept {
            ZoneScopedN("VulkanRenderTarget::set_samples");
            samples_ = samples;
            return *this;
        }

/// Adds color using the supplied arguments and current state.
///
/// @param attachment `attachment` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
VulkanRenderTarget &VulkanRenderTarget::add_color(const VulkanAttachmentRef &attachment) {
            ZoneScopedN("VulkanRenderTarget::add_color");
            color_.push_back(attachment);
            if (extent_.width == 0 || extent_.height == 0) {
                extent_ = attachment.extent_2d();
            }
            samples_ = attachment.samples;
            return *this;
        }

/// Sets the colors for this `Vulkan`.
///
/// @param attachments `attachments` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
VulkanRenderTarget &VulkanRenderTarget::set_colors(span<const VulkanAttachmentRef> attachments) {
            ZoneScopedN("VulkanRenderTarget::set_colors");
            color_.assign(attachments.begin(), attachments.end());
            if (!color_.empty()) {
                extent_ = color_.front().extent_2d();
                samples_ = color_.front().samples;
            }
            return *this;
        }

/// Sets the depth stencil for this `Vulkan`.
///
/// @param attachment `attachment` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanRenderTarget &VulkanRenderTarget::set_depth_stencil(const VulkanAttachmentRef &attachment) noexcept {
            ZoneScopedN("VulkanRenderTarget::set_depth_stencil");
            depth_stencil_ = attachment;
            has_depth_stencil_ = attachment.is_valid();
            if (extent_.width == 0 || extent_.height == 0) {
                extent_ = attachment.extent_2d();
            }
            samples_ = attachment.samples;
            return *this;
        }

/// Returns the current or globally available colors value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const VulkanAttachmentRef> VulkanRenderTarget::colors() const noexcept { return color_; }

/// Returns the current or globally available depth stencil value.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanAttachmentRef *VulkanRenderTarget::depth_stencil() const noexcept {
            ZoneScopedN("VulkanRenderTarget::depth_stencil");
            return has_depth_stencil_ ? &depth_stencil_ : nullptr;
        }

/// Returns the current or globally available extent value.
///
/// @return Returns the current extent value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkExtent2D VulkanRenderTarget::extent() const noexcept { return extent_; }

/// Renders area using the current rendering state.
///
/// @return Returns the current render area value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRect2D VulkanRenderTarget::render_area() const noexcept {
            ZoneScopedN("VulkanRenderTarget::render_area");
            return VkRect2D{.offset = {.x = 0, .y = 0}, .extent = extent_};
        }

/// Returns the current or globally available samples value.
///
/// @return Returns the current samples value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSampleCountFlagBits VulkanRenderTarget::samples() const noexcept { return samples_; }

/// Reports whether this `Vulkan` has depth stencil.
///
/// @return Returns the current has depth stencil value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanRenderTarget::has_depth_stencil() const noexcept { return has_depth_stencil_; }

/// Reports whether this `Vulkan` contains no elements or payload.
///
/// @return Returns the current empty value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanRenderTarget::empty() const noexcept { return color_.empty() && !has_depth_stencil_; }

} // namespace SFT::Core::Vulkan

namespace SFT::Core::Vulkan {

    /// Creates a depth from the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param allocator Allocator used for storage owned by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param extent `extent` value used by the operation.
    /// @param extra_usage Usage flags or category applied to the resource.
    /// @param samples `samples` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    RendererExpected<VulkanAttachmentImage> VulkanAttachmentImage::create_depth(
        VkDevice device,
        VmaAllocator allocator,
        VkFormat format,
        VkExtent2D extent,
        VkImageUsageFlags extra_usage,
        VkSampleCountFlagBits samples) noexcept {
        return create(device, allocator, VulkanAttachmentImageDesc{
            .format = format,
            .extent = {.width = extent.width, .height = extent.height, .depth = 1},
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | extra_usage,
            .aspects = default_aspect_for_format(format),
            .samples = samples,
        });
    }

} // namespace SFT::Core::Vulkan

