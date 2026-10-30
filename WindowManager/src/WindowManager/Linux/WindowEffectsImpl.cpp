#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstdint>
#include <expected>

#if defined(__linux__)
#include <SDL3/SDL.h>
#endif
#pragma endregion

#include <WindowManager/Linux/WaylandBackgroundEffects.hpp>
#include <WindowManager/Linux/X11BlurProperty.hpp>
#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>
#include <WindowManager/WindowNative.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::unexpected;

using std::uintptr_t;

namespace SFT::WindowManager {

    namespace {

#if defined(__linux__)
        /// Attempts to ext background effect blur without requiring normal failure to be exceptional.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
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

        /// Attempts to kde blur without requiring normal failure to be exceptional.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
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

        /// Attempts to linux blur without requiring normal failure to be exceptional.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        WindowEffectResult try_linux_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            ZoneScopedN("Windowing::try_linux_blur");
            // X11 has exactly one route, whatever protocol was asked for: the KDE blur property.
            if (handle.system == NativeWindowSystem::X11) {
                if (effect.linux_blur_protocol == LinuxBlurProtocol::ExtBackgroundEffect ||
                    effect.linux_blur_protocol == LinuxBlurProtocol::KdeBlur) {
                    return WindowEffectResult::failed("The Wayland blur protocols do not apply to an X11 window.");
                }
                return Detail::set_x11_kde_blur(handle, effect);
            }
            switch (effect.linux_blur_protocol) {
                case LinuxBlurProtocol::ExtBackgroundEffect:
                    return try_ext_background_effect_blur(handle, effect);
                case LinuxBlurProtocol::KdeBlur:
                    return try_kde_blur(handle, effect);
                case LinuxBlurProtocol::KdeX11:
                case LinuxBlurProtocol::Automatic:
                    break;
            }


            return Detail::set_wayland_background_blur(handle, effect);
        }
#endif

    } // namespace

    /// Returns the current or globally available current operating system value.
    ///
    /// @return Returns the current current operating system value.
    /// @note This function does not throw exceptions.
    OperatingSystem current_operating_system() noexcept {
#if defined(__linux__)
        return OperatingSystem::Linux;
#else
        return OperatingSystem::Unknown;
#endif
    }

    /// Performs the operating system may support window effect operation for `Windowing` using the supplied arguments.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
#if defined(__linux__)
        return window_effect_supported_on_platform(effect);
#else
        (void)effect;
        return false;
#endif
    }

    /// Releases native window effects using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param release_display `release_display` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_native_window_effects(NativeWindowHandle handle, bool release_display) noexcept {
#if defined(__linux__)
        Detail::release_wayland_background_effects(handle, release_display);
#else
        (void)handle;
        (void)release_display;
#endif
    }

    /// Enables native window effect using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    WindowEffectSupport query_native_window_effect_support(NativeWindowHandle handle, WindowEffectKind kind) noexcept {
        WindowEffectSupport support;
#if defined(__linux__)
        if (kind == WindowEffectKind::Opacity) {
            if (handle.system == NativeWindowSystem::Wayland) {
                return Detail::query_wayland_opacity_support(handle);
            }
            support.add("_NET_WM_WINDOW_OPACITY", handle.system == NativeWindowSystem::X11 ? EffectAvailability::Unknown
                                                                                          : EffectAvailability::Unavailable,
                        "X11 cannot report whether the compositor reads this property");
            return support;
        }
        if (kind == WindowEffectKind::MousePassthrough) {
            support.add(handle.system == NativeWindowSystem::Wayland ? "wl_surface input region" : "X Shape input region",
                        handle.system == NativeWindowSystem::Wayland || handle.system == NativeWindowSystem::X11
                            ? EffectAvailability::Available
                            : EffectAvailability::Unavailable,
                        "the window is neither a Wayland nor an X11 window");
            return support;
        }
        if (kind == WindowEffectKind::Focusable || kind == WindowEffectKind::KeyboardGrab) {
            if (handle.system == NativeWindowSystem::X11) {
                support.add(kind == WindowEffectKind::Focusable ? "WM_HINTS input flag" : "XGrabKeyboard", EffectAvailability::Unknown,
                            "X11 cannot report whether the window manager honours this");
            } else {
                support.add("none", EffectAvailability::Unavailable, "Wayland compositors own focus and keyboard routing");
            }
            return support;
        }
        if (kind == WindowEffectKind::CornerRadius) {
            if (handle.system == NativeWindowSystem::X11) {
                support.add("X Shape extension", EffectAvailability::Available, "hard-edged; the compositor may still draw its own shadow");
            } else {
                support.add("none", EffectAvailability::Unavailable,
                            "Wayland compositors own the window outline; use a transparent window and draw the corners");
            }
            return support;
        }
        if (kind == WindowEffectKind::DarkMode) {
            support.add("_GTK_THEME_VARIANT", handle.system == NativeWindowSystem::X11 ? EffectAvailability::Unknown
                                                                                      : EffectAvailability::Unavailable,
                        "X11 only; Wayland has no per-window theme request");
            return support;
        }
        if (kind != WindowEffectKind::Blur) {
            support.add("none", EffectAvailability::Unavailable,
                        "Linux offers background blur, opacity and (X11) dark decorations; transparency is a creation-time window flag");
            return support;
        }
        if (handle.system == NativeWindowSystem::Wayland) {
            return Detail::query_wayland_background_support(handle);
        }
        if (handle.system == NativeWindowSystem::X11) {
            return Detail::query_x11_blur_support(handle);
        }
        support.add("none", EffectAvailability::Unavailable, "the window is neither a Wayland nor an X11 window");
