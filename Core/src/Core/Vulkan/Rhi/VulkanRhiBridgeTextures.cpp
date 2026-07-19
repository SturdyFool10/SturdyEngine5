// RhiDevice texture + texture-view resource creation/destruction.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <utility>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::TextureHandle> VulkanRhiDeviceBridge::create_texture(const rhi::TextureDesc &desc) {
        if (allocator_ == nullptr || logical_device_ == nullptr) {
            return device_not_ready<rhi::TextureHandle>("create_texture");
        }

        VkExtent3D extent{desc.extent.width, desc.extent.height, 1};
        u32 array_layers = 1;
        if (desc.dimension == rhi::TextureDimension::Dim3D) {
            extent.depth = desc.extent.depth_or_layers;
        } else {
            array_layers = desc.extent.depth_or_layers;
        }

        // The RHI carries no explicit "cube" or "3D-sliceable" bit on TextureDesc, so infer the
        // image-creation flags that let create_texture_view later expose the view types the RHI
        // advertises (ViewCube/ViewCubeArray, and 2D/2DArray slices of a 3D texture). Both flags are
        // strictly capability-additive — a flagged image still supports every view a plain one does —
        // and each inference exactly matches its Vulkan validity rule, so setting it is never wrong:
        //   • CUBE_COMPATIBLE requires a 2D, square image with a multiple-of-6 (>= 6) layer count.
        //   • 2D_ARRAY_COMPATIBLE requires a 3D image.
        // Without this, creating a cubemap (shadow cubes, IBL/environment maps) or rendering into an
        // individual slice of a volume texture was impossible through the RHI.
        VkImageCreateFlags create_flags = 0;
        if (desc.dimension == rhi::TextureDimension::Dim2D && array_layers >= 6 && array_layers % 6 == 0 &&
            extent.width == extent.height) {
            create_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        if (desc.dimension == rhi::TextureDimension::Dim3D) {
            create_flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        }

        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = create_flags,
            .imageType = to_vk(desc.dimension),
            .format = to_vk(desc.format),
            .extent = extent,
            .mipLevels = desc.mip_levels,
            .arrayLayers = array_layers,
            .samples = to_vk(desc.samples),
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = to_vk(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo alloc_info{
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        auto image = allocator_->create_image(logical_device_->vk_handle(), image_info, alloc_info);
        if (!image) {
            return rhi_error_from_graphics(image.error());
        }

        return textures_.insert(TextureRecord{std::move(*image), desc.format});
    }

    void VulkanRhiDeviceBridge::destroy_texture(rhi::TextureHandle handle) noexcept {
        textures_.erase(handle);
    }

    rhi::RhiExpected<rhi::TextureViewHandle> VulkanRhiDeviceBridge::create_texture_view(const rhi::TextureViewDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::TextureViewHandle>("create_texture_view");
        }

        TextureRecord *record = textures_.find(desc.texture);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_texture_view: unknown texture handle.");
        }

        const rhi::Format view_format = desc.format == rhi::Format::Undefined ? record->format : desc.format;
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = record->image.vk_handle(),
            .viewType = to_vk(desc.view_type),
            .format = to_vk(view_format),
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = aspect_for_format(view_format),
                .baseMipLevel = desc.base_mip_level,
                .levelCount = desc.mip_level_count,
                .baseArrayLayer = desc.base_array_layer,
                .layerCount = desc.array_layer_count,
            },
        };

        auto view = VulkanImageView::create(logical_device_->vk_handle(), view_info);
        if (!view) {
            return rhi_error_from_graphics(view.error());
        }

        return texture_views_.insert(std::move(*view));
    }

    void VulkanRhiDeviceBridge::destroy_texture_view(rhi::TextureViewHandle handle) noexcept {
        texture_views_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
