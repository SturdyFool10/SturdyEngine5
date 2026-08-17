#pragma once

#include <Foundation/src/Foundation.hpp>
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

#include <graphicsPlatform/src/CompositionPresent.hpp>

using SFT::Core::RendererExpected;

namespace SFT::Core::Vulkan {










    /// One composition-presenter shared image, imported as a real (owned) VkImage this bridge's
    /// texture pool can hold exactly like a native swapchain image.
    struct CompositionSwapchainImage {
        VulkanImage image;
        /// The VkDeviceMemory bound to `image`, owned here rather than by VulkanImage (which never
        /// owns memory it didn't allocate itself — see VulkanImage::bind_memory's doc comment).
        /// destroy_composition_swapchain_resources() frees it after destroying `image`, the safe order
        /// for memory that's still bound at teardown time.
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    /// Everything a SwapchainRecord needs to present through the OS compositor instead of
    /// vkQueuePresentKHR — the presenter itself, this swapchain's imported render targets, and the two
    /// imported timeline semaphores that hand image ownership back and forth across the API boundary
    /// (see GraphicsPlatform::CompositionSharedFences's own doc comment for which direction each one
    /// carries). `render_complete_value` belongs beside its semaphore: resize-in-place transfers both
    /// to the replacement generation, so its next signal remains strictly greater than the old value.
    struct CompositionSwapchainResources {
        std::unique_ptr<GraphicsPlatform::CompositionPresenter> presenter;
        std::vector<CompositionSwapchainImage> images;
        std::vector<VulkanImageView> views;
        VulkanSemaphore render_complete_semaphore;
        VulkanSemaphore present_complete_semaphore;
        u64 render_complete_value = 0;
    };

    /// Attempts to build the composition-present path for one swapchain surface — on Windows, the
    /// caller (VulkanRhiBridgeSwapchain.cpp) always tries this first, for every swapchain, transparent
    /// or opaque, rather than only as a fallback for a surface whose composite alpha can't do
    /// transparency: presenting exclusively through vkQueuePresentKHR on some frames and through
    /// DirectComposition on others left a stale legacy-presented frame visible underneath the live
    /// composition content whenever a window switched from one to the other mid-session (the two
    /// don't compose with each other the way switching implies) — always using this path sidesteps
    /// that class of bug entirely rather than papering over one instance of it. `vk_format` must be one
    /// of the handful of formats GraphicsPlatform::CompositionFormat covers (see
    /// vk_format_to_composition_format in the .cpp); anything else — same as any other platform/driver
    /// combination that can't do this — fails rather than silently substituting a close format, since a
    /// silently-wrong format here reproduces exactly the "looks fine, renders wrong" failure mode this
    /// path exists to avoid.
    ///
    /// Returns an error (never a partially-usable result) on any platform or configuration that can't
    /// support this, so the caller can fall back to the native vkQueuePresentKHR path exactly as it
    /// already does when this isn't attempted at all.
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

    /// Rebuilds an existing composition-present swapchain's images in place at a new size, reusing
    /// `previous`'s presenter (and, transitively, its D3D device, DXGI factory, and — critically — its
    /// already-attached DirectComposition target and visual) instead of tearing all of that down and
    /// building it fresh the way create_composition_swapchain_resources() above does. Format and alpha
    /// mode can't change through this path (CompositionPresenter::resize only takes a new width/height —
    /// both are otherwise fixed at DXGI swap-chain creation time); callers must fall back to
    /// create_composition_swapchain_resources() whenever either needs to change.
    ///
    /// This exists because a plain resize is by far the most common reason a composition-present
    /// swapchain gets rebuilt (live window resizing fires it continuously), and paying full D3D device
    /// creation plus a DirectComposition target detach/reattach on every single resize event — which is
    /// what happens if this call is skipped and create_composition_swapchain_resources() is used
    /// unconditionally instead — is both needlessly slow and visibly disruptive: detaching and
    /// reattaching the compositor target is exactly what produces a visible flicker during live resize.
    ///
    /// `previous` is consumed: its presenter, already-imported timeline semaphores, and the current
    /// render-complete value are moved into the result (resize() leaves the underlying D3D fences
    /// untouched, so re-importing them would only reproduce the same semaphores at extra cost). Before
    /// invalidating the old shared images, the implementation waits for that render-complete value — the
    /// exact Vulkan lifetime proof for this presenter — rather than idling unrelated device work. Its
    /// images/views are deliberately NOT moved — resize() invalidates every previously-returned shared
    /// image handle — so the caller's *own* copy of `previous` (the object this was called on, e.g. an
    /// old SwapchainRecord's `composition` field) still correctly holds everything
    /// destroy_composition_swapchain_resources() needs to free through the normal retirement path.
    [[nodiscard]] RendererExpected<CompositionSwapchainResources> resize_composition_swapchain_resources(
        VkDevice device,
        VkPhysicalDevice physical_device,
        CompositionSwapchainResources &&previous,
        VkFormat vk_format,
        VkImageUsageFlags usage,
        u32 width,
        u32 height);

    /// Releases every Vulkan-side resource `resources` owns, in the order that's actually safe to use
    /// once the caller has already proven no in-flight GPU submission still references any of it (the
    /// same invariant destroy_swapchain() already relies on for a native swapchain's images — see its
    /// own call site for why that's already guaranteed by the time this runs). Also destroys the
    /// presenter itself last, whose own destructor drains whatever the compositor still has queued
    /// against the shared D3D-side handles.
    void destroy_composition_swapchain_resources(VkDevice device, CompositionSwapchainResources &resources) noexcept;

} // namespace SFT::Core::Vulkan
