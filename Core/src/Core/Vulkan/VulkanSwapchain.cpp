#include "VulkanSwapchain.hpp"

namespace SFT::Core::Vulkan {

VulkanSwapchain::~VulkanSwapchain() { destroy(); }

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &&o) noexcept
            : device_(o.device_), swapchain_(o.swapchain_), images_(std::move(o.images_)),
              image_views_(std::move(o.image_views_)), depth_image_(std::move(o.depth_image_)),
              depth_image_view_(std::move(o.depth_image_view_)),
              render_finished_semaphores_(std::move(o.render_finished_semaphores_)),
              format_(o.format_), color_space_(o.color_space_),
              extent_(o.extent_), present_mode_(o.present_mode_) {
            o.device_ = VK_NULL_HANDLE;
            o.swapchain_ = VK_NULL_HANDLE;
        }

VulkanSwapchain &VulkanSwapchain::operator=(VulkanSwapchain &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanSwapchain> VulkanSwapchain::create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept {
            VkSwapchainKHR sc = VK_NULL_HANDLE;
            if (vkCreateSwapchainKHR(device, &info, nullptr, &sc) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSwapchainKHR failed.");

            u32 count = 0;
            if (vkGetSwapchainImagesKHR(device, sc, &count, nullptr) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (count) failed.");
            }
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (vkGetSwapchainImagesKHR(device, sc, &count, images.data()) != VK_SUCCESS) {
                vkDestroySwapchainKHR(device, sc, nullptr);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (populate) failed.");
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

[[nodiscard]] VkSwapchainKHR VulkanSwapchain::vk_handle() const noexcept { return swapchain_; }

[[nodiscard]] bool VulkanSwapchain::is_valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }

[[nodiscard]] VkFormat VulkanSwapchain::format() const noexcept { return format_; }

[[nodiscard]] VkColorSpaceKHR VulkanSwapchain::color_space() const noexcept { return color_space_; }

[[nodiscard]] VkExtent2D VulkanSwapchain::extent() const noexcept { return extent_; }

[[nodiscard]] VkPresentModeKHR VulkanSwapchain::present_mode() const noexcept { return present_mode_; }

[[nodiscard]] u32 VulkanSwapchain::image_count() const noexcept { return static_cast<u32>(images_.size()); }

[[nodiscard]] const vector<VkImage> &VulkanSwapchain::images() const noexcept { return images_; }

[[nodiscard]] VkImage VulkanSwapchain::image(u32 i) const noexcept { return images_[i]; }

void VulkanSwapchain::set_image_views(vector<VulkanImageView> views) noexcept { image_views_ = std::move(views); }

[[nodiscard]] const vector<VulkanImageView> &VulkanSwapchain::image_views() const noexcept { return image_views_; }

[[nodiscard]] VkImageView VulkanSwapchain::image_view(u32 i) const noexcept { return image_views_[i].vk_handle(); }

void VulkanSwapchain::set_depth_attachment(VulkanImage image, VulkanImageView view) noexcept {
            depth_image_view_ = std::move(view);
            depth_image_ = std::move(image);
        }

[[nodiscard]] const VulkanImage &VulkanSwapchain::depth_image() const noexcept { return depth_image_; }

[[nodiscard]] const VulkanImageView &VulkanSwapchain::depth_image_view() const noexcept { return depth_image_view_; }

[[nodiscard]] VkImageView VulkanSwapchain::depth_image_view_handle() const noexcept { return depth_image_view_.vk_handle(); }

void VulkanSwapchain::set_render_finished_semaphores(vector<VulkanSemaphore> semaphores) noexcept {
            render_finished_semaphores_ = std::move(semaphores);
        }

[[nodiscard]] const vector<VulkanSemaphore> &VulkanSwapchain::render_finished_semaphores() const noexcept {
            return render_finished_semaphores_;
        }

[[nodiscard]] VkSemaphore VulkanSwapchain::render_finished_semaphore(u32 image_index) const noexcept {
            return render_finished_semaphores_[image_index].vk_handle();
        }

[[nodiscard]] VkRenderingInfo VulkanSwapchain::RenderingAttachments::rendering_info(VkRect2D render_area) const noexcept {
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

[[nodiscard]] VulkanSwapchain::RenderingAttachments VulkanSwapchain::rendering_attachments(
            u32 image_index,
            VkClearColorValue clear_color,
            VkClearDepthStencilValue clear_depth) const noexcept {
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

[[nodiscard]] vector<VkImageMemoryBarrier2> VulkanSwapchain::undefined_to_attachment_barriers(u32 image_index) const noexcept {
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

[[nodiscard]] vector<VkImageMemoryBarrier2> VulkanSwapchain::attachment_to_present_barrier(u32 image_index) const noexcept {
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

[[nodiscard]] VkPresentInfoKHR VulkanSwapchain::PresentRequest::present_info() const noexcept {
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

[[nodiscard]] VulkanSwapchain::PresentRequest VulkanSwapchain::present_request(u32 image_index) const noexcept {
            return PresentRequest{
                .swapchain = swapchain_,
                .image_index = image_index,
                .wait_semaphore = render_finished_semaphores_[image_index].vk_handle(),
            };
        }

[[nodiscard]] RendererExpected<u32> VulkanSwapchain::acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence,
            u64 timeout_ns) noexcept {
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
            if (res == VK_ERROR_DEVICE_LOST)
                return graphics_backend_error(GraphicsBackendErrorCode::DeviceLost, "vkAcquireNextImage2KHR reported device loss.");
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkAcquireNextImage2KHR failed.");
            return index;
        }

void VulkanSwapchain::destroy() noexcept {
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

} // namespace SFT::Core::Vulkan
