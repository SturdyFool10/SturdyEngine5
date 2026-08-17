#include "VulkanSurface.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Returns the current or globally available destroy command resources value.
///
/// @return Returns the current destroy command resources value.
/// @note This function does not throw exceptions.
void FrameResources::destroyCommandResources() noexcept {
            ZoneScopedN("FrameResources::destroyCommandResources");
            commandBuffer.destroy();
            commandPool.destroy();
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void FrameResources::destroy() noexcept {
            ZoneScopedN("FrameResources::destroy");
            destroyCommandResources();
            imageAcquiredSemaphore.destroy();
        }

/// Performs the vulkan surface operation for `Vulkan` using the supplied arguments.
///
/// @param vk_surface Surface used or affected by the operation.
/// @param descriptor Description of the resource or operation to perform.
/// @param window Window used or affected by the operation.
/// @param extent `extent` value used by the operation.
/// @param frames_in_flight `frames_in_flight` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanSurface::VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight) noexcept
            : window_(window), descriptor_(descriptor), extent_(extent),
              vk_surface_(vk_surface), frames_in_flight_(frames_in_flight),
              active_(true), swapchain_dirty_(true) {}

/// Performs the vulkan surface operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanSurface::VulkanSurface(VulkanSurface &&o) noexcept
            : window_(o.window_), descriptor_(o.descriptor_), extent_(o.extent_),
              vk_surface_(o.vk_surface_), rhi_surface_(o.rhi_surface_), swapchain_(std::move(o.swapchain_)), frames_in_flight_(o.frames_in_flight_),
              active_(o.active_), swapchain_dirty_(o.swapchain_dirty_),
              frames_(std::move(o.frames_)), frame_timeline_(std::move(o.frame_timeline_)),
              frame_cursor_(o.frame_cursor_), next_signal_value_(o.next_signal_value_) {
            ZoneScopedN("VulkanSurface::VulkanSurface");
            o.vk_surface_ = VK_NULL_HANDLE;
            o.rhi_surface_ = {};
            o.active_ = false;
            o.frame_cursor_ = 0;
            o.next_signal_value_ = 0;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanSurface &VulkanSurface::operator=(VulkanSurface &&o) noexcept {
            ZoneScopedN("VulkanSurface::operator=");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSurfaceKHR VulkanSurface::vk_handle() const noexcept { return vk_surface_; }

/// Returns the current or globally available RHI surface value.
///
/// @return Returns the current RHI surface value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::SurfaceHandle VulkanSurface::rhi_surface() const noexcept { return rhi_surface_; }

/// Sets the RHI surface for this `Vulkan`.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSurface::set_rhi_surface(RHI::SurfaceHandle surface) noexcept { rhi_surface_ = surface; }

/// Clears RHI surface.
///
/// @return Returns the current clear RHI surface value.
/// @note This function does not throw exceptions.
void VulkanSurface::clear_rhi_surface() noexcept { rhi_surface_ = {}; }

/// Reports whether active holds for this `Vulkan`.
///
/// @return Returns the current is active value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSurface::is_active() const noexcept { return active_; }

/// Returns the current or globally available descriptor value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const RenderSurfaceDescriptor &VulkanSurface::descriptor() const noexcept { return descriptor_; }

/// Returns the current or globally available extent value.
///
/// @return Returns the current extent value.
/// @note This function does not throw exceptions.
[[nodiscard]] Extent2D VulkanSurface::extent() const noexcept { return extent_; }

/// Returns the current or globally available frames in flight value.
///
/// @return Returns the current frames in flight value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanSurface::frames_in_flight() const noexcept { return frames_in_flight_; }

/// Returns the current or globally available swapchain dirty value.
///
/// @return Returns the current swapchain dirty value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSurface::swapchain_dirty() const noexcept { return swapchain_dirty_; }

/// Returns the current or globally available window value.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] Window *VulkanSurface::window() const noexcept { return window_; }

/// Returns the current or globally available window ID value.
///
/// @return Returns the current window ID value.
/// @note This function does not throw exceptions.
[[nodiscard]] WindowId VulkanSurface::window_id() const noexcept { return window_ ? window_->id() : Platform::Windowing::invalid_window_id; }

/// Returns the current or globally available swapchain value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanSwapchain &VulkanSurface::swapchain() noexcept { return swapchain_; }

/// Returns the current or globally available swapchain value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VulkanSwapchain &VulkanSurface::swapchain() const noexcept { return swapchain_; }

/// Sets the swapchain for this `Vulkan`.
///
/// @param swapchain Swapchain used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSurface::set_swapchain(VulkanSwapchain swapchain) noexcept { swapchain_ = std::move(swapchain); }

/// Reports whether this `Vulkan` has frame resources.
///
/// @return Returns the current has frame resources value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSurface::has_frame_resources() const noexcept { return !frames_.empty(); }

/// Returns the current or globally available frame timeline value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanSemaphore &VulkanSurface::frame_timeline() noexcept { return frame_timeline_; }

/// Returns the current or globally available begin frame value.
///
/// @return Returns the current begin frame value.
/// @note This function does not throw exceptions.
[[nodiscard]] VulkanSurface::FrameTicket VulkanSurface::begin_frame() noexcept {
            ZoneScopedN("VulkanSurface::begin_frame");
            const u32 slot = frame_cursor_++ % frames_in_flight_;
            const u64 signal_value = next_signal_value_++;
            const u64 wait_value = signal_value - frames_in_flight_;
            return FrameTicket{&frames_[slot], signal_value, wait_value};
        }

/// Sets the frame resources for this `Vulkan`.
///
/// @param frames `frames` value used by the operation.
/// @param timeline `timeline` value used by the operation.
/// @param next_signal_value Value consumed by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSurface::set_frame_resources(vector<FrameResources> frames, VulkanSemaphore timeline, u64 next_signal_value) noexcept {
            ZoneScopedN("VulkanSurface::set_frame_resources");
            destroy_frame_resources();
            frames_ = std::move(frames);
            frame_timeline_ = std::move(timeline);
            next_signal_value_ = next_signal_value;
            frame_cursor_ = 0;
        }

/// Destroys the frame resources identified by the supplied parameters.
///
/// @return Returns the current destroy frame resources value.
/// @note This function does not throw exceptions.
void VulkanSurface::destroy_frame_resources() noexcept {
            ZoneScopedN("VulkanSurface::destroy_frame_resources");
            std::ranges::for_each(frames_, &FrameResources::destroy);
            frames_.clear();
            frame_timeline_.destroy();
            frame_cursor_ = 0;
            next_signal_value_ = 0;
        }

/// Marks dirty using the supplied arguments and current state.
///
/// @return Returns the current mark dirty value.
/// @note This function does not throw exceptions.
void VulkanSurface::mark_dirty() noexcept { swapchain_dirty_ = true; }

/// Clears dirty.
///
/// @return Returns the current clear dirty value.
/// @note This function does not throw exceptions.
void VulkanSurface::clear_dirty() noexcept { swapchain_dirty_ = false; }

/// Sets the extent for this `Vulkan`.
///
/// @param extent `extent` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSurface::set_extent(Extent2D extent) noexcept { extent_ = extent; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @param instance Instance used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanSurface::destroy(VkInstance instance) noexcept {
            ZoneScopedN("VulkanSurface::destroy");
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
