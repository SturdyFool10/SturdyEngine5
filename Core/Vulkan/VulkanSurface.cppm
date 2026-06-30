module;
#include "volk.h"

export module Sturdy.Core:VulkanSurface;

import :RenderSurface;
import :RendererError;
import :VulkanHelpers;
import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Core::Extent2D;
using SFT::Core::RenderSurfaceDescriptor;
using SFT::Platform::Windowing::Window;

export namespace SFT::Core::Vulkan {

    // Owns a VkSurfaceKHR and all per-surface presentation state.
    // Explicit destroy(VkInstance) is required — the backend calls it in reverse creation order
    // before vkDestroyInstance. On destroy(), the generation is preserved so stale
    // RenderSurfaceHandles remain detectable after slot reuse.
    class VulkanSurface {
      public:
        VulkanSurface() = default;
        VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight, u32 generation) noexcept
            : window_(window), descriptor_(descriptor), extent_(extent),
              vk_surface_(vk_surface), frames_in_flight_(frames_in_flight),
              generation_(generation), active_(true), swapchain_dirty_(true) {}

        VulkanSurface(const VulkanSurface &) = delete;
        VulkanSurface &operator=(const VulkanSurface &) = delete;

        VulkanSurface(VulkanSurface &&o) noexcept
            : window_(o.window_), descriptor_(o.descriptor_), extent_(o.extent_),
              vk_surface_(o.vk_surface_), frames_in_flight_(o.frames_in_flight_),
              generation_(o.generation_), active_(o.active_), swapchain_dirty_(o.swapchain_dirty_) {
            o.vk_surface_ = VK_NULL_HANDLE;
            o.active_ = false;
        }

        VulkanSurface &operator=(VulkanSurface &&o) noexcept {
            if (this != &o) {
                window_ = o.window_;
                descriptor_ = o.descriptor_;
                extent_ = o.extent_;
                vk_surface_ = o.vk_surface_;
                frames_in_flight_ = o.frames_in_flight_;
                generation_ = o.generation_;
                active_ = o.active_;
                swapchain_dirty_ = o.swapchain_dirty_;
                o.vk_surface_ = VK_NULL_HANDLE;
                o.active_ = false;
            }
            return *this;
        }

        [[nodiscard]] VkSurfaceKHR vk_handle() const noexcept { return vk_surface_; }
        [[nodiscard]] bool is_active() const noexcept { return active_; }
        [[nodiscard]] u32 generation() const noexcept { return generation_; }
        [[nodiscard]] const RenderSurfaceDescriptor &descriptor() const noexcept { return descriptor_; }
        [[nodiscard]] Extent2D extent() const noexcept { return extent_; }
        [[nodiscard]] u32 frames_in_flight() const noexcept { return frames_in_flight_; }
        [[nodiscard]] bool swapchain_dirty() const noexcept { return swapchain_dirty_; }
        [[nodiscard]] Window *window() const noexcept { return window_; }

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

        // Destroys the VkSurfaceKHR and marks the slot inactive.
        // TODO(renderer): destroy swapchain and per-frame sync objects before calling this.
        void destroy(VkInstance instance) noexcept {
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
            // generation_ intentionally left unchanged for stale-handle detection on slot reuse
        }

      private:
        Window *window_ = nullptr;
        RenderSurfaceDescriptor descriptor_{};
        Extent2D extent_{};
        VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
        u32 frames_in_flight_ = 2;
        u32 generation_ = 0;
        bool active_ = false;
        bool swapchain_dirty_ = false;
    };

} // namespace SFT::Core::Vulkan
