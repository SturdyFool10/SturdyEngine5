// Vulkan-side VK_EXT_full_screen_exclusive support — see
// VulkanRhiBridgeFullScreenExclusive.hpp for the seam this implements and why. Same two-file,
// internally-guarded split as VulkanRhiBridgeComposition.cpp, so call sites never need their own
// `#if defined(_WIN32)`.

#pragma region Imports
#if defined(_WIN32)
// The extension's structs/functions live entirely inside vulkan_win32.h — see the header's own doc
// comment for why. VulkanRhiBridgeFullScreenExclusive.hpp pulls in "volk.h" itself (needed on every
// platform for the plain portable declarations it exposes), so this define has to land before that
// header's own #include, same ordering requirement as VulkanRhiBridgeComposition.cpp. Local to this
// TU only (volk.h's include guard means no other file including volk.h is affected).
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "VulkanRhiBridgeFullScreenExclusive.hpp"
#pragma endregion

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;

namespace SFT::Core::Vulkan {

#if defined(_WIN32)

    namespace {

        class Win32FullScreenExclusiveRequest final : public FullScreenExclusiveRequest {
          public:
            explicit Win32FullScreenExclusiveRequest(HMONITOR monitor) noexcept {
                win32_info_ = VkSurfaceFullScreenExclusiveWin32InfoEXT{
                    .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT,
                    .pNext = nullptr,
                    .hmonitor = monitor,
                };
                // Application-controlled, not the platform-default "let the driver decide" mode: the
                // whole point of exposing this as an explicit engine feature is that the app (via the
                // window's WindowMode) decides when exclusivity is wanted, not the driver guessing
                // from swapchain state.
                info_ = VkSurfaceFullScreenExclusiveInfoEXT{
                    .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                    .pNext = &win32_info_,
                    .fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT,
                };
            }

            [[nodiscard]] const void *pnext() const noexcept override { return &info_; }

          private:
            VkSurfaceFullScreenExclusiveWin32InfoEXT win32_info_{};
            VkSurfaceFullScreenExclusiveInfoEXT info_{};
        };

    } // namespace

    std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle &surface) noexcept {
        if (surface.system != GraphicsPlatform::WindowSystem::Win32 || surface.window == nullptr) {
            return nullptr;
        }
        // The extension keys exclusivity to a specific *monitor*, not the window — MonitorFromWindow
        // with MONITOR_DEFAULTTONEAREST always returns a real monitor for a valid HWND (never null),
        // matching how a fullscreen window is already positioned to cover exactly one display.
        HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(surface.window), MONITOR_DEFAULTTONEAREST);
        return std::make_unique<Win32FullScreenExclusiveRequest>(monitor);
    }

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

    void release_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept {
        if (vkReleaseFullScreenExclusiveModeEXT == nullptr) {
            return;
        }
        vkReleaseFullScreenExclusiveModeEXT(device, swapchain);
    }

#else // !defined(_WIN32)

    std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle & /*surface*/) noexcept {
        return nullptr;
    }

    RendererResult acquire_full_screen_exclusive_mode(VkDevice /*device*/, VkSwapchainKHR /*swapchain*/) noexcept {
        return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                      "VK_EXT_full_screen_exclusive is implemented only on Windows.");
    }

    void release_full_screen_exclusive_mode(VkDevice /*device*/, VkSwapchainKHR /*swapchain*/) noexcept {
    }

#endif // defined(_WIN32)

} // namespace SFT::Core::Vulkan
