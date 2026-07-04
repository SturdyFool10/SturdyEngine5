// VulkanBackend swapchain construction and teardown: image count/present-mode negotiation,
// per-image views and render-finished semaphores, and the depth attachment.
module;
#include "glm/ext/vector_float2.hpp"
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

#include <algorithm>
#include <format>
#include <vector>

module Sturdy.Core;

import :VulkanAllocator;
import :VulkanBackend;
import :VulkanConstants;
import :VulkanImage;
import :VulkanPhysicalDevice;
import :VulkanSurface;
import :VulkanSwapchain;
import :VulkanSync;
import :RendererError;
import :Renderer;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::format;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace {

        [[nodiscard]] VkPresentModeKHR choose_present_mode(const vector<VkPresentModeKHR> &available_modes) noexcept {
            constexpr VkPresentModeKHR preferred_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            return std::ranges::find(available_modes, preferred_mode) != available_modes.end()
                       ? preferred_mode
                       : VK_PRESENT_MODE_FIFO_KHR;
        }

    } // namespace

    void VulkanBackend::destroySwapchain(VulkanSurface &surface) noexcept {
        surface.swapchain().destroy();
        surface.mark_dirty();
    }

    RendererResult VulkanBackend::createSwapchain(const RendererCreateInfo &init, VulkanSurface &surface) {
        (void)init;

        // Swapchain extent must match the surface's pixel dimensions, not the window's
        // logical/point size — these differ under HiDPI scaling.
        auto windowExtentResult = surface.window()->framebuffer_size();
        if (!windowExtentResult.has_value()) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  format("Failed to query framebuffer size for swapchain creation: {}", windowExtentResult.error().message));
        }
        auto winSize = windowExtentResult.value();

        // request the apropriate number of images
        auto surface_caps_result = this->physicalDevice.surface_capabilities(surface.vk_handle());
        if (!surface_caps_result.has_value()) [[unlikely]] {
            return renderer_error(surface_caps_result.error().code,
                                  format("Failed to query surface capabilities: {}", surface_caps_result.error().message));
        }
        const VkSurfaceCapabilitiesKHR &surfaceCaps = *surface_caps_result;
        u32 requestedImageCount = sanitize_frames_in_flight(std::max(2u, surfaceCaps.minImageCount));
        if (surfaceCaps.maxImageCount > 0) [[likely]] {
            requestedImageCount = std::min(requestedImageCount, surfaceCaps.maxImageCount);
        }

        auto present_modes_result = this->physicalDevice.surface_present_modes(surface.vk_handle());
        if (!present_modes_result.has_value()) [[unlikely]] {
            return renderer_error(present_modes_result.error().code,
                                  format("Failed to query surface present modes: {}", present_modes_result.error().message));
        }
        const VkPresentModeKHR present_mode = choose_present_mode(*present_modes_result);

        // Hand the retiring swapchain to the driver as oldSwapchain so it can transition that
        // swapchain's presentation backing into the new one instead of committing a fresh set of
        // images every resize — recreating from scratch grows the process's resident memory each
        // time. VK_NULL_HANDLE on the first build. The old handle is only destroyed further down,
        // when set_swapchain() replaces it, so it stays valid across vkCreateSwapchainKHR here.
        const VkSwapchainKHR old_swapchain = surface.swapchain().vk_handle();

        VkSwapchainCreateInfoKHR swapchainCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface.vk_handle(),
            .minImageCount = requestedImageCount,
            .imageFormat = SWAPCHAIN_FORMAT,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent{.width = winSize.x, .height = winSize.y},
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = surfaceCaps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = present_mode,
            .oldSwapchain = old_swapchain};

        auto swapchain_result = VulkanSwapchain::create(this->logicalDevice.vk_handle(), swapchainCreateInfo);
        if (!swapchain_result.has_value()) [[unlikely]] {
            return renderer_error(swapchain_result.error().code,
                                  format("Failed to create swapchain: {}", swapchain_result.error().message));
        }

        surface.set_swapchain(std::move(*swapchain_result));
        surface.clear_dirty();

        // One VkImageView per swapchain image — dynamic rendering attachments and any future
        // framebuffer-less render pass need a view, not the raw VkImage.
        vector<VulkanImageView> image_views;
        image_views.reserve(surface.swapchain().image_count());
        for (VkImage image : surface.swapchain().images()) {
            VkImageViewCreateInfo viewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = surface.swapchain().format(),
                .components = {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };

            auto view_result = VulkanImageView::create(this->logicalDevice.vk_handle(), viewCreateInfo);
            if (!view_result.has_value()) [[unlikely]] {
                return renderer_error(view_result.error().code,
                                      format("Failed to create swapchain image view: {}", view_result.error().message));
            }
            image_views.push_back(std::move(*view_result));
        }

        surface.swapchain().set_image_views(std::move(image_views));

        // One render-finished semaphore per swapchain image, signaled by the submit that
        // renders into that image and waited on by the present that follows it.
        vector<VulkanSemaphore> render_finished_semaphores;
        render_finished_semaphores.reserve(surface.swapchain().image_count());
        for (u32 i = 0; i < surface.swapchain().image_count(); ++i) {
            auto semaphore_result = VulkanSemaphore::create_binary(this->logicalDevice.vk_handle());
            if (!semaphore_result.has_value()) [[unlikely]] {
                return renderer_error(semaphore_result.error().code,
                                      format("Failed to create render-finished semaphore: {}", semaphore_result.error().message));
            }
            render_finished_semaphores.push_back(std::move(*semaphore_result));
        }

        surface.swapchain().set_render_finished_semaphores(std::move(render_finished_semaphores));

        VkImageCreateInfo depthCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = DEPTH_FORMAT,
            .extent = {.width = winSize.x, .height = winSize.y, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo depthAllocationInfo{
            .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        auto depth_image_result = this->vmaAllocator.create_image(
            this->logicalDevice.vk_handle(),
            depthCreateInfo,
            depthAllocationInfo);
        if (!depth_image_result.has_value()) [[unlikely]] {
            return renderer_error(depth_image_result.error().code,
                                  format("Failed to create depth image: {}", depth_image_result.error().message));
        }

        auto depth_image = std::move(*depth_image_result);
        auto depth_view_result = depth_image.create_view(VK_IMAGE_ASPECT_DEPTH_BIT,
                                                         VK_IMAGE_VIEW_TYPE_2D,
                                                         0,
                                                         1,
                                                         0,
                                                         1);
        if (!depth_view_result.has_value()) [[unlikely]] {
            return renderer_error(depth_view_result.error().code,
                                  format("Failed to create depth image view: {}", depth_view_result.error().message));
        }

        surface.swapchain().set_depth_attachment(std::move(depth_image), std::move(*depth_view_result));

        return {};
    }

} // namespace SFT::Core::Vulkan
