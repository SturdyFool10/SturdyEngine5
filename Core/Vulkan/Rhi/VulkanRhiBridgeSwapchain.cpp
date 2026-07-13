// RHI surface/swapchain/presentation implementation for Vulkan.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#if defined(__linux__)
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <X11/Xlib.h>
#include <wayland-client.h>
#include <xcb/xcb.h>
#endif
#include "volk.h"
#if defined(__linux__)
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#endif
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanDevice;
import :VulkanImage;
import :VulkanPhysicalDevice;
import :VulkanQueue;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import :VulkanSwapchain;
import :VulkanSync;
import Sturdy.Foundation;
import Sturdy.RHI;

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        [[nodiscard]] constexpr VkPresentModeKHR present_mode_to_vk(rhi::PresentMode mode) noexcept {
            switch (mode) {
                case rhi::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
                case rhi::PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
                case rhi::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
                case rhi::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        [[nodiscard]] VkPresentModeKHR choose_present_mode(span<const VkPresentModeKHR> modes,
                                                           rhi::PresentMode requested) noexcept {
            const VkPresentModeKHR preferred = present_mode_to_vk(requested);
            if (std::ranges::contains(modes, preferred)) {
                return preferred;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(span<const VkSurfaceFormatKHR> formats,
                                                               rhi::Format requested) noexcept {
            const VkFormat preferred = SFT::Core::Vulkan::to_vk(requested);
            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    return format;
                }
            }
            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.format == preferred) {
                    return format;
                }
            }
            return formats.empty() ? VkSurfaceFormatKHR{.format = preferred, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                   : formats.front();
        }

        [[nodiscard]] u32 choose_image_count(const VkSurfaceCapabilitiesKHR &caps, u32 requested) noexcept {
            u32 count = requested == 0 ? caps.minImageCount + 1 : requested;
            count = std::max(count, caps.minImageCount);
            if (caps.maxImageCount > 0) {
                count = std::min(count, caps.maxImageCount);
            }
            return count;
        }

        [[nodiscard]] VkCompositeAlphaFlagBitsKHR composite_alpha_to_vk(rhi::CompositeAlphaMode mode) noexcept {
            switch (mode) {
                case rhi::CompositeAlphaMode::Opaque: return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                case rhi::CompositeAlphaMode::Premultiplied: return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
                case rhi::CompositeAlphaMode::PostMultiplied: return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
                case rhi::CompositeAlphaMode::Inherit: return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
                case rhi::CompositeAlphaMode::Auto: return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }

        [[nodiscard]] VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkCompositeAlphaFlagsKHR supported,
                                                                         rhi::CompositeAlphaMode requested) noexcept {
            const VkCompositeAlphaFlagBitsKHR preferred = composite_alpha_to_vk(requested);
            if ((supported & preferred) != 0) {
                return preferred;
            }
            constexpr VkCompositeAlphaFlagBitsKHR choices[] = {
                VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            };
            for (VkCompositeAlphaFlagBitsKHR choice : choices) {
                if ((supported & choice) != 0) {
                    return choice;
                }
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }

    } // namespace

    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::create_surface(const rhi::SurfaceDesc &desc) {
        if (instance_ == VK_NULL_HANDLE) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run create_surface: instance resources are not ready.");
        }

        VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(__linux__)
        switch (desc.system) {
            case rhi::WindowSystem::Xlib: {
                const auto create_xlib_surface = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateXlibSurfaceKHR"));
                if (create_xlib_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_xlib_surface is not enabled or loaded.");
                }
                const VkXlibSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                    .dpy = static_cast<Display *>(desc.display),
                    .window = static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(desc.window)),
                };
                if (create_xlib_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateXlibSurfaceKHR failed.");
                }
                break;
            }
            case rhi::WindowSystem::Xcb: {
                const auto create_xcb_surface = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateXcbSurfaceKHR"));
                if (create_xcb_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_xcb_surface is not enabled or loaded.");
                }
                const VkXcbSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                    .connection = static_cast<xcb_connection_t *>(desc.display),
                    .window = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(desc.window)),
                };
                if (create_xcb_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateXcbSurfaceKHR failed.");
                }
                break;
            }
            case rhi::WindowSystem::Wayland: {
                const auto create_wayland_surface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
                if (create_wayland_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_wayland_surface is not enabled or loaded.");
                }
                const VkWaylandSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                    .display = static_cast<wl_display *>(desc.display),
                    .surface = static_cast<wl_surface *>(desc.window),
                };
                if (create_wayland_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateWaylandSurfaceKHR failed.");
                }
                break;
            }
            default:
                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "create_surface: this WindowSystem is not supported by the Linux Vulkan bridge.");
        }
