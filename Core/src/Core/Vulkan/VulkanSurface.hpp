#pragma once

#include <Foundation/src/Foundation.hpp>
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


    struct FrameResources {
        VulkanSemaphore imageAcquiredSemaphore;
        VulkanCommandPool commandPool;
        VulkanCommandBuffer commandBuffer;

        /// Performs the destroy command resources operation for `FrameResources` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void destroyCommandResources() noexcept;

        /// Destroys or releases the `FrameResources` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;
    };


    class VulkanSurface {
      public:
        /// Constructs a `VulkanSurface` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanSurface() = default;
        /// Constructs a `VulkanSurface` from the supplied initialization values.
        ///
        /// @param vk_surface Surface used or affected by the operation.
        /// @param descriptor Description of the resource or operation to perform.
        /// @param window Window used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param frames_in_flight `frames_in_flight` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanSurface(VkSurfaceKHR vk_surface, RenderSurfaceDescriptor descriptor, Window *window, Extent2D extent, u32 frames_in_flight) noexcept;

        /// Disables this construction form for `VulkanSurface`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSurface(const VulkanSurface &) = delete;
        /// Assigns a new value to this `VulkanSurface`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSurface &operator=(const VulkanSurface &) = delete;

        /// Constructs a `VulkanSurface` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanSurface(VulkanSurface &&o) noexcept;

        /// Assigns a new value to this `VulkanSurface`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanSurface &operator=(VulkanSurface &&o) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanSurface`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSurfaceKHR vk_handle() const noexcept;
        /// Returns the current or globally available RHI surface value.
        ///
        /// @return Returns the current RHI surface value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::SurfaceHandle rhi_surface() const noexcept;
        /// Sets the RHI surface for this `VulkanSurface`.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_rhi_surface(RHI::SurfaceHandle surface) noexcept;
        /// Clears RHI surface.
        ///
        /// @note This function does not throw exceptions.
        void clear_rhi_surface() noexcept;
        /// Reports whether active holds for this `VulkanSurface`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_active() const noexcept;
        /// Returns the current or globally available descriptor value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RenderSurfaceDescriptor &descriptor() const noexcept;
        /// Returns the current or globally available extent value.
        ///
        /// @return Returns the current extent value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Extent2D extent() const noexcept;
        /// Returns the current or globally available frames in flight value.
        ///
        /// @return Returns the current frames in flight value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 frames_in_flight() const noexcept;
        /// Returns the current or globally available swapchain dirty value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool swapchain_dirty() const noexcept;
        /// Returns the current or globally available window value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Window *window() const noexcept;
        /// Returns the current or globally available window ID value.
        ///
        /// @return Returns the current window ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowId window_id() const noexcept;

        /// Returns the current or globally available swapchain value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanSwapchain &swapchain() noexcept;
        /// Returns the current or globally available swapchain value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanSwapchain &swapchain() const noexcept;
        /// Sets the swapchain for this `VulkanSurface`.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_swapchain(VulkanSwapchain swapchain) noexcept;


        struct FrameTicket {
            FrameResources *resources = nullptr;
            u64 signal_value = 0;
            u64 wait_value = 0;
        };

        /// Reports whether this `VulkanSurface` has frame resources.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_frame_resources() const noexcept;
        /// Returns the current or globally available frame timeline value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanSemaphore &frame_timeline() noexcept;


        /// Returns the current or globally available begin frame value.
        ///
        /// @return Returns the current begin frame value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] FrameTicket begin_frame() noexcept;


        /// Sets the frame resources for this `VulkanSurface`.
        ///
        /// @param frames `frames` value used by the operation.
        /// @param timeline `timeline` value used by the operation.
        /// @param next_signal_value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_frame_resources(vector<FrameResources> frames, VulkanSemaphore timeline, u64 next_signal_value) noexcept;

        /// Destroys the frame resources identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_frame_resources() noexcept;

        /// Marks dirty using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void mark_dirty() noexcept;
        /// Clears dirty.
        ///
        /// @note This function does not throw exceptions.
        void clear_dirty() noexcept;


        /// Sets the extent for this `VulkanSurface`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_extent(Extent2D extent) noexcept;


        /// Destroys or releases the `VulkanSurface` resource represented by the supplied parameters.
        ///
        /// @param instance Instance used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
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


        vector<FrameResources> frames_;
        VulkanSemaphore frame_timeline_;
        u32 frame_cursor_ = 0;
        u64 next_signal_value_ = 0;
    };

} // namespace SFT::Core::Vulkan
