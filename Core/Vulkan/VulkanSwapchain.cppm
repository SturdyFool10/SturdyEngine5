module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <vector>

export module Sturdy.Core:VulkanSwapchain;

import :RendererError;
import :VulkanImage;
import :VulkanSync;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::vector;

export namespace SFT::Core::Vulkan {

    // Owns a VkSwapchainKHR and caches the swapchain images (non-owning — the
    // driver owns swapchain images). Destroyed via destroy() or destructor.
    class VulkanSwapchain {
      public:
        VulkanSwapchain() = default;
        ~VulkanSwapchain() { destroy(); }

        VulkanSwapchain(const VulkanSwapchain &) = delete;
        VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

        VulkanSwapchain(VulkanSwapchain &&o) noexcept
            : device_(o.device_), swapchain_(o.swapchain_), images_(std::move(o.images_)),
              image_views_(std::move(o.image_views_)), depth_image_(std::move(o.depth_image_)),
              depth_image_view_(std::move(o.depth_image_view_)),
              render_finished_semaphores_(std::move(o.render_finished_semaphores_)),
              format_(o.format_), color_space_(o.color_space_),
              extent_(o.extent_), present_mode_(o.present_mode_) {
            o.device_ = VK_NULL_HANDLE;
            o.swapchain_ = VK_NULL_HANDLE;
        }

        VulkanSwapchain &operator=(VulkanSwapchain &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                swapchain_ = o.swapchain_;
                images_ = std::move(o.images_);
                image_views_ = std::move(o.image_views_);
                depth_image_ = std::move(o.depth_image_);
                depth_image_view_ = std::move(o.depth_image_view_);
                render_finished_semaphores_ = std::move(o.render_finished_semaphores_);
                format_ = o.format_;
                color_space_ = o.color_space_;
                extent_ = o.extent_;
                present_mode_ = o.present_mode_;
                o.device_ = VK_NULL_HANDLE;
                o.swapchain_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanSwapchain> create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept {
            VkSwapchainKHR sc = VK_NULL_HANDLE;
            if (vkCreateSwapchainKHR(device, &info, nullptr, &sc) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSwapchainKHR failed.");

            u32 count = 0;
            if (vkGetSwapchainImagesKHR(device, sc, &count, nullptr) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (count) failed.");
            }
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (vkGetSwapchainImagesKHR(device, sc, &count, images.data()) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (populate) failed.");
            }

            VulkanSwapchain out;
            out.device_ = device;
            out.swapchain_ = sc;
            out.images_ = std::move(images);
            out.format_ = info.imageFormat;
            out.color_space_ = info.imageColorSpace;
            out.extent_ = info.imageExtent;
            out.present_mode_ = info.presentMode;
            return out;
        }

        [[nodiscard]] VkSwapchainKHR vk_handle() const noexcept { return swapchain_; }
        [[nodiscard]] bool is_valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkFormat format() const noexcept { return format_; }
        [[nodiscard]] VkColorSpaceKHR color_space() const noexcept { return color_space_; }
        [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
        [[nodiscard]] VkPresentModeKHR present_mode() const noexcept { return present_mode_; }
        [[nodiscard]] u32 image_count() const noexcept { return static_cast<u32>(images_.size()); }
        [[nodiscard]] const vector<VkImage> &images() const noexcept { return images_; }
        [[nodiscard]] VkImage image(u32 i) const noexcept { return images_[i]; }

        // Populated separately from create(), once per swapchain build — one view per
        // swapchain image, in the same order as images(). Replacing the views (e.g. after a
        // swapchain rebuild) destroys whatever views were set previously.
        void set_image_views(vector<VulkanImageView> views) noexcept { image_views_ = std::move(views); }
        [[nodiscard]] const vector<VulkanImageView> &image_views() const noexcept { return image_views_; }
        [[nodiscard]] VkImageView image_view(u32 i) const noexcept { return image_views_[i].vk_handle(); }

        void set_depth_attachment(VulkanImage image, VulkanImageView view) noexcept {
            depth_image_view_ = std::move(view);
            depth_image_ = std::move(image);
        }
        [[nodiscard]] const VulkanImage &depth_image() const noexcept { return depth_image_; }
        [[nodiscard]] const VulkanImageView &depth_image_view() const noexcept { return depth_image_view_; }
        [[nodiscard]] VkImageView depth_image_view_handle() const noexcept { return depth_image_view_.vk_handle(); }

        // One binary semaphore per swapchain image, signaled when that image's rendering work
        // is done and it's safe to present. Keyed by swapchain image index (the index returned
        // by acquire_next_image), not by frame-in-flight index — reusing a single semaphore
        // across frames risks a present/wait-twice hazard if images are acquired out of order.
        void set_render_finished_semaphores(vector<VulkanSemaphore> semaphores) noexcept {
            render_finished_semaphores_ = std::move(semaphores);
        }
        [[nodiscard]] const vector<VulkanSemaphore> &render_finished_semaphores() const noexcept {
            return render_finished_semaphores_;
        }
        [[nodiscard]] VkSemaphore render_finished_semaphore(u32 image_index) const noexcept {
            return render_finished_semaphores_[image_index].vk_handle();
        }

        // Bundles the color+depth VkRenderingAttachmentInfo for one dynamic-rendering pass. Kept
        // together (rather than returned separately) since rendering_info() hands out pointers into
        // this object — callers must keep it alive across the vkCmdBeginRendering() call it feeds.
        struct RenderingAttachments {
            VkRenderingAttachmentInfo color{};
            VkRenderingAttachmentInfo depth{};

            [[nodiscard]] VkRenderingInfo rendering_info(VkRect2D render_area) const noexcept {
                return VkRenderingInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .renderArea = render_area,
                    .layerCount = 1,
                    .viewMask = 0,
                    .colorAttachmentCount = 1,
                    .pColorAttachments = &color,
                    .pDepthAttachment = &depth,
                    .pStencilAttachment = nullptr,
                };
            }
        };

        // Builds the color+depth attachment infos for image_index, clearing both on load.
        [[nodiscard]] RenderingAttachments rendering_attachments(
            u32 image_index,
            VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}},
            VkClearDepthStencilValue clear_depth = {1.0f, 0}) const noexcept {
            return RenderingAttachments{
                .color{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = image_views_[image_index].vk_handle(),
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue{.color = clear_color},
                },
                .depth{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = depth_image_view_.vk_handle(),
                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                    .clearValue{.depthStencil = clear_depth},
                },
            };
        }

