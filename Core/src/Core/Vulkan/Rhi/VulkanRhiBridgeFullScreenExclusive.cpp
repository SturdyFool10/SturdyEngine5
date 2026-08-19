

#pragma region Imports
#if defined(_WIN32)


#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <Core/Vulkan/Rhi/VulkanRhiBridgeFullScreenExclusive.hpp>
#pragma endregion

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;

namespace SFT::Core::Vulkan {

#if defined(_WIN32)

    namespace {

        class Win32FullScreenExclusiveRequest final : public FullScreenExclusiveRequest {
          public:
            /// Constructs a `Win32FullScreenExclusiveRequest` from the supplied initialization values.
            ///
            /// @param monitor `monitor` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            explicit Win32FullScreenExclusiveRequest(HMONITOR monitor) noexcept {
                win32_info_ = VkSurfaceFullScreenExclusiveWin32InfoEXT{
                    .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT,
                    .pNext = nullptr,
                    .hmonitor = monitor,
                };


                info_ = VkSurfaceFullScreenExclusiveInfoEXT{
                    .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                    .pNext = &win32_info_,
                    .fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT,
                };
            }

            /// Returns the current or globally available pnext value.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const void *pnext() const noexcept override { return &info_; }

          private:
            VkSurfaceFullScreenExclusiveWin32InfoEXT win32_info_{};
            VkSurfaceFullScreenExclusiveInfoEXT info_{};
        };

    } // namespace

    /// Builds full screen exclusive request.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function does not throw exceptions.
    std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle &surface) noexcept {
        if (surface.system != GraphicsPlatform::WindowSystem::Win32 || surface.window == nullptr) {
            return nullptr;
        }


        HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(surface.window), MONITOR_DEFAULTTONEAREST);
        return std::make_unique<Win32FullScreenExclusiveRequest>(monitor);
    }

    /// Acquires full screen exclusive mode.
    ///
    /// @param device Device used or affected by the operation.
    /// @param swapchain Swapchain used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`, `GraphicsBackendErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    RendererResult acquire_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept {
        if (vkAcquireFullScreenExclusiveModeEXT == nullptr) {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "vkAcquireFullScreenExclusiveModeEXT is not available (VK_EXT_full_screen_exclusive "
                                          "was not enabled).");
        }
        const VkResult result = vkAcquireFullScreenExclusiveModeEXT(device, swapchain);
        if (result != VK_SUCCESS) {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                          "vkAcquireFullScreenExclusiveModeEXT failed — commonly because the window "
                                          "does not currently have focus, or another application already holds "
                                          "exclusive access to the target monitor.");
        }
        return {};
    }

    /// Releases full screen exclusive mode using the supplied arguments and current state.
    ///
    /// @param device Device used or affected by the operation.
    /// @param swapchain Swapchain used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept {
        if (vkReleaseFullScreenExclusiveModeEXT == nullptr) {
            return;
        }
        vkReleaseFullScreenExclusiveModeEXT(device, swapchain);
    }

#else

    /// Builds full screen exclusive request.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function does not throw exceptions.
    std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle &            ) noexcept {
        return nullptr;
    }

    /// Acquires full screen exclusive mode.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    RendererResult acquire_full_screen_exclusive_mode(VkDevice           , VkSwapchainKHR              ) noexcept {
        return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                      "VK_EXT_full_screen_exclusive is implemented only on Windows.");
    }

    /// Releases full screen exclusive mode using the supplied arguments and current state.
    ///
    /// @note This function does not throw exceptions.
    void release_full_screen_exclusive_mode(VkDevice           , VkSwapchainKHR              ) noexcept {
    }

#endif // defined(_WIN32)

} // namespace SFT::Core::Vulkan
