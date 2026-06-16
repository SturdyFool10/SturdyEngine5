#include "../Window/WindowEffects.hpp"
#include "../Window/WindowLog.hpp"
#include "../Window/WindowNative.hpp"

#if defined(_WIN32)
#include <SDL3/SDL.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <dwmapi.h>
#include <windows.h>
#endif

namespace SFT::Platform::Windowing {

namespace {

#if defined(_WIN32)
constexpr DWORD sturdy_dwmwa_use_immersive_dark_mode = 20;
constexpr DWORD sturdy_dwmwa_border_color = 34;
constexpr DWORD sturdy_dwmwa_caption_color = 35;
constexpr DWORD sturdy_dwmwa_text_color = 36;
constexpr DWORD sturdy_dwmwa_system_backdrop_type = 38;

constexpr int sturdy_dwmsbt_none = 1;
constexpr int sturdy_dwmsbt_main_window = 2;
constexpr int sturdy_dwmsbt_transient_window = 3;
constexpr int sturdy_dwmsbt_tabbed_window = 4;

constexpr COLORREF sturdy_dwm_color_default = 0xFFFFFFFF;

[[nodiscard]] COLORREF colorref_from_argb(std::uint32_t color_argb) noexcept
{
    const BYTE red = static_cast<BYTE>((color_argb >> 16U) & 0xFFU);
    const BYTE green = static_cast<BYTE>((color_argb >> 8U) & 0xFFU);
    const BYTE blue = static_cast<BYTE>(color_argb & 0xFFU);
    return RGB(red, green, blue);
}

WindowEffectResult set_dwm_attribute(HWND hwnd, DWORD attribute, const void* value, DWORD value_size, const char* label) noexcept
{
    const HRESULT result = DwmSetWindowAttribute(hwnd, attribute, value, value_size);
    if (FAILED(result)) {
        Detail::window_error("Windows DWM attribute failed: hwnd={} attribute={} label='{}' hresult={}", hwnd, attribute, label, static_cast<long>(result));
        return WindowEffectResult::failed("DwmSetWindowAttribute failed.");
    }

    Detail::window_info("Windows DWM attribute set: hwnd={} attribute={} label='{}'", hwnd, attribute, label);
    return WindowEffectResult::success("Windows DWM attribute applied.");
}

WindowEffectResult set_legacy_blur(HWND hwnd, bool enabled) noexcept
{
    DWM_BLURBEHIND blur {};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = enabled ? TRUE : FALSE;

    const HRESULT result = DwmEnableBlurBehindWindow(hwnd, &blur);
    if (FAILED(result)) {
        Detail::window_error("Windows DWM legacy blur failed: hwnd={} enabled={} hresult={}", hwnd, enabled, static_cast<long>(result));
        return WindowEffectResult::failed("DwmEnableBlurBehindWindow failed.");
    }

    Detail::window_info("Windows DWM legacy blur set: hwnd={} enabled={}", hwnd, enabled);
    return WindowEffectResult::success("Windows legacy DWM blur applied.");
}

WindowEffectResult set_system_backdrop(HWND hwnd, int enabled_backdrop, bool enabled, const char* label) noexcept
{
    const int backdrop = enabled ? enabled_backdrop : sturdy_dwmsbt_none;
    Detail::window_debug("Windows DWM backdrop request: hwnd={} label='{}' enabled={} backdrop={}", hwnd, label, enabled, backdrop);
    WindowEffectResult result = set_dwm_attribute(hwnd, sturdy_dwmwa_system_backdrop_type, &backdrop, sizeof(backdrop), label);
    if (result.succeeded()) {
        return WindowEffectResult::success("Windows system backdrop applied.");
    }

    if (!enabled) {
        return result;
    }

    WindowEffectResult fallback = set_legacy_blur(hwnd, true);
    if (fallback.succeeded()) {
        Detail::window_warn("Windows DWM backdrop degraded to legacy blur: hwnd={} label='{}'", hwnd, label);
        return WindowEffectResult::degraded("Requested Windows backdrop was unavailable; legacy blur was enabled instead.");
    }

    return WindowEffectResult::failed("Requested Windows backdrop failed and legacy blur fallback also failed.");
}
#endif

} // namespace

OperatingSystem current_operating_system() noexcept
{
#if defined(_WIN32)
    return OperatingSystem::Windows;
#else
    return OperatingSystem::Unknown;
#endif
}

bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept
{
#if defined(_WIN32)
    switch (effect) {
    case WindowEffectKind::Blur:
    case WindowEffectKind::Acrylic:
    case WindowEffectKind::Mica:
    case WindowEffectKind::MicaAlt:
    case WindowEffectKind::Tabbed:
    case WindowEffectKind::DarkMode:
    case WindowEffectKind::BorderColor:
    case WindowEffectKind::CaptionColor:
    case WindowEffectKind::TextColor:
        return true;
    }
#else
    (void)effect;
#endif

    return false;
}

WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept
{
#if defined(_WIN32)
    if (handle.system != NativeWindowSystem::Win32 || !handle.window) {
        Detail::window_error(
            "Windows native effect rejected invalid native handle: system={} display={} window={} kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(handle.system),
            handle.display,
            handle.window,
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("Windows window effects require a Win32 HWND.");
    }

    HWND hwnd = static_cast<HWND>(handle.window);
    Detail::window_info(
        "Windows native effect requested: hwnd={} kind={} enabled={} color_argb=0x{:08X}",
        hwnd,
        static_cast<int>(effect.kind),
        effect.enabled,
        effect.color_argb);

    switch (effect.kind) {
    case WindowEffectKind::Blur:
        return set_legacy_blur(hwnd, effect.enabled);
    case WindowEffectKind::Acrylic:
        return set_system_backdrop(hwnd, sturdy_dwmsbt_transient_window, effect.enabled, "Acrylic");
    case WindowEffectKind::Mica:
        return set_system_backdrop(hwnd, sturdy_dwmsbt_main_window, effect.enabled, "Mica");
    case WindowEffectKind::MicaAlt:
    case WindowEffectKind::Tabbed:
        return set_system_backdrop(hwnd, sturdy_dwmsbt_tabbed_window, effect.enabled, "MicaAlt/Tabbed");
    case WindowEffectKind::DarkMode: {
        const BOOL enabled = effect.enabled ? TRUE : FALSE;
        return set_dwm_attribute(hwnd, sturdy_dwmwa_use_immersive_dark_mode, &enabled, sizeof(enabled), "DarkMode");
    }
    case WindowEffectKind::BorderColor: {
        const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
        return set_dwm_attribute(hwnd, sturdy_dwmwa_border_color, &color, sizeof(color), "BorderColor");
    }
    case WindowEffectKind::CaptionColor: {
        const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
        return set_dwm_attribute(hwnd, sturdy_dwmwa_caption_color, &color, sizeof(color), "CaptionColor");
    }
    case WindowEffectKind::TextColor: {
        const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
        return set_dwm_attribute(hwnd, sturdy_dwmwa_text_color, &color, sizeof(color), "TextColor");
    }
    }

    return WindowEffectResult::failed("Unsupported Windows window effect.");
#else
    (void)handle;
    (void)effect;
    return WindowEffectResult::failed("Windows window effects are only available on Windows builds.");
#endif
}

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing::Detail {

NativeWindowHandle native_window_handle_from_glfw(GLFWwindow* window) noexcept
{
#if defined(_WIN32)
    if (!window) {
        Detail::window_error("GLFW Win32 native handle rejected null window.");
        return {};
    }

    NativeWindowHandle handle {NativeWindowSystem::Win32, nullptr, glfwGetWin32Window(window)};
    Detail::window_debug("GLFW Win32 native handle resolved: glfw_window={} hwnd={}", static_cast<void*>(window), handle.window);
    return handle;
#else
    (void)window;
    return {};
#endif
}

NativeWindowHandle native_window_handle_from_sdl(SDL_Window* window) noexcept
{
#if defined(_WIN32)
    if (!window) {
        Detail::window_error("SDL3 Win32 native handle rejected null window.");
        return {};
    }

    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    NativeWindowHandle handle {
        NativeWindowSystem::Win32,
        nullptr,
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr),
    };
    Detail::window_debug("SDL3 Win32 native handle resolved: sdl_window={} properties={} hwnd={}", static_cast<void*>(window), properties, handle.window);
    return handle;
#else
    (void)window;
    return {};
#endif
}

} // namespace SFT::Platform::Windowing::Detail