        // Transitions the color image at image_index and the shared depth image from UNDEFINED to
        // their respective attachment-optimal layouts, ready for dynamic rendering to write into.
        [[nodiscard]] vector<VkImageMemoryBarrier2> undefined_to_attachment_barriers(u32 image_index) const noexcept {
            return {
                VkImageMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .srcAccessMask = 0,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .image = images_[image_index],
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                },
                VkImageMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    .srcAccessMask = 0,
                    .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    .image = depth_image_.vk_handle(),
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                },
            };
        }

        // Transitions the color image at image_index from attachment-optimal to PRESENT_SRC once
        // rendering into it has finished, ready to be handed to vkQueuePresentKHR.
        [[nodiscard]] vector<VkImageMemoryBarrier2> attachment_to_present_barrier(u32 image_index) const noexcept {
            return {
                VkImageMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
                    .dstAccessMask = 0,
                    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    .image = images_[image_index],
                    .subresourceRange{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                },
            };
        }

        // Bundles the fields a VkPresentInfoKHR needs pointers into — kept together so callers
        // keep this object alive across the vkQueuePresentKHR() call it feeds, same rationale as
        // RenderingAttachments above.
        struct PresentRequest {
            VkSwapchainKHR swapchain{};
            u32 image_index{};
            VkSemaphore wait_semaphore{};

            [[nodiscard]] VkPresentInfoKHR present_info() const noexcept {
                return VkPresentInfoKHR{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .pNext = nullptr,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &wait_semaphore,
                    .swapchainCount = 1,
                    .pSwapchains = &swapchain,
                    .pImageIndices = &image_index,
                    .pResults = nullptr,
                };
            }
        };

        // Waits on this swapchain image's render-finished semaphore (signaled by the submit that
        // rendered into it) before presenting image_index.
        [[nodiscard]] PresentRequest present_request(u32 image_index) const noexcept {
            return PresentRequest{
                .swapchain = swapchain_,
                .image_index = image_index,
                .wait_semaphore = render_finished_semaphores_[image_index].vk_handle(),
            };
        }

        // VK_SUBOPTIMAL_KHR is treated as success — caller should rebuild the swapchain after the frame.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence = VK_NULL_HANDLE,
            u64 timeout_ns = UINT64_MAX) noexcept {
            VkAcquireNextImageInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
                .pNext = nullptr,
                .swapchain = swapchain_,
                .timeout = timeout_ns,
                .semaphore = signal_semaphore,
                .fence = fence,
                .deviceMask = 1,
            };
            u32 index = 0;
            VkResult res = vkAcquireNextImage2KHR(device_, &info, &index);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return renderer_error(RendererErrorCode::OperationFailed, "vkAcquireNextImage2KHR failed.");
            return index;
        }

        void destroy() noexcept {
            // Views reference images, so tear them down before their images/swapchain.
            image_views_.clear();
            depth_image_view_.destroy();
            depth_image_.destroy();
            render_finished_semaphores_.clear();

            if (swapchain_ != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            }

            swapchain_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            images_.clear();
            format_ = VK_FORMAT_UNDEFINED;
            color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            extent_ = {};
            present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
        }

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
