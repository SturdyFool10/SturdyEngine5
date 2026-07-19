#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanSync.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

namespace SFT::Core::Vulkan {

    // Owns a VkSwapchainKHR and caches the swapchain images (non-owning — the
    // driver owns swapchain images). Destroyed via destroy() or destructor.
    class VulkanSwapchain {
      public:
        VulkanSwapchain() = default;
        ~VulkanSwapchain();

        VulkanSwapchain(const VulkanSwapchain &) = delete;
        VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

        VulkanSwapchain(VulkanSwapchain &&o) noexcept;

        VulkanSwapchain &operator=(VulkanSwapchain &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanSwapchain> create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept;

        [[nodiscard]] VkSwapchainKHR vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] VkColorSpaceKHR color_space() const noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkPresentModeKHR present_mode() const noexcept;
        [[nodiscard]] u32 image_count() const noexcept;
        [[nodiscard]] const vector<VkImage> &images() const noexcept;
        [[nodiscard]] VkImage image(u32 i) const noexcept;

        // Populated separately from create(), once per swapchain build — one view per
        // swapchain image, in the same order as images(). Replacing the views (e.g. after a
        // swapchain rebuild) destroys whatever views were set previously.
        void set_image_views(vector<VulkanImageView> views) noexcept;
        [[nodiscard]] const vector<VulkanImageView> &image_views() const noexcept;
        [[nodiscard]] VkImageView image_view(u32 i) const noexcept;

        void set_depth_attachment(VulkanImage image, VulkanImageView view) noexcept;
        [[nodiscard]] const VulkanImage &depth_image() const noexcept;
        [[nodiscard]] const VulkanImageView &depth_image_view() const noexcept;
        [[nodiscard]] VkImageView depth_image_view_handle() const noexcept;

        // One binary semaphore per swapchain image, signaled when that image's rendering work
        // is done and it's safe to present. Keyed by swapchain image index (the index returned
        // by acquire_next_image), not by frame-in-flight index — reusing a single semaphore
        // across frames risks a present/wait-twice hazard if images are acquired out of order.
        void set_render_finished_semaphores(vector<VulkanSemaphore> semaphores) noexcept;
        [[nodiscard]] const vector<VulkanSemaphore> &render_finished_semaphores() const noexcept;
        [[nodiscard]] VkSemaphore render_finished_semaphore(u32 image_index) const noexcept;

        // Bundles the color+depth VkRenderingAttachmentInfo for one dynamic-rendering pass. Kept
        // together (rather than returned separately) since rendering_info() hands out pointers into
        // this object — callers must keep it alive across the vkCmdBeginRendering() call it feeds.
        struct RenderingAttachments {
            VkRenderingAttachmentInfo color{};
            VkRenderingAttachmentInfo depth{};

            [[nodiscard]] VkRenderingInfo rendering_info(VkRect2D render_area) const noexcept;
        };

        // Builds the color+depth attachment infos for image_index, clearing both on load.
        [[nodiscard]] RenderingAttachments rendering_attachments(
            u32 image_index,
            VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}},
            VkClearDepthStencilValue clear_depth = {1.0f, 0}) const noexcept;

        // Transitions the color image at image_index and the shared depth image from UNDEFINED to
        // their respective attachment-optimal layouts, ready for dynamic rendering to write into.
        [[nodiscard]] vector<VkImageMemoryBarrier2> undefined_to_attachment_barriers(u32 image_index) const noexcept;

        // Transitions the color image at image_index from attachment-optimal to PRESENT_SRC once
        // rendering into it has finished, ready to be handed to vkQueuePresentKHR.
        [[nodiscard]] vector<VkImageMemoryBarrier2> attachment_to_present_barrier(u32 image_index) const noexcept;

        // Bundles the fields a VkPresentInfoKHR needs pointers into — kept together so callers
        // keep this object alive across the vkQueuePresentKHR() call it feeds, same rationale as
        // RenderingAttachments above.
        struct PresentRequest {
            VkSwapchainKHR swapchain{};
            u32 image_index{};
            VkSemaphore wait_semaphore{};

            [[nodiscard]] VkPresentInfoKHR present_info() const noexcept;
        };

        // Waits on this swapchain image's render-finished semaphore (signaled by the submit that
        // rendered into it) before presenting image_index.
        [[nodiscard]] PresentRequest present_request(u32 image_index) const noexcept;

        // VK_SUBOPTIMAL_KHR is treated as success — caller should rebuild the swapchain after the frame.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence = VK_NULL_HANDLE,
            u64 timeout_ns = UINT64_MAX) noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        vector<VkImage> images_;
        vector<VulkanImageView> image_views_;
        VulkanImage depth_image_;
        VulkanImageView depth_image_view_;
        vector<VulkanSemaphore> render_finished_semaphores_;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkExtent2D extent_ = {};
        VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    };

} // namespace SFT::Core::Vulkan
