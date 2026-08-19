#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <memory>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanSync.hpp>

#include <Core/GraphicsPlatform/CompositionPresent.hpp>

using SFT::Core::RendererExpected;

namespace SFT::Core::Vulkan {


    struct CompositionSwapchainImage {
        VulkanImage image;


        VkDeviceMemory memory = VK_NULL_HANDLE;
    };


    struct CompositionSwapchainResources {
        std::unique_ptr<GraphicsPlatform::CompositionPresenter> presenter;
        std::vector<CompositionSwapchainImage> images;
        std::vector<VulkanImageView> views;
        VulkanSemaphore render_complete_semaphore;
        VulkanSemaphore present_complete_semaphore;
        u64 render_complete_value = 0;
    };


    /// Creates a composition swapchain resources from the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param physical_device Device used or affected by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param vk_format Format used for the resource, render target, or conversion.
    /// @param usage Usage flags or category applied to the resource.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param image_count Number of elements or operations to process.
    /// @param alpha_mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] RendererExpected<CompositionSwapchainResources> create_composition_swapchain_resources(
        VkDevice device,
        VkPhysicalDevice physical_device,
        const GraphicsPlatform::NativeSurfaceHandle &surface,
        VkFormat vk_format,
        VkImageUsageFlags usage,
        u32 width,
        u32 height,
        u32 image_count,
        GraphicsPlatform::CompositionAlphaMode alpha_mode);


    /// Changes the logical size to the requested value, creating or removing elements as needed.
    ///
    /// @param device Device used or affected by the operation.
    /// @param physical_device Device used or affected by the operation.
    /// @param previous `previous` value used by the operation.
    /// @param vk_format Format used for the resource, render target, or conversion.
    /// @param usage Usage flags or category applied to the resource.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] RendererExpected<CompositionSwapchainResources> resize_composition_swapchain_resources(
        VkDevice device,
        VkPhysicalDevice physical_device,
        CompositionSwapchainResources &&previous,
        VkFormat vk_format,
        VkImageUsageFlags usage,
        u32 width,
        u32 height);


    /// Destroys the composition swapchain resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_composition_swapchain_resources(VkDevice device, CompositionSwapchainResources &resources) noexcept;

} // namespace SFT::Core::Vulkan
