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

        void destroyCommandResources() noexcept;

        void destroy() noexcept;
    };

    // Owns a VkSurfaceKHR, its swapchain, and all other per-window presentation state. One
    // instance exists per live window, keyed by that window's WindowId in the backend's surface
    // map. Explicit destroy(VkInstance) is required — the backend calls it in reverse creation
    // order before vkDestroyInstance.
    class VulkanSurface {
      public:
        VulkanSurface() = default;
        VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight) noexcept;

        VulkanSurface(const VulkanSurface &) = delete;
        VulkanSurface &operator=(const VulkanSurface &) = delete;

        VulkanSurface(VulkanSurface &&o) noexcept;

        VulkanSurface &operator=(VulkanSurface &&o) noexcept;

        [[nodiscard]] VkSurfaceKHR vk_handle() const noexcept;
        [[nodiscard]] RHI::SurfaceHandle rhi_surface() const noexcept;
        void set_rhi_surface(RHI::SurfaceHandle surface) noexcept;
        void clear_rhi_surface() noexcept;
        [[nodiscard]] bool is_active() const noexcept;
        [[nodiscard]] const RenderSurfaceDescriptor &descriptor() const noexcept;
        [[nodiscard]] Extent2D extent() const noexcept;
        [[nodiscard]] u32 frames_in_flight() const noexcept;
        [[nodiscard]] bool swapchain_dirty() const noexcept;
        [[nodiscard]] Window *window() const noexcept;
        [[nodiscard]] WindowId window_id() const noexcept;

        [[nodiscard]] VulkanSwapchain &swapchain() noexcept;
        [[nodiscard]] const VulkanSwapchain &swapchain() const noexcept;
        void set_swapchain(VulkanSwapchain swapchain) noexcept;

        // What one render_frame call needs from the surface's frame pacing: the resources to record
        // into, the timeline value this frame's submission signals on completion, and the value a
        // prior in-flight frame must have already reached before its resources may be reused.
        struct FrameTicket {
            FrameResources *resources = nullptr;
            u64 signal_value = 0;
            u64 wait_value = 0;
        };

        [[nodiscard]] bool has_frame_resources() const noexcept;
        [[nodiscard]] VulkanSemaphore &frame_timeline() noexcept;

        // Advances the per-surface frame cursor and hands back the next frame's resources + timeline
        // values. Must only be called when has_frame_resources() is true (frames_in_flight_ > 0).
        [[nodiscard]] FrameTicket begin_frame() noexcept;

        // Installs a freshly built set of frame-pacing resources, tearing down any previous set.
        // The timeline must already be created with initial value frames_in_flight_ so the first
        // frames_in_flight_ frames never block; next_signal_value seeds the signal counter to match.
        void set_frame_resources(vector<FrameResources> frames, VulkanSemaphore timeline, u64 next_signal_value) noexcept;

        void destroy_frame_resources() noexcept;

        void mark_dirty() noexcept;
        void clear_dirty() noexcept;

        // Sets the surface's tracked extent directly from an already-resolved framebuffer size.
        // Deliberately does not query window_->framebuffer_size() itself: this is called from
        // on_surface_resize_needed(), which can run on a render thread where the owning Window is
        // not safe to touch (see EngineBackend::on_surface_resize_needed's docs).
        void set_extent(Extent2D extent) noexcept;

        // Destroys the per-frame sync/command resources, the swapchain and the VkSurfaceKHR, and
        // marks this entry inactive. Frame resources go first: they may hold command buffers with
        // pending references to swapchain images, and the caller is expected to have drained the
        // GPU (wait_idle) before invoking this.
        void destroy(VkInstance instance) noexcept;

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