#else
        (void)desc;
        return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                              "create_surface: raw-native Vulkan RHI surfaces are not compiled into this platform build.");
#endif

        return surfaces_.insert(SurfaceRecord{.surface = surface, .owns_surface = true});
    }

    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::import_surface(VkSurfaceKHR surface) {
        if (surface == VK_NULL_HANDLE) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "import_surface: cannot import a null VkSurfaceKHR.");
        }
        return surfaces_.insert(SurfaceRecord{.surface = surface, .owns_surface = false});
    }

    void VulkanRhiDeviceBridge::destroy_surface(rhi::SurfaceHandle handle) noexcept {
        SurfaceRecord *record = surfaces_.find(handle);
        if (record != nullptr && record->surface != VK_NULL_HANDLE && record->owns_surface) {
            vkDestroySurfaceKHR(instance_, record->surface, nullptr);
        }
        surfaces_.erase(handle);
    }

    rhi::RhiExpected<rhi::SwapchainHandle> VulkanRhiDeviceBridge::create_swapchain(const rhi::SwapchainDesc &desc) {
        if (logical_device_ == nullptr || physical_device_ == nullptr) {
            return device_not_ready<rhi::SwapchainHandle>("create_swapchain");
        }
        SurfaceRecord *surface = surfaces_.find(desc.surface);
        if (surface == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_swapchain: unknown surface handle.");
        }

        SwapchainRecord *old_record = nullptr;
        if (desc.old_swapchain.is_valid()) {
            old_record = swapchains_.find(desc.old_swapchain);
            if (old_record == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_swapchain: old_swapchain is not a live swapchain handle.");
            }
            if (old_record->surface != desc.surface) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_swapchain: old_swapchain belongs to a different surface.");
            }
        }

        auto caps = physical_device_->surface_capabilities(surface->surface);
        if (!caps) {
            return rhi_error_from_graphics(caps.error());
        }
        auto formats = physical_device_->surface_formats(surface->surface);
        if (!formats) {
            return rhi_error_from_graphics(formats.error());
        }
        auto modes = physical_device_->surface_present_modes(surface->surface);
        if (!modes) {
            return rhi_error_from_graphics(modes.error());
        }

        const VkSurfaceFormatKHR format = choose_surface_format(*formats, desc.format);
        const VkExtent2D extent{
            .width = desc.width != 0 ? desc.width : caps->currentExtent.width,
            .height = desc.height != 0 ? desc.height : caps->currentExtent.height,
        };
        VkImageUsageFlags usage = to_vk(desc.usage);
        if (usage == 0) {
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        usage &= caps->supportedUsageFlags;
        if (usage == 0) {
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }

        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface->surface,
            .minImageCount = choose_image_count(*caps, desc.image_count),
            .imageFormat = format.format,
            .imageColorSpace = format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = usage,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps->currentTransform,
            .compositeAlpha = choose_composite_alpha(caps->supportedCompositeAlpha, desc.composite_alpha),
            .presentMode = choose_present_mode(*modes, desc.present_mode),
            .clipped = desc.clipped ? VK_TRUE : VK_FALSE,
            .oldSwapchain = old_record != nullptr ? old_record->swapchain.vk_handle() : VK_NULL_HANDLE,
        };

        auto swapchain = VulkanSwapchain::create(logical_device_->vk_handle(), info);
        if (!swapchain) {
            return rhi_error_from_graphics(swapchain.error());
        }

        SwapchainRecord record{};
        record.swapchain = std::move(*swapchain);
        record.surface = desc.surface;
        record.textures.reserve(record.swapchain.image_count());
        record.views.reserve(record.swapchain.image_count());
        record.image_available_semaphores.reserve(record.swapchain.image_count());
        record.render_finished_semaphores.reserve(record.swapchain.image_count());
        record.image_available_signal_indices.resize(record.swapchain.image_count(), 0);

        for (VkImage image : record.swapchain.images()) {
            VulkanImage borrowed = VulkanImage::borrow(logical_device_->vk_handle(), image, record.swapchain.format(),
                                                       VkExtent3D{extent.width, extent.height, 1}, usage);
            rhi::TextureHandle texture = textures_.insert(TextureRecord{std::move(borrowed), desc.format});
            record.textures.push_back(texture);

            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = record.swapchain.format(),
                .components = {
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            auto view = VulkanImageView::create(logical_device_->vk_handle(), view_info);
            if (!view) {
                return rhi_error_from_graphics(view.error());
            }
            record.views.push_back(texture_views_.insert(std::move(*view)));

            auto image_available = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!image_available) {
                return rhi_error_from_graphics(image_available.error());
            }
            record.image_available_semaphores.push_back(std::move(*image_available));

            auto render_finished = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!render_finished) {
                return rhi_error_from_graphics(render_finished.error());
            }
            record.render_finished_semaphores.push_back(std::move(*render_finished));
        }

        return swapchains_.insert(std::move(record));
    }

    void VulkanRhiDeviceBridge::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        SwapchainRecord *record = swapchains_.find(handle);
        if (record != nullptr) {
            for (rhi::TextureViewHandle view : record->views) {
                texture_views_.erase(view);
            }
            for (rhi::TextureHandle texture : record->textures) {
                textures_.erase(texture);
            }
        }
        swapchains_.erase(handle);
    }

    rhi::RhiExpected<rhi::SurfaceTexture> VulkanRhiDeviceBridge::acquire_next_texture(rhi::SwapchainHandle handle) {
        SwapchainRecord *record = swapchains_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "acquire_next_texture: unknown swapchain handle.");
        }
        if (record->image_available_semaphores.empty()) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "acquire_next_texture: swapchain has no image-available semaphores.");
        }

        const u32 semaphore_index = record->acquire_cursor++ % static_cast<u32>(record->image_available_semaphores.size());
        VkAcquireNextImageInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
            .swapchain = record->swapchain.vk_handle(),
            .timeout = UINT64_MAX,
            .semaphore = record->image_available_semaphores[semaphore_index].vk_handle(),
            .fence = VK_NULL_HANDLE,
            .deviceMask = 1,
        };
        u32 image_index = 0;
        const VkResult result = vkAcquireNextImage2KHR(logical_device_->vk_handle(), &info, &image_index);
        if (result == VK_ERROR_DEVICE_LOST) {
            return rhi::rhi_error(rhi::RhiErrorCode::DeviceLost, "acquire_next_texture: vkAcquireNextImage2KHR reported device loss.");
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
            return rhi::rhi_error(rhi::RhiErrorCode::SurfaceLost, "acquire_next_texture: swapchain surface is out of date or lost.");
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "acquire_next_texture: vkAcquireNextImage2KHR failed.");
        }

        record->image_available_signal_indices[image_index] = semaphore_index;
        record->current_image = image_index;
        record->current_suboptimal = result == VK_SUBOPTIMAL_KHR;
        return rhi::SurfaceTexture{
            .swapchain = handle,
            .texture = record->textures[image_index],
            .view = record->views[image_index],
            .image_index = image_index,
            .suboptimal = record->current_suboptimal,
        };
    }

    rhi::RhiExpected<bool> VulkanRhiDeviceBridge::present(const rhi::PresentDesc &desc) {
        if (graphics_queue_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run present: device resources are not ready.");
        }
        SwapchainRecord *record = swapchains_.find(desc.texture.swapchain);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "present: unknown swapchain handle.");
        }
        if (desc.texture.image_index >= record->render_finished_semaphores.size()) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "present: image index is out of range.");
        }

        const VkSwapchainKHR swapchain = record->swapchain.vk_handle();
        const u32 image_index = desc.texture.image_index;
        const VkSemaphore wait = record->render_finished_semaphores[image_index].vk_handle();
        const VkPresentInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        auto result = graphics_queue_->present(info);
        if (!result) {
            return rhi_error_from_graphics(result.error());
        }
        return *result || desc.texture.suboptimal;
    }

} // namespace SFT::Core::Vulkan
