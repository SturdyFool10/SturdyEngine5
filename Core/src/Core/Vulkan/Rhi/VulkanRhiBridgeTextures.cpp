
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
#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    /// Creates a texture from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::TextureHandle> VulkanRhiDeviceBridge::create_texture(const rhi::TextureDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_texture");
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


        VkImageCreateFlags create_flags = 0;
        if (desc.dimension == rhi::TextureDimension::Dim2D && array_layers >= 6 && array_layers % 6 == 0 &&
            extent.width == extent.height) {
            create_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        if (desc.dimension == rhi::TextureDimension::Dim3D) {
            create_flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        }


        vector<u32> concurrent_families;
        for (const rhi::QueueClass queue_class : desc.concurrent_queue_classes) {
            const u32 family = queue_family_for_lane(rhi::QueueLane{queue_class, 0});
            if (family != VK_QUEUE_FAMILY_IGNORED &&
                std::find(concurrent_families.begin(), concurrent_families.end(), family) == concurrent_families.end()) {
                concurrent_families.push_back(family);
            }
        }
        const bool use_concurrent = concurrent_families.size() > 1;

        VkImageCreateInfo image_info{
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
            .sharingMode = use_concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (use_concurrent) {
            image_info.queueFamilyIndexCount = static_cast<u32>(concurrent_families.size());
            image_info.pQueueFamilyIndices = concurrent_families.data();
        }
        const bool transient_attachment = rhi::has_any(desc.usage, rhi::TextureUsage::TransientAttachment);
        const VmaAllocationCreateInfo alloc_info{
            .usage = transient_attachment ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO,


            .preferredFlags = transient_attachment ? VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT : 0u,
        };

        auto image = allocator_->create_image(logical_device_->vk_handle(), image_info, alloc_info);
        if (!image) {
            return rhi_error_from_graphics(image.error());
        }

        return textures_.insert(TextureRecord{std::move(*image), desc.format});
    }

    /// Destroys the texture identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_texture(rhi::TextureHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_texture");
        textures_.erase(handle);
    }

    /// Creates a texture view from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::TextureViewHandle> VulkanRhiDeviceBridge::create_texture_view(const rhi::TextureViewDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_texture_view");
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

    /// Destroys the texture view identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_texture_view(rhi::TextureViewHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_texture_view");
        texture_views_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
