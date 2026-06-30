module;
#include "volk.h"
#include <utility>

export module Sturdy.Core:VulkanSurface;

import :RenderSurface;
import :RendererError;
import :VulkanHelpers;
import :VulkanSwapchain;
import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Core::Extent2D;
using SFT::Core::RenderSurfaceDescriptor;
using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;

export namespace SFT::Core::Vulkan {

    // Owns a VkSurfaceKHR, its swapchain, and all other per-window presentation state. One
    // instance exists per live window, keyed by that window's WindowId in the backend's surface
    // map. Explicit destroy(VkInstance) is required — the backend calls it in reverse creation
    // order before vkDestroyInstance.
    class VulkanSurface {
      public:
        VulkanSurface() = default;
        VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight) noexcept
            : window_(window), descriptor_(descriptor), extent_(extent),
              vk_surface_(vk_surface), frames_in_flight_(frames_in_flight),
              active_(true), swapchain_dirty_(true) {}

        VulkanSurface(const VulkanSurface &) = delete;
        VulkanSurface &operator=(const VulkanSurface &) = delete;

        VulkanSurface(VulkanSurface &&o) noexcept
            : window_(o.window_), descriptor_(o.descriptor_), extent_(o.extent_),
              vk_surface_(o.vk_surface_), swapchain_(std::move(o.swapchain_)), frames_in_flight_(o.frames_in_flight_),
              active_(o.active_), swapchain_dirty_(o.swapchain_dirty_) {
            o.vk_surface_ = VK_NULL_HANDLE;
            o.active_ = false;
        }

        VulkanSurface &operator=(VulkanSurface &&o) noexcept {
            if (this != &o) {
                window_ = o.window_;
                descriptor_ = o.descriptor_;
                extent_ = o.extent_;
                vk_surface_ = o.vk_surface_;
                swapchain_ = std::move(o.swapchain_);
                frames_in_flight_ = o.frames_in_flight_;
                active_ = o.active_;
                swapchain_dirty_ = o.swapchain_dirty_;
                o.vk_surface_ = VK_NULL_HANDLE;
                o.active_ = false;
            }
            return *this;
        }

        [[nodiscard]] VkSurfaceKHR vk_handle() const noexcept { return vk_surface_; }
        [[nodiscard]] bool is_active() const noexcept { return active_; }
        [[nodiscard]] const RenderSurfaceDescriptor &descriptor() const noexcept { return descriptor_; }
        [[nodiscard]] Extent2D extent() const noexcept { return extent_; }
        [[nodiscard]] u32 frames_in_flight() const noexcept { return frames_in_flight_; }
        [[nodiscard]] bool swapchain_dirty() const noexcept { return swapchain_dirty_; }
        [[nodiscard]] Window *window() const noexcept { return window_; }
        [[nodiscard]] WindowId window_id() const noexcept { return window_ ? window_->id() : Platform::Windowing::invalid_window_id; }

        [[nodiscard]] VulkanSwapchain &swapchain() noexcept { return swapchain_; }
        [[nodiscard]] const VulkanSwapchain &swapchain() const noexcept { return swapchain_; }
        void set_swapchain(VulkanSwapchain swapchain) noexcept { swapchain_ = std::move(swapchain); }

        void mark_dirty() noexcept { swapchain_dirty_ = true; }
        void clear_dirty() noexcept { swapchain_dirty_ = false; }

        void refresh_extent() noexcept {
            if (!window_) {
                extent_ = {};
                return;
            }
            if (auto fb = window_->framebuffer_size()) {
                extent_ = {fb->x, fb->y};
            } else {
                extent_ = {};
            }
        }

        // Destroys the swapchain and the VkSurfaceKHR, and marks this entry inactive.
        // TODO(renderer): destroy per-frame sync objects here too once they exist.
        void destroy(VkInstance instance) noexcept {
            swapchain_.destroy();
            if (vk_surface_ != VK_NULL_HANDLE) {
                Foundation::log_info("Vulkan surface destroyed: provider={} system={}",
                                     surface_provider_name(descriptor_.provider),
                                     surface_system_name(descriptor_.system));
                vkDestroySurfaceKHR(instance, vk_surface_, nullptr);
                vk_surface_ = VK_NULL_HANDLE;
            }
            window_ = nullptr;
            descriptor_ = {};
            extent_ = {};
            frames_in_flight_ = 2;
            active_ = false;
            swapchain_dirty_ = false;
        }

      private:
        Window *window_ = nullptr;
        RenderSurfaceDescriptor descriptor_{};
        Extent2D extent_{};
        VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
        VulkanSwapchain swapchain_{};
        u32 frames_in_flight_ = 2;
        bool active_ = false;
        bool swapchain_dirty_ = false;
    };

} // namespace SFT::Core::Vulkan
