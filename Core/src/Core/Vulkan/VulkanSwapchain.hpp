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


    class VulkanSwapchain {
      public:
        /// Constructs a `VulkanSwapchain` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanSwapchain() = default;
        /// Destroys the `VulkanSwapchain` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanSwapchain();

        /// Disables this construction form for `VulkanSwapchain`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSwapchain(const VulkanSwapchain &) = delete;
        /// Assigns a new value to this `VulkanSwapchain`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSwapchain &operator=(const VulkanSwapchain &) = delete;

        /// Constructs a `VulkanSwapchain` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanSwapchain(VulkanSwapchain &&o) noexcept;

        /// Assigns a new value to this `VulkanSwapchain`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanSwapchain &operator=(VulkanSwapchain &&o) noexcept;

        /// Creates a `VulkanSwapchain` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanSwapchain> create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR &info) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanSwapchain`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSwapchainKHR vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanSwapchain`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the current format value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkFormat format() const noexcept;
        /// Returns the current or globally available color space value.
        ///
        /// @return Returns the current color space value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkColorSpaceKHR color_space() const noexcept;
        /// Returns the current or globally available extent value.
        ///
        /// @return Returns the current extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkExtent2D extent() const noexcept;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @return Returns the current present mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPresentModeKHR present_mode() const noexcept;
        /// Returns the image count for this `VulkanSwapchain`.
        ///
        /// @return Returns the current image count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 image_count() const noexcept;
        /// Returns the current or globally available images value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<VkImage> &images() const noexcept;
        /// Performs the image operation for `VulkanSwapchain` using the supplied arguments.
        ///
        /// @param i Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImage image(u32 i) const noexcept;


        /// Sets the image views for this `VulkanSwapchain`.
        ///
        /// @param views `views` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_image_views(vector<VulkanImageView> views) noexcept;
        /// Returns the current or globally available image views value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<VulkanImageView> &image_views() const noexcept;
        /// Performs the image view operation for `VulkanSwapchain` using the supplied arguments.
        ///
        /// @param i Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageView image_view(u32 i) const noexcept;

        /// Sets the depth attachment for this `VulkanSwapchain`.
        ///
        /// @param image `image` value used by the operation.
        /// @param view `view` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_depth_attachment(VulkanImage image, VulkanImageView view) noexcept;
        /// Returns the current or globally available depth image value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanImage &depth_image() const noexcept;
        /// Returns the current or globally available depth image view value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanImageView &depth_image_view() const noexcept;
        /// Returns the depth image view handle associated with this `VulkanSwapchain`.
        ///
        /// @return Returns the current depth image view handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkImageView depth_image_view_handle() const noexcept;


        /// Sets the render finished semaphores for this `VulkanSwapchain`.
        ///
        /// @param semaphores Semaphore used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_render_finished_semaphores(vector<VulkanSemaphore> semaphores) noexcept;
        /// Renders finished semaphores using the current rendering state.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<VulkanSemaphore> &render_finished_semaphores() const noexcept;
        /// Renders finished semaphore using the current rendering state.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSemaphore render_finished_semaphore(u32 image_index) const noexcept;


        struct RenderingAttachments {
            VkRenderingAttachmentInfo color{};
            VkRenderingAttachmentInfo depth{};

            /// Renders info using the current rendering state.
            ///
            /// @param render_area `render_area` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] VkRenderingInfo rendering_info(VkRect2D render_area) const noexcept;
        };


        /// Renders attachments using the current rendering state.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        /// @param clear_color `clear_color` value used by the operation.
        /// @param clear_depth `clear_depth` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderingAttachments rendering_attachments(
            u32 image_index,
            VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}},
            VkClearDepthStencilValue clear_depth = {1.0f, 0}) const noexcept;


        /// Performs the undefined to attachment barriers operation for `VulkanSwapchain` using the supplied arguments.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VkImageMemoryBarrier2> undefined_to_attachment_barriers(u32 image_index) const noexcept;


        /// Performs the attachment to present barrier operation for `VulkanSwapchain` using the supplied arguments.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] vector<VkImageMemoryBarrier2> attachment_to_present_barrier(u32 image_index) const noexcept;


        struct PresentRequest {
            VkSwapchainKHR swapchain{};
            u32 image_index{};
            VkSemaphore wait_semaphore{};

            /// Returns the current present info.
            ///
            /// @return Returns the current present info value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] VkPresentInfoKHR present_info() const noexcept;
        };


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param image_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] PresentRequest present_request(u32 image_index) const noexcept;


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
        [[nodiscard]] RendererExpected<u32> acquire_next_image(
            VkSemaphore signal_semaphore,
            VkFence fence = VK_NULL_HANDLE,
            u64 timeout_ns = UINT64_MAX) noexcept;

        /// Destroys or releases the `VulkanSwapchain` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
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