#else
        (void)handle;
        (void)kind;
        support.add("none", EffectAvailability::Unavailable, "not a Linux build");
#endif
        return support;
    }

    WindowEffectResult set_native_window_type(NativeWindowHandle handle, WindowType type) noexcept {
#if defined(__linux__)
        return handle.system == NativeWindowSystem::X11
                   ? Detail::set_x11_window_type(handle, type)
                   : WindowEffectResult::failed("Wayland leaves a window's role to the compositor and the toplevel's parent.");
#else
        (void)handle;
        (void)type;
        return WindowEffectResult::failed("Linux window types are only available on Linux builds.");
#endif
    }

    WindowEffectResult set_native_window_shape(NativeWindowHandle handle, const WindowShapeMask &mask, WindowExtent pixel_size) noexcept {
#if defined(__linux__)
        return handle.system == NativeWindowSystem::X11 ? Detail::set_x11_window_shape(handle, mask, pixel_size)
                                                        : WindowEffectResult::failed("Wayland has no client-side window outline.");
#else
        (void)handle;
        (void)mask;
        (void)pixel_size;
        return WindowEffectResult::failed("Linux window shapes are only available on Linux builds.");
#endif
    }

    WindowEffectResult set_native_window_parent(NativeWindowHandle child, std::optional<NativeWindowHandle> parent, bool modal) noexcept {
#if defined(__linux__)
        if (child.system == NativeWindowSystem::X11) {
            return Detail::set_x11_window_parent(child, parent, modal);
        }
        return WindowEffectResult::failed("Wayland ties a toplevel to its parent through xdg_toplevel, which the window provider owns.");
#else
        (void)child;
        (void)parent;
        (void)modal;
        return WindowEffectResult::failed("Linux window parenting is only available on Linux builds.");
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

        if (effect.kind == WindowEffectKind::Opacity) {
            return handle.system == NativeWindowSystem::Wayland ? Detail::set_wayland_window_opacity(handle, effect)
                                                                : Detail::set_x11_window_opacity(handle, effect);
        }
        if (effect.kind == WindowEffectKind::MousePassthrough) {
            return handle.system == NativeWindowSystem::Wayland ? Detail::set_wayland_mouse_passthrough(handle, effect.enabled)
                                                                : Detail::set_x11_mouse_passthrough(handle, effect.enabled);
        }
        if (effect.kind == WindowEffectKind::Focusable) {
            return handle.system == NativeWindowSystem::X11
                       ? Detail::set_x11_focusable(handle, effect.enabled)
                       : WindowEffectResult::failed("Wayland compositors decide focus; a client cannot refuse it.");
        }
        if (effect.kind == WindowEffectKind::KeyboardGrab) {
            return handle.system == NativeWindowSystem::X11
                       ? Detail::set_x11_keyboard_grab(handle, effect.enabled)
                       : WindowEffectResult::failed("Wayland has no keyboard grab (keyboard-shortcuts-inhibit is a compositor opt-in).");
        }
        if (effect.kind == WindowEffectKind::CornerRadius) {
            return handle.system == NativeWindowSystem::X11
                       ? Detail::set_x11_corner_radius(handle, effect)
                       : WindowEffectResult::failed(
                             "Wayland has no client request for a window outline; draw rounded corners into a transparent window instead.");
        }
        if (effect.kind == WindowEffectKind::DarkMode) {
            return handle.system == NativeWindowSystem::X11
                       ? Detail::set_x11_dark_mode(handle, effect.enabled)
                       : WindowEffectResult::failed("Wayland has no per-window dark-mode request.");
        }

        return WindowEffectResult::failed("This window effect is not modeled for Linux.");
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

} // namespace SFT::WindowManager

namespace SFT::WindowManager::Detail {


    /// Performs the native window handle from SDL operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
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

} // namespace SFT::WindowManager::Detail
