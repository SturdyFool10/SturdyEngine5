#include "VulkanRenderTargets.hpp"

namespace SFT::Core::Vulkan {

[[nodiscard]] VkImageViewCreateInfo VulkanImageViewDesc::to_vk(VkImage image) const noexcept {
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

[[nodiscard]] bool VulkanAttachmentRef::is_valid() const noexcept {
            return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
        }

[[nodiscard]] VkExtent2D VulkanAttachmentRef::extent_2d() const noexcept {
            return VkExtent2D{.width = extent.width, .height = extent.height};
        }

[[nodiscard]] RendererExpected<VulkanAttachmentImage> VulkanAttachmentImage::create(
            VkDevice device,
            VmaAllocator allocator,
            const VulkanAttachmentImageDesc &desc) noexcept {
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

[[nodiscard]] RendererExpected<VulkanAttachmentImage> VulkanAttachmentImage::create_color(
            VkDevice device,
            VmaAllocator allocator,
            VkFormat format,
            VkExtent2D extent,
            VkImageUsageFlags extra_usage,
            VkSampleCountFlagBits samples,
            u32 mip_levels) noexcept {
            return create(device, allocator, VulkanAttachmentImageDesc{
                .format = format,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra_usage,
                .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
                .samples = samples,
                .mip_levels = mip_levels,
            });
        }

[[nodiscard]] const VulkanImage &VulkanAttachmentImage::image() const noexcept { return image_; }

[[nodiscard]] const VulkanImageView &VulkanAttachmentImage::view() const noexcept { return view_; }

[[nodiscard]] const VulkanAttachmentRef &VulkanAttachmentImage::ref() const noexcept { return ref_; }

[[nodiscard]] bool VulkanAttachmentImage::is_valid() const noexcept { return ref_.is_valid(); }

VulkanRenderTarget &VulkanRenderTarget::set_extent(VkExtent2D extent) noexcept {
            extent_ = extent;
            return *this;
        }

VulkanRenderTarget &VulkanRenderTarget::set_samples(VkSampleCountFlagBits samples) noexcept {
            samples_ = samples;
            return *this;
        }

VulkanRenderTarget &VulkanRenderTarget::add_color(const VulkanAttachmentRef &attachment) {
            color_.push_back(attachment);
            if (extent_.width == 0 || extent_.height == 0) {
                extent_ = attachment.extent_2d();
            }
            samples_ = attachment.samples;
            return *this;
        }

VulkanRenderTarget &VulkanRenderTarget::set_colors(span<const VulkanAttachmentRef> attachments) {
            color_.assign(attachments.begin(), attachments.end());
            if (!color_.empty()) {
                extent_ = color_.front().extent_2d();
                samples_ = color_.front().samples;
            }
            return *this;
        }

VulkanRenderTarget &VulkanRenderTarget::set_depth_stencil(const VulkanAttachmentRef &attachment) noexcept {
            depth_stencil_ = attachment;
            has_depth_stencil_ = attachment.is_valid();
            if (extent_.width == 0 || extent_.height == 0) {
                extent_ = attachment.extent_2d();
            }
            samples_ = attachment.samples;
            return *this;
        }

[[nodiscard]] span<const VulkanAttachmentRef> VulkanRenderTarget::colors() const noexcept { return color_; }

[[nodiscard]] const VulkanAttachmentRef *VulkanRenderTarget::depth_stencil() const noexcept {
            return has_depth_stencil_ ? &depth_stencil_ : nullptr;
        }

[[nodiscard]] VkExtent2D VulkanRenderTarget::extent() const noexcept { return extent_; }

[[nodiscard]] VkRect2D VulkanRenderTarget::render_area() const noexcept {
            return VkRect2D{.offset = {.x = 0, .y = 0}, .extent = extent_};
        }

[[nodiscard]] VkSampleCountFlagBits VulkanRenderTarget::samples() const noexcept { return samples_; }

[[nodiscard]] bool VulkanRenderTarget::has_depth_stencil() const noexcept { return has_depth_stencil_; }

[[nodiscard]] bool VulkanRenderTarget::empty() const noexcept { return color_.empty() && !has_depth_stencil_; }

} // namespace SFT::Core::Vulkan
