#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstdint>
#include <expected>

#if defined(__linux__)
#include <SDL3/SDL.h>
#endif
#pragma endregion

#include <Platform/Linux/WaylandBackgroundEffects.hpp>
#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowEffect.hpp>
#include <Platform/Window/WindowEffects.hpp>
#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/WindowNative.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::unexpected;

using std::uintptr_t;

namespace SFT::Platform::Windowing {

    namespace {

#if defined(__linux__)
        WindowEffectResult try_ext_background_effect_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            ZoneScopedN("Windowing::try_ext_background_effect_blur");
            Detail::window_debug(
                "Linux ext-background-effect-v1 blur attempt: system={} display={} window={} enabled={}",
                static_cast<int>(handle.system),
                handle.display,
                handle.window,
                effect.enabled);

            effect.linux_blur_protocol = LinuxBlurProtocol::ExtBackgroundEffect;
            return Detail::set_wayland_background_blur(handle, effect);
        }

        WindowEffectResult try_kde_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            ZoneScopedN("Windowing::try_kde_blur");
            Detail::window_debug(
                "Linux KDE blur attempt: system={} display={} window={} enabled={}",
                static_cast<int>(handle.system),
                handle.display,
                handle.window,
                effect.enabled);

            effect.linux_blur_protocol = LinuxBlurProtocol::KdeBlur;
            return Detail::set_wayland_background_blur(handle, effect);
        }

        WindowEffectResult try_linux_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            ZoneScopedN("Windowing::try_linux_blur");
            switch (effect.linux_blur_protocol) {
                case LinuxBlurProtocol::ExtBackgroundEffect:
                    return try_ext_background_effect_blur(handle, effect);
                case LinuxBlurProtocol::KdeBlur:
                    return try_kde_blur(handle, effect);
                case LinuxBlurProtocol::Automatic:
                    break;
            }

            // Keep automatic routing in the shared backend so enable and disable operate on the same
            // per-surface state. In particular, disabling must remove a KDE fallback object rather than
            // returning early after a no-op ext-background-effect disable.
            return Detail::set_wayland_background_blur(handle, effect);
        }
#endif

    } // namespace

    OperatingSystem current_operating_system() noexcept {
#if defined(__linux__)
        return OperatingSystem::Linux;
#else
        return OperatingSystem::Unknown;
#endif
    }

    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
#if defined(__linux__)
        return effect == WindowEffectKind::Blur;
#else
        (void)effect;
        return false;
#endif
    }

    void release_native_window_effects(NativeWindowHandle handle, bool release_display) noexcept {
#if defined(__linux__)
        Detail::release_wayland_background_effects(handle, release_display);
#else
        (void)handle;
        (void)release_display;
#endif
    }

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::enable_native_window_effect");
#if defined(__linux__)
        if (!operating_system_may_support_window_effect(effect.kind)) [[unlikely]] {
            Detail::window_warn(
                "Linux window effect kind={} enabled={} is not supported on this OS; no-op. "
                "(Window transparency on Linux is a creation-time-only WindowConfig::transparent flag — "
                "see its doc comment for why — not something Window::set_transparent() can toggle live.)",
                static_cast<int>(effect.kind),
                effect.enabled);
            return WindowEffectResult::failed("This window effect is not supported on the current OS.");
        }

        if (effect.kind == WindowEffectKind::Blur) [[likely]] {
            return try_linux_blur(handle, effect);
        }

        return WindowEffectResult::failed("Only blur is currently modeled for Linux window effects.");
#else
        (void)handle;
        Detail::window_warn(
            "Linux window effect implementation called on non-Linux build: kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("Linux window effects are only available on Linux builds.");
#endif
    }

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing::Detail {


    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
        ZoneScopedN("Windowing::Detail::native_window_handle_from_sdl");
#if defined(__linux__)
        auto *window = static_cast<SDL_Window *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("SDL3 Linux native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Linux native handle requires a live window."});
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        if (void *wayland_display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
            NativeWindowHandle handle{
                NativeWindowSystem::Wayland,
                wayland_display,
                SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr),
            };
            if (!handle.window) [[unlikely]] {
                Detail::window_error("SDL3 Linux Wayland native handle missing surface: sdl_window={} properties={} display={} window={}", static_cast<void *>(window), properties, handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Wayland native handle is incomplete."});
            }
            Detail::window_debug("SDL3 Linux native handle resolved Wayland: sdl_window={} properties={} display={} window={}", static_cast<void *>(window), properties, handle.display, handle.window);
            return handle;
        }

        if (void *x11_display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
            NativeWindowHandle handle{
                NativeWindowSystem::X11,
                x11_display,
                reinterpret_cast<void *>(static_cast<uintptr_t>(
                    SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0))),
            };
            if (!handle.window) [[unlikely]] {
                Detail::window_error("SDL3 Linux X11 native handle missing window: sdl_window={} properties={} display={} window={}", static_cast<void *>(window), properties, handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 X11 native handle is incomplete."});
            }
            Detail::window_debug("SDL3 Linux native handle resolved X11: sdl_window={} properties={} display={} window={}", static_cast<void *>(window), properties, handle.display, handle.window);
            return handle;
        }
        Detail::window_warn("SDL3 Linux native handle unresolved: sdl_window={} properties={}", static_cast<void *>(window), properties);
        return unexpected(WindowError{WindowErrorCode::Unsupported, "SDL3 Linux native handle platform is unsupported."});
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "SDL3 Linux native handles are only available on Linux builds."});
#endif
    }

} // namespace SFT::Platform::Windowing::Detail
