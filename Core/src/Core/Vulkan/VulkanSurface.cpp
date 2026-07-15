#include "VulkanSurface.hpp"

namespace SFT::Core::Vulkan {

void FrameResources::destroyCommandResources() noexcept {
            commandBuffer.destroy();
            commandPool.destroy();
        }

void FrameResources::destroy() noexcept {
            destroyCommandResources();
            imageAcquiredSemaphore.destroy();
        }

VulkanSurface::VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight) noexcept
            : window_(window), descriptor_(descriptor), extent_(extent),
              vk_surface_(vk_surface), frames_in_flight_(frames_in_flight),
              active_(true), swapchain_dirty_(true) {}

VulkanSurface::VulkanSurface(VulkanSurface &&o) noexcept
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

VulkanSurface &VulkanSurface::operator=(VulkanSurface &&o) noexcept {
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

[[nodiscard]] VkSurfaceKHR VulkanSurface::vk_handle() const noexcept { return vk_surface_; }

[[nodiscard]] RHI::SurfaceHandle VulkanSurface::rhi_surface() const noexcept { return rhi_surface_; }

void VulkanSurface::set_rhi_surface(RHI::SurfaceHandle surface) noexcept { rhi_surface_ = surface; }

void VulkanSurface::clear_rhi_surface() noexcept { rhi_surface_ = {}; }

[[nodiscard]] bool VulkanSurface::is_active() const noexcept { return active_; }

[[nodiscard]] const RenderSurfaceDescriptor &VulkanSurface::descriptor() const noexcept { return descriptor_; }

[[nodiscard]] Extent2D VulkanSurface::extent() const noexcept { return extent_; }

[[nodiscard]] u32 VulkanSurface::frames_in_flight() const noexcept { return frames_in_flight_; }

[[nodiscard]] bool VulkanSurface::swapchain_dirty() const noexcept { return swapchain_dirty_; }

[[nodiscard]] Window *VulkanSurface::window() const noexcept { return window_; }

[[nodiscard]] WindowId VulkanSurface::window_id() const noexcept { return window_ ? window_->id() : Platform::Windowing::invalid_window_id; }

[[nodiscard]] VulkanSwapchain &VulkanSurface::swapchain() noexcept { return swapchain_; }

[[nodiscard]] const VulkanSwapchain &VulkanSurface::swapchain() const noexcept { return swapchain_; }

void VulkanSurface::set_swapchain(VulkanSwapchain swapchain) noexcept { swapchain_ = std::move(swapchain); }

[[nodiscard]] bool VulkanSurface::has_frame_resources() const noexcept { return !frames_.empty(); }

[[nodiscard]] VulkanSemaphore &VulkanSurface::frame_timeline() noexcept { return frame_timeline_; }

[[nodiscard]] VulkanSurface::FrameTicket VulkanSurface::begin_frame() noexcept {
            const u32 slot = frame_cursor_++ % frames_in_flight_;
            const u64 signal_value = next_signal_value_++;
            const u64 wait_value = signal_value - frames_in_flight_;
            return FrameTicket{&frames_[slot], signal_value, wait_value};
        }

void VulkanSurface::set_frame_resources(vector<FrameResources> frames, VulkanSemaphore timeline, u64 next_signal_value) noexcept {
            destroy_frame_resources();
            frames_ = std::move(frames);
            frame_timeline_ = std::move(timeline);
            next_signal_value_ = next_signal_value;
            frame_cursor_ = 0;
        }

void VulkanSurface::destroy_frame_resources() noexcept {
            std::ranges::for_each(frames_, &FrameResources::destroy);
            frames_.clear();
            frame_timeline_.destroy();
            frame_cursor_ = 0;
            next_signal_value_ = 0;
        }

void VulkanSurface::mark_dirty() noexcept { swapchain_dirty_ = true; }

void VulkanSurface::clear_dirty() noexcept { swapchain_dirty_ = false; }

void VulkanSurface::refresh_extent() noexcept {
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

void VulkanSurface::destroy(VkInstance instance) noexcept {
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

} // namespace SFT::Core::Vulkan
