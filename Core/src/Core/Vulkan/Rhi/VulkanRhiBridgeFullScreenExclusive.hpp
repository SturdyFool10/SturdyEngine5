#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <memory>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

#include <graphicsPlatform/src/CompositionPresent.hpp>

using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {

    // VK_EXT_full_screen_exclusive support — application-controlled exclusive fullscreen, which
    // bypasses the OS compositor entirely for the lowest achievable presentation latency, unlike a
    // borderless window (which still goes through DWM/the compositor even when it covers the whole
    // display). Same two-file, internally-guarded split as VulkanRhiBridgeComposition.hpp/.cpp and
    // graphicsPlatform's CompositionPresent.hpp/.cpp: every declaration here is portable (no Win32
    // types), so call sites never need their own `#if defined(_WIN32)`.
    //
    // The extension's own structs (VkSurfaceFullScreenExclusiveInfoEXT, ...) and functions
    // (vkAcquireFullScreenExclusiveModeEXT, ...) live entirely inside vulkan_win32.h in this
    // codebase's vendored Vulkan headers — gated behind VK_USE_PLATFORM_WIN32_KHR the same way the
    // rest of vulkan_win32.h is — so they can't be named directly in a portable header. This is why
    // FullScreenExclusiveRequest below is an opaque interface: its `pnext()` accessor hands back a
    // plain `const void *` the caller (VulkanRhiBridgeSwapchain.cpp, which also needs to stay
    // platform-neutral — it already carries Linux's Xlib/Xcb/Wayland headers) can assign straight to
    // VkSwapchainCreateInfoKHR::pNext without ever seeing the Win32-specific struct types.

    // Owns the pNext chain a swapchain create needs to request exclusive mode. Must stay alive for
    // the duration of the vkCreateSwapchainKHR call it's used in (the chain is pointers into this
    // object), and can be discarded immediately afterward — nothing about acquiring or holding
    // exclusive mode itself depends on this object surviving past that one call.
    class FullScreenExclusiveRequest {
      public:
        virtual ~FullScreenExclusiveRequest() = default;
        FullScreenExclusiveRequest(const FullScreenExclusiveRequest &) = delete;
        FullScreenExclusiveRequest &operator=(const FullScreenExclusiveRequest &) = delete;

        [[nodiscard]] virtual const void *pnext() const noexcept = 0;

      protected:
        FullScreenExclusiveRequest() = default;
    };

    // Builds the pNext chain for VkSwapchainCreateInfoKHR requesting VK_EXT_full_screen_exclusive's
    // application-controlled mode for `surface`'s window. Returns nullptr — never a partial or
    // best-effort chain — on any platform/configuration that can't do this (non-Windows, or an
    // invalid/non-Win32 surface), so the caller's fallback is simply "don't attach anything and
    // create an ordinary swapchain," exactly as if exclusive mode had never been requested.
    [[nodiscard]] std::unique_ptr<FullScreenExclusiveRequest> build_full_screen_exclusive_request(
        const GraphicsPlatform::NativeSurfaceHandle &surface) noexcept;

    // Acquires exclusive mode on an already-created swapchain (VkSwapchainCreateInfoKHR::pNext must
    // have carried a FullScreenExclusiveRequest's chain for this to have any effect — acquiring
    // without having requested it at creation time is invalid per spec). Failure here is an ordinary,
    // expected outcome (the window doesn't currently have focus, another app holds exclusive access,
    // ...) — return it to the caller to log/ignore, never treat it as fatal: the swapchain remains
    // perfectly usable non-exclusively either way.
    [[nodiscard]] RendererResult acquire_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept;

    // Releases exclusive mode before a swapchain that actually holds it is destroyed or rebuilt
    // without it. Callers must only call this for a swapchain whose matching
    // acquire_full_screen_exclusive_mode() call actually returned success — releasing on one that
    // never acquired (or was never created with the extension's pNext chain at all) is undefined per
    // spec, not a safe no-op, so this is not a "call unconditionally, just in case" helper.
    void release_full_screen_exclusive_mode(VkDevice device, VkSwapchainKHR swapchain) noexcept;

} // namespace SFT::Core::Vulkan
