#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <memory>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

#include <Core/GraphicsPlatform/CompositionPresent.hpp>

using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {


    class FullScreenExclusiveRequest {
      public:
        /// Destroys the `FullScreenExclusiveRequest` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~FullScreenExclusiveRequest() = default;
        /// Disables this construction form for `FullScreenExclusiveRequest`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        FullScreenExclusiveRequest(const FullScreenExclusiveRequest &) = delete;
        /// Assigns a new value to this `FullScreenExclusiveRequest`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        FullScreenExclusiveRequest &operator=(const FullScreenExclusiveRequest &) = delete;

        /// Returns the current or globally available pnext value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const void *pnext() const noexcept = 0;

      protected:
        /// Constructs a `FullScreenExclusiveRequest` in its default state.
        ///
        /// @note This function does not throw exceptions.
        FullScreenExclusiveRequest() = default;
    };


    /// Builds full screen exclusive request.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle &surface) noexcept;


    /// Acquires full screen exclusive mode.
    ///
    /// @param device Device used or affected by the operation.
    /// @param swapchain Swapchain used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RendererResult acquire_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept;


    /// Releases full screen exclusive mode using the supplied arguments and current state.
    ///
    /// @param device Device used or affected by the operation.
    /// @param swapchain Swapchain used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept;

} // namespace SFT::Core::Vulkan
