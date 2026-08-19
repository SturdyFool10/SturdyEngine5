#include <Core/Vulkan/VulkanSwapchain.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanSwapchain::~VulkanSwapchain() { destroy(); }

/// Performs the vulkan swapchain operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanSwapchain::VulkanSwapchain(VulkanSwapchain &&o) noexcept
            : device_(o.device_), swapchain_(o.swapchain_), images_(std::move(o.images_)),
              image_views_(std::move(o.image_views_)), depth_image_(std::move(o.depth_image_)),
              depth_image_view_(std::move(o.depth_image_view_)),
              render_finished_semaphores_(std::move(o.render_finished_semaphores_)),
              format_(o.format_), color_space_(o.color_space_),
              extent_(o.extent_), present_mode_(o.present_mode_) {
            ZoneScopedN("VulkanSwapchain::VulkanSwapchain");
            o.device_ = VK_NULL_HANDLE;
            o.swapchain_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanSwapchain &VulkanSwapchain::operator=(VulkanSwapchain &&o) noexcept {
            ZoneScopedN("VulkanSwapchain::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanSwapchain> VulkanSwapchain::create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept {
            ZoneScopedN("VulkanSwapchain::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSwapchainKHR VulkanSwapchain::vk_handle() const noexcept { return swapchain_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSwapchain::is_valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }

/// Formats the supplied value into the provided formatting context.
///
/// @return Returns the current format value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkFormat VulkanSwapchain::format() const noexcept { return format_; }

/// Returns the current or globally available color space value.
///
/// @return Returns the current color space value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkColorSpaceKHR VulkanSwapchain::color_space() const noexcept { return color_space_; }

/// Returns the current or globally available extent value.
///
/// @return Returns the current extent value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkExtent2D VulkanSwapchain::extent() const noexcept { return extent_; }

/// Presents the completed frame to the target surface or swapchain.
///
/// @return Returns the current present mode value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPresentModeKHR VulkanSwapchain::present_mode() const noexcept { return present_mode_; }

/// Returns the image count for this `Vulkan`.
///
/// @return Returns the current image count value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanSwapchain::image_count() const noexcept { return static_cast<u32>(images_.size()); }

/// Returns the current or globally available images value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<VkImage> &VulkanSwapchain::images() const noexcept { return images_; }

/// Performs the image operation for `Vulkan` using the supplied arguments.
///
/// @param i Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImage VulkanSwapchain::image(u32 i) const noexcept { return images_[i]; }

/// Sets the image views for this `Vulkan`.
///
/// @param views `views` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSwapchain::set_image_views(vector<VulkanImageView> views) noexcept { image_views_ = std::move(views); }

/// Returns the current or globally available image views value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<VulkanImageView> &VulkanSwapchain::image_views() const noexcept { return image_views_; }

/// Performs the image view operation for `Vulkan` using the supplied arguments.
///
/// @param i Zero-based index of the target element or entry.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageView VulkanSwapchain::image_view(u32 i) const noexcept { return image_views_[i].vk_handle(); }

/// Sets the depth attachment for this `Vulkan`.
///
/// @param image `image` value used by the operation.
/// @param view `view` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSwapchain::set_depth_attachment(VulkanImage image, VulkanImageView view) noexcept {
            ZoneScopedN("VulkanSwapchain::set_depth_attachment");
            depth_image_view_ = std::move(view);
            depth_image_ = std::move(image);
        }

/// Returns the current or globally available depth image value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanImage &VulkanSwapchain::depth_image() const noexcept { return depth_image_; }

/// Returns the current or globally available depth image view value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanImageView &VulkanSwapchain::depth_image_view() const noexcept { return depth_image_view_; }

/// Returns the depth image view handle associated with this `Vulkan`.
///
/// @return Returns the current depth image view handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkImageView VulkanSwapchain::depth_image_view_handle() const noexcept { return depth_image_view_.vk_handle(); }

/// Sets the render finished semaphores for this `Vulkan`.
///
/// @param semaphores Semaphore used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSwapchain::set_render_finished_semaphores(vector<VulkanSemaphore> semaphores) noexcept {
            ZoneScopedN("VulkanSwapchain::set_render_finished_semaphores");
            render_finished_semaphores_ = std::move(semaphores);
        }

/// Renders finished semaphores using the current rendering state.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<VulkanSemaphore> &VulkanSwapchain::render_finished_semaphores() const noexcept {
            ZoneScopedN("VulkanSwapchain::render_finished_semaphores");
            return render_finished_semaphores_;
        }

/// Renders finished semaphore using the current rendering state.
///
/// @param image_index Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSemaphore VulkanSwapchain::render_finished_semaphore(u32 image_index) const noexcept {
            ZoneScopedN("VulkanSwapchain::render_finished_semaphore");
            return render_finished_semaphores_[image_index].vk_handle();
        }

/// Renders info using the current rendering state.
///
/// @param render_area `render_area` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkRenderingInfo VulkanSwapchain::RenderingAttachments::rendering_info(VkRect2D render_area) const noexcept {
                ZoneScopedN("RenderingAttachments::rendering_info");
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

/// Renders attachments using the current rendering state.
///
/// @param image_index Zero-based index of the target element or entry.
/// @param clear_color `clear_color` value used by the operation.
/// @param clear_depth `clear_depth` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanSwapchain::RenderingAttachments VulkanSwapchain::rendering_attachments(
            u32 image_index,
            VkClearColorValue clear_color,
            VkClearDepthStencilValue clear_depth) const noexcept {
            ZoneScopedN("VulkanSwapchain::rendering_attachments");
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

/// Performs the undefined to attachment barriers operation for `Vulkan` using the supplied arguments.
///
/// @param image_index Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VkImageMemoryBarrier2> VulkanSwapchain::undefined_to_attachment_barriers(u32 image_index) const noexcept {
            ZoneScopedN("VulkanSwapchain::undefined_to_attachment_barriers");
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

/// Performs the attachment to present barrier operation for `Vulkan` using the supplied arguments.
///
/// @param image_index Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VkImageMemoryBarrier2> VulkanSwapchain::attachment_to_present_barrier(u32 image_index) const noexcept {
            ZoneScopedN("VulkanSwapchain::attachment_to_present_barrier");
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

/// Returns the current present info.
///
/// @return Returns the current present info value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPresentInfoKHR VulkanSwapchain::PresentRequest::present_info() const noexcept {
                ZoneScopedN("PresentRequest::present_info");
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

/// Presents the completed frame to the target surface or swapchain.
///
/// @param image_index Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanSwapchain::PresentRequest VulkanSwapchain::present_request(u32 image_index) const noexcept {
            ZoneScopedN("VulkanSwapchain::present_request");
            return PresentRequest{
                .swapchain = swapchain_,
                .image_index = image_index,
                .wait_semaphore = render_finished_semaphores_[image_index].vk_handle(),
            };
        }

/// Acquires next image.
///
/// @param signal_semaphore Semaphore used or affected by the operation.
/// @param fence Fence used or affected by the operation.
/// @param timeout_ns Maximum amount of time to wait before giving up.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<u32> VulkanSwapchain::acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence,
            u64 timeout_ns) noexcept {
            ZoneScopedN("VulkanSwapchain::acquire_next_image");
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

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanSwapchain::destroy() noexcept {
            ZoneScopedN("VulkanSwapchain::destroy");

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
