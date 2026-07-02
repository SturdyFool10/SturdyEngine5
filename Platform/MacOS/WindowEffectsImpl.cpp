module;

#include <expected>
#if defined(__APPLE__)
#include <SDL3/SDL.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

module Sturdy.Platform;

import Sturdy.Foundation;
import :WindowError;
import :WindowConfig;
import :WindowEffect;
import :WindowEffects;
import :WindowLog;
import :WindowNative;

using std::expected;
using std::unexpected;

namespace SFT::Platform::Windowing {

    OperatingSystem current_operating_system() noexcept {
#if defined(__APPLE__)
        return OperatingSystem::MacOS;
#else
        return OperatingSystem::Unknown;
#endif
    }

    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
        (void)effect;
        return false;
    }

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
#if defined(__APPLE__)
        Detail::window_warn(
            "macOS native window effect requested but unsupported: system={} display={} window={} kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(handle.system),
            handle.display,
            handle.window,
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("macOS window effects are not implemented yet.");
#else
        (void)handle;
        Detail::window_warn(
            "macOS window effect implementation called on non-Apple build: kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("macOS window effects are only available on Apple builds.");
#endif
    }

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing::Detail {

    expected<NativeWindowHandle, WindowError> native_window_handle_from_glfw(void *window_handle) noexcept {
#if defined(__APPLE__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("GLFW Cocoa native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle requires a live window."});
        }

        NativeWindowHandle handle{NativeWindowSystem::Cocoa, nullptr, glfwGetCocoaWindow(window)};
        if (!handle.window) [[unlikely]] {
            Detail::window_error("GLFW Cocoa native handle missing NSWindow: glfw_window={}", static_cast<void *>(window));
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle is incomplete."});
        }
        Detail::window_debug("GLFW Cocoa native handle resolved: glfw_window={} ns_window={}", static_cast<void *>(window), handle.window);
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Cocoa native handles are only available on Apple builds."});
#endif
    }

    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
#if defined(__APPLE__)
        auto *window = static_cast<SDL_Window *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("SDL3 Cocoa native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Cocoa native handle requires a live window."});
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        NativeWindowHandle handle{
            NativeWindowSystem::Cocoa,
            nullptr,
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr),
        };
        if (!handle.window) [[unlikely]] {
            Detail::window_error("SDL3 Cocoa native handle missing NSWindow: sdl_window={} properties={}", static_cast<void *>(window), properties);
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Cocoa native handle is incomplete."});
        }
        Detail::window_debug("SDL3 Cocoa native handle resolved: sdl_window={} properties={} ns_window={}", static_cast<void *>(window), properties, handle.window);
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "SDL3 Cocoa native handles are only available on Apple builds."});
#endif
    }

} // namespace SFT::Platform::Windowing::Detail
