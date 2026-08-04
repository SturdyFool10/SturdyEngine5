#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#if defined(__APPLE__)
#include <SDL3/SDL.h>
#endif
#pragma endregion

#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowEffect.hpp>
#include <Platform/Window/WindowEffects.hpp>
#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/WindowNative.hpp>

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
        (void)handle;
        if (!operating_system_may_support_window_effect(effect.kind)) [[likely]] {
            Detail::window_warn(
                "macOS window effect kind={} enabled={} is not supported on this OS build; no-op. "
                "(NSWindow supports live transparency toggling in principle, but this engine has no "
                "Objective-C++ build support yet — not implemented.)",
                static_cast<int>(effect.kind),
                effect.enabled);
            return WindowEffectResult::failed("This window effect is not supported on the current OS.");
        }
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
