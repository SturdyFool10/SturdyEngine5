#include "../Window/WindowEffects.hpp"
#include "../Window/WindowLog.hpp"
#include "../Window/WindowNative.hpp"

#include <cstdint>

#if defined(__linux__)
#include <SDL3/SDL.h>

#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(Success)
#undef Success
#endif
#endif

using std::uintptr_t;

namespace SFT::Platform::Windowing {

    namespace {

#if defined(__linux__)
        WindowEffectResult try_ext_background_effect_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            Detail::window_debug(
                "Linux ext-background-effect-v1 blur attempt: system={} display={} window={} enabled={}",
                static_cast<int>(handle.system),
                handle.display,
                handle.window,
                effect.enabled);

            if (handle.system != NativeWindowSystem::Wayland || !handle.display || !handle.window) {
                return WindowEffectResult::failed("ext-background-effect-v1 requires a Wayland display and wl_surface.");
            }

            return WindowEffectResult::failed("ext-background-effect-v1 support is not wired yet; generated Wayland protocol bindings are required.");
        }

        WindowEffectResult try_kde_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            Detail::window_debug(
                "Linux KDE blur attempt: system={} display={} window={} enabled={}",
                static_cast<int>(handle.system),
                handle.display,
                handle.window,
                effect.enabled);

            if (handle.system != NativeWindowSystem::Wayland || !handle.display || !handle.window) {
                return WindowEffectResult::failed("KDE blur requires a Wayland display and wl_surface.");
            }

            return WindowEffectResult::failed("KDE blur support is not wired yet; generated org_kde_kwin_blur protocol bindings are required.");
        }

        WindowEffectResult try_linux_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
            switch (effect.linux_blur_protocol) {
                case LinuxBlurProtocol::ExtBackgroundEffect:
                    return try_ext_background_effect_blur(handle, effect);
                case LinuxBlurProtocol::KdeBlur:
                    return try_kde_blur(handle, effect);
                case LinuxBlurProtocol::Automatic:
                    break;
            }

            WindowEffectResult ext_result = try_ext_background_effect_blur(handle, effect);
            if (ext_result.kind == WindowEffectResultKind::Success) {
                return ext_result;
            }

            Detail::window_warn(
                "Linux ext-background-effect-v1 blur did not fully apply; trying KDE blur fallback: ext_kind={} ext_details='{}'",
                static_cast<int>(ext_result.kind),
                ext_result.details);

            WindowEffectResult kde_result = try_kde_blur(handle, effect);
            if (kde_result.succeeded()) {
                return WindowEffectResult::degraded("ext-background-effect-v1 was unavailable or degraded; KDE blur fallback applied.");
            }

            Detail::window_error(
                "Linux blur failed on both ext-background-effect-v1 and KDE blur paths: ext_details='{}' kde_details='{}'",
                ext_result.details,
                kde_result.details);
            return WindowEffectResult::failed("Linux blur failed: ext-background-effect-v1 did not apply and KDE blur fallback failed.");
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

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
#if defined(__linux__)
        Detail::window_warn(
            "Linux native window effect requested but unsupported: system={} display={} window={} kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(handle.system),
            handle.display,
            handle.window,
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);

        if (effect.kind == WindowEffectKind::Blur) {
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

    NativeWindowHandle native_window_handle_from_glfw(GLFWwindow *window) noexcept {
#if defined(__linux__)
        if (!window) {
            Detail::window_error("GLFW Linux native handle rejected null window.");
            return {};
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            NativeWindowHandle handle{NativeWindowSystem::Wayland, glfwGetWaylandDisplay(), glfwGetWaylandWindow(window)};
            Detail::window_debug("GLFW Linux native handle resolved Wayland: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
            return handle;
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            NativeWindowHandle handle{
                NativeWindowSystem::X11,
                glfwGetX11Display(),
                reinterpret_cast<void *>(static_cast<uintptr_t>(glfwGetX11Window(window))),
            };
            Detail::window_debug("GLFW Linux native handle resolved X11: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
            return handle;
        }
        Detail::window_warn("GLFW Linux native handle unresolved platform: glfw_window={} glfw_platform={}", static_cast<void *>(window), glfwGetPlatform());
#else
        (void)window;
#endif

        return {};
    }

    NativeWindowHandle native_window_handle_from_sdl(SDL_Window *window) noexcept {
#if defined(__linux__)
        if (!window) {
            Detail::window_error("SDL3 Linux native handle rejected null window.");
            return {};
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        if (void *wayland_display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
            NativeWindowHandle handle{
                NativeWindowSystem::Wayland,
                wayland_display,
                SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr),
            };
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
            Detail::window_debug("SDL3 Linux native handle resolved X11: sdl_window={} properties={} display={} window={}", static_cast<void *>(window), properties, handle.display, handle.window);
            return handle;
        }
        Detail::window_warn("SDL3 Linux native handle unresolved: sdl_window={} properties={}", static_cast<void *>(window), properties);
#else
        (void)window;
#endif

        return {};
    }

} // namespace SFT::Platform::Windowing::Detail
