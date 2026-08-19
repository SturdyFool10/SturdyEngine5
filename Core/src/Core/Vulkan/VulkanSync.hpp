#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {


    class VulkanFence {
      public:
        /// Constructs a `VulkanFence` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanFence() = default;
        /// Destroys the `VulkanFence` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanFence();

        /// Disables this construction form for `VulkanFence`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanFence(const VulkanFence &) = delete;
        /// Assigns a new value to this `VulkanFence`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanFence &operator=(const VulkanFence &) = delete;

        /// Constructs a `VulkanFence` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanFence(VulkanFence &&o) noexcept;
        /// Assigns a new value to this `VulkanFence`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanFence &operator=(VulkanFence &&o) noexcept;

        /// Creates a `VulkanFence` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param signaled `signaled` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanFence> create(
            VkDevice device,
            bool signaled = false) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanFence`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkFence vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanFence`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;


        /// Reports whether signaled holds for this `VulkanFence`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<bool> is_signaled() const noexcept;


        /// Waits for the associated operation or synchronization primitive to complete.
        ///
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult wait(u64 timeout_ns = UINT64_MAX) noexcept;

        /// Resets the object to its baseline state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset() noexcept;

        /// Destroys or releases the `VulkanFence` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
    };


    class VulkanSemaphore {
      public:
        /// Constructs a `VulkanSemaphore` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanSemaphore() = default;
        /// Destroys the `VulkanSemaphore` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanSemaphore();

        /// Disables this construction form for `VulkanSemaphore`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSemaphore(const VulkanSemaphore &) = delete;
        /// Assigns a new value to this `VulkanSemaphore`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSemaphore &operator=(const VulkanSemaphore &) = delete;

        /// Constructs a `VulkanSemaphore` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanSemaphore(VulkanSemaphore &&o) noexcept;
        /// Assigns a new value to this `VulkanSemaphore`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanSemaphore &operator=(VulkanSemaphore &&o) noexcept;

        /// Creates a binary from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanSemaphore> create_binary(VkDevice device) noexcept;

        /// Creates a timeline from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param initial_value Value consumed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanSemaphore> create_timeline(
            VkDevice device,
            u64 initial_value = 0) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanSemaphore`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSemaphore vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanSemaphore`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the runtime or backend type represented by `VulkanSemaphore`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSemaphoreType type() const noexcept;
        /// Reports whether timeline holds for this `VulkanSemaphore`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_timeline() const noexcept;


        /// Returns the current or globally available counter value value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<u64> counter_value() const noexcept;

        /// Signals the associated synchronization primitive or event.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult signal(u64 value) noexcept;


        /// Waits for the associated operation or synchronization primitive to complete.
        ///
        /// @param value Value consumed by the operation.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult wait(u64 value, u64 timeout_ns = UINT64_MAX) noexcept;


        /// Submits info.
        ///
        /// @param stage `stage` value used by the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSemaphoreSubmitInfo submit_info(VkPipelineStageFlags2 stage, u64 value = 0) const noexcept;

        /// Destroys or releases the `VulkanSemaphore` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        /// Creates a `VulkanSemaphore` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param initial_value Value consumed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanSemaphore> create(
            VkDevice device,
            VkSemaphoreType type,
            u64 initial_value) noexcept;

        VkDevice device_ = VK_NULL_HANDLE;
        VkSemaphore semaphore_ = VK_NULL_HANDLE;
        VkSemaphoreType type_ = VK_SEMAPHORE_TYPE_BINARY;
    };


    class VulkanEvent {
      public:
        /// Constructs a `VulkanEvent` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanEvent() = default;
        /// Destroys the `VulkanEvent` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanEvent();

        /// Disables this construction form for `VulkanEvent`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanEvent(const VulkanEvent &) = delete;
        /// Assigns a new value to this `VulkanEvent`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanEvent &operator=(const VulkanEvent &) = delete;

        /// Constructs a `VulkanEvent` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanEvent(VulkanEvent &&o) noexcept;
        /// Assigns a new value to this `VulkanEvent`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanEvent &operator=(VulkanEvent &&o) noexcept;

        /// Creates a `VulkanEvent` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param device_only Device used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanEvent> create(VkDevice device,
                                                                  bool device_only = true) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanEvent`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkEvent vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanEvent`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;


        /// Reports whether signaled holds for this `VulkanEvent`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<bool> is_signaled() const noexcept;
        /// Returns the current or globally available set value.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult set() noexcept;
        /// Resets the object to its baseline state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult reset() noexcept;

        /// Destroys or releases the `VulkanEvent` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkEvent event_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
