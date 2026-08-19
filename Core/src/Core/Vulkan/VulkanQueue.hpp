#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <variant>
#pragma endregion

#include <Async/Mutex.hpp>
#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::PresentOutcome;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;

namespace SFT::Core::Vulkan {


    class VulkanQueue {
      public:
        /// Constructs a `VulkanQueue` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanQueue() = default;

        /// Constructs a `VulkanQueue` from the supplied initialization values.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param family_index Zero-based index of the target element or entry.
        ///
        /// @note This function does not throw exceptions.
        VulkanQueue(VkQueue handle, u32 family_index) noexcept;

        /// Disables this construction form for `VulkanQueue`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanQueue(const VulkanQueue &) = delete;
        /// Assigns a new value to this `VulkanQueue`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanQueue &operator=(const VulkanQueue &) = delete;

        /// Constructs a `VulkanQueue` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanQueue(VulkanQueue &&o) noexcept;

        /// Assigns a new value to this `VulkanQueue`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanQueue &operator=(VulkanQueue &&o) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanQueue`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueue vk_handle() const noexcept;
        /// Computes the family index required by the supplied values.
        ///
        /// @return Returns the current family index value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 family_index() const noexcept;
        /// Reports whether valid holds for this `VulkanQueue`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Submits the requested work.
        ///
        /// @param submits `submits` value used by the operation.
        /// @param fence Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult submit(span<const VkSubmitInfo2> submits,
                                            VkFence fence = VK_NULL_HANDLE) noexcept;


        /// Submits the requested work.
        ///
        /// @param command_buffer Buffer used or affected by the operation.
        /// @param waits `waits` value used by the operation.
        /// @param signals `signals` value used by the operation.
        /// @param fence Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult submit(
            const VkCommandBufferSubmitInfo &command_buffer,
            span<const VkSemaphoreSubmitInfo> waits,
            span<const VkSemaphoreSubmitInfo> signals,
            VkFence fence = VK_NULL_HANDLE) noexcept;


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param info Description of the resource or operation to perform.
        /// @param lock_wait_ms `lock_wait_ms` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::SurfaceLost`, `GraphicsBackendErrorCode::FullScreenExclusiveLost`, `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<PresentOutcome> present(const VkPresentInfoKHR &info, f64 *lock_wait_ms = nullptr) noexcept;

        /// Waits for idle to complete.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult wait_idle() noexcept;

        /// Binds sparse for subsequent operations.
        ///
        /// @param infos Description of the resource or operation to perform.
        /// @param fence Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult bind_sparse(span<const VkBindSparseInfo> infos,
                                                 VkFence fence = VK_NULL_HANDLE) noexcept;

      private:
        VkQueue handle_ = VK_NULL_HANDLE;
        u32 family_index_ = 0;


        mutable Async::Mutex<std::monostate> submission_lock_;
    };

} // namespace SFT::Core::Vulkan
