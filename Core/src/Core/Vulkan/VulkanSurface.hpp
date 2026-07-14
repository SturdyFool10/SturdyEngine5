#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/RenderSurface.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanHelpers.hpp>
#include <Core/Vulkan/VulkanSwapchain.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <Core/Vulkan/VulkanCommandPool.hpp>
#include <Core/Vulkan/VulkanCommandBuffer.hpp>
#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>

using SFT::Core::Extent2D;
using SFT::Core::RenderSurfaceDescriptor;
using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;
using std::vector;

namespace SFT::Core::Vulkan {

    // One frame-in-flight's worth of recording/synchronization state: a dedicated command pool +
    // primary command buffer (a pool per frame allows a cheap whole-pool reset each time the slot
    // is reused) and the binary semaphore signalled when this frame's swapchain image is acquired.
    // A set of these lives on each VulkanSurface, so every window paces its own frames — the
    // groundwork for rendering multiple windows independently.
    struct FrameResources {
        VulkanSemaphore imageAcquiredSemaphore;
        VulkanCommandPool commandPool;
        VulkanCommandBuffer commandBuffer;

        void destroyCommandResources() noexcept {
            commandBuffer.destroy();
            commandPool.destroy();
        }

        void destroy() noexcept {
            destroyCommandResources();
            imageAcquiredSemaphore.destroy();
        }
    };

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
              vk_surface_(o.vk_surface_), rhi_surface_(o.rhi_surface_), swapchain_(std::move(o.swapchain_)), frames_in_flight_(o.frames_in_flight_),
              active_(o.active_), swapchain_dirty_(o.swapchain_dirty_),
              frames_(std::move(o.frames_)), frame_timeline_(std::move(o.frame_timeline_)),
              frame_cursor_(o.frame_cursor_), next_signal_value_(o.next_signal_value_) {
            o.vk_surface_ = VK_NULL_HANDLE;
            o.rhi_surface_ = {};
            o.active_ = false;
            o.frame_cursor_ = 0;
            o.next_signal_value_ = 0;
        }

        VulkanSurface &operator=(VulkanSurface &&o) noexcept {
            if (this != &o) {
                window_ = o.window_;
                descriptor_ = o.descriptor_;
                extent_ = o.extent_;
                vk_surface_ = o.vk_surface_;
                rhi_surface_ = o.rhi_surface_;
                swapchain_ = std::move(o.swapchain_);
                frames_in_flight_ = o.frames_in_flight_;
                active_ = o.active_;
                swapchain_dirty_ = o.swapchain_dirty_;
                frames_ = std::move(o.frames_);
                frame_timeline_ = std::move(o.frame_timeline_);
                frame_cursor_ = o.frame_cursor_;
                next_signal_value_ = o.next_signal_value_;
                o.vk_surface_ = VK_NULL_HANDLE;
                o.rhi_surface_ = {};
                o.active_ = false;
                o.frame_cursor_ = 0;
                o.next_signal_value_ = 0;
            }
            return *this;
        }

        [[nodiscard]] VkSurfaceKHR vk_handle() const noexcept { return vk_surface_; }
        [[nodiscard]] RHI::SurfaceHandle rhi_surface() const noexcept { return rhi_surface_; }
        void set_rhi_surface(RHI::SurfaceHandle surface) noexcept { rhi_surface_ = surface; }
        void clear_rhi_surface() noexcept { rhi_surface_ = {}; }
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

        // What one render_frame call needs from the surface's frame pacing: the resources to record
        // into, the timeline value this frame's submission signals on completion, and the value a
        // prior in-flight frame must have already reached before its resources may be reused.
        struct FrameTicket {
            FrameResources *resources = nullptr;
            u64 signal_value = 0;
            u64 wait_value = 0;
        };

        [[nodiscard]] bool has_frame_resources() const noexcept { return !frames_.empty(); }
        [[nodiscard]] VulkanSemaphore &frame_timeline() noexcept { return frame_timeline_; }

        // Advances the per-surface frame cursor and hands back the next frame's resources + timeline
        // values. Must only be called when has_frame_resources() is true (frames_in_flight_ > 0).
        [[nodiscard]] FrameTicket begin_frame() noexcept {
            const u32 slot = frame_cursor_++ % frames_in_flight_;
            const u64 signal_value = next_signal_value_++;
            const u64 wait_value = signal_value - frames_in_flight_;
            return FrameTicket{&frames_[slot], signal_value, wait_value};
        }

        // Installs a freshly built set of frame-pacing resources, tearing down any previous set.
        // The timeline must already be created with initial value frames_in_flight_ so the first
        // frames_in_flight_ frames never block; next_signal_value seeds the signal counter to match.
        void set_frame_resources(vector<FrameResources> frames, VulkanSemaphore timeline, u64 next_signal_value) noexcept {
            destroy_frame_resources();
            frames_ = std::move(frames);
            frame_timeline_ = std::move(timeline);
            next_signal_value_ = next_signal_value;
            frame_cursor_ = 0;
        }

        void destroy_frame_resources() noexcept {
            std::ranges::for_each(frames_, &FrameResources::destroy);
            frames_.clear();
            frame_timeline_.destroy();
            frame_cursor_ = 0;
            next_signal_value_ = 0;
        }

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

        // Destroys the per-frame sync/command resources, the swapchain and the VkSurfaceKHR, and
        // marks this entry inactive. Frame resources go first: they may hold command buffers with
        // pending references to swapchain images, and the caller is expected to have drained the
        // GPU (wait_idle) before invoking this.
        void destroy(VkInstance instance) noexcept {
            destroy_frame_resources();
            swapchain_.destroy();
            if (vk_surface_ != VK_NULL_HANDLE) {
                Foundation::log_info("Vulkan surface destroyed: provider={} system={}",
                                     surface_provider_name(descriptor_.provider),
                                     surface_system_name(descriptor_.system));
                vkDestroySurfaceKHR(instance, vk_surface_, nullptr);
                vk_surface_ = VK_NULL_HANDLE;
            }
            rhi_surface_ = {};
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
        RHI::SurfaceHandle rhi_surface_{};
        VulkanSwapchain swapchain_{};
        u32 frames_in_flight_ = 2;
        bool active_ = false;
        bool swapchain_dirty_ = false;

        // Per-surface frame pacing. frames_ has one entry per frame in flight; frame_timeline_ is a
        // timeline semaphore the CPU waits on to keep at most frames_in_flight_ frames outstanding.
        // frame_cursor_ round-robins through frames_; next_signal_value_ is the next value a frame
        // submission will signal. Empty/zero until the backend installs them via set_frame_resources.
        vector<FrameResources> frames_;
        VulkanSemaphore frame_timeline_;
        u32 frame_cursor_ = 0;
        u64 next_signal_value_ = 0;
    };

} // namespace SFT::Core::Vulkan
