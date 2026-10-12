#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cmath>
#include <expected>
#include <vector>
#if defined(_WIN32)
#include <SDL3/SDL.h>

#include <dwmapi.h>
#include <windows.h>
#endif
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>
#include <WindowManager/WindowNative.hpp>

using std::expected;
using std::unexpected;

namespace SFT::WindowManager {

    namespace {

#if defined(_WIN32)
        constexpr DWORD sturdy_dwmwa_use_immersive_dark_mode = 20;
        constexpr DWORD sturdy_dwmwa_border_color = 34;
        constexpr DWORD sturdy_dwmwa_caption_color = 35;
        constexpr DWORD sturdy_dwmwa_text_color = 36;
        constexpr DWORD sturdy_dwmwa_system_backdrop_type = 38;

        constexpr DWORD sturdy_dwmwa_window_corner_preference = 33;

        // Windows build numbers that introduced each facility (docs: DWMWINDOWATTRIBUTE and community-documented
        // accent policy). System backdrops (`DWMWA_SYSTEMBACKDROP_TYPE`) shipped in Windows 11 22H2.
        constexpr DWORD sturdy_build_win10 = 10240;
        constexpr DWORD sturdy_build_dark_mode = 19041;
        constexpr DWORD sturdy_build_accent_acrylic = 17134;
        constexpr DWORD sturdy_build_win11 = 22000;
        constexpr DWORD sturdy_build_system_backdrop = 22621;

        constexpr int sturdy_dwmsbt_auto = 0;
        constexpr int sturdy_dwmsbt_none = 1;
        constexpr int sturdy_dwmsbt_main_window = 2;
        constexpr int sturdy_dwmsbt_transient_window = 3;
        constexpr int sturdy_dwmsbt_tabbed_window = 4;

        constexpr COLORREF sturdy_dwm_color_default = 0xFFFFFFFF;

        /// Performs the colorref from argb operation for `Windowing` using the supplied arguments.
        ///
        /// @param color_argb `color_argb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] COLORREF colorref_from_argb(u32 color_argb) noexcept {
            const BYTE red = static_cast<BYTE>((color_argb >> 16U) & 0xFFU);
            const BYTE green = static_cast<BYTE>((color_argb >> 8U) & 0xFFU);
            const BYTE blue = static_cast<BYTE>(color_argb & 0xFFU);
            return RGB(red, green, blue);
        }

        /// Logs hwnd using the supplied arguments and current state.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const void *log_hwnd(HWND hwnd) noexcept {
            return static_cast<const void *>(hwnd);
        }

        /// Sets the dwm attribute for this `Windowing`.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param attribute `attribute` value used by the operation.
        /// @param value Value consumed by the operation.
        /// @param value_size Requested or available size for the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        WindowEffectResult set_dwm_attribute(HWND hwnd, DWORD attribute, const void *value, DWORD value_size, const char *label) noexcept {
            const HRESULT result = DwmSetWindowAttribute(hwnd, attribute, value, value_size);
            if (FAILED(result)) [[unlikely]] {
                Detail::window_error("Windows DWM attribute failed: hwnd={} attribute={} label='{}' hresult={}", log_hwnd(hwnd), attribute, label, static_cast<long>(result));
                return WindowEffectResult::failed("DwmSetWindowAttribute failed.");
            }

            Detail::window_info("Windows DWM attribute set: hwnd={} attribute={} label='{}'", log_hwnd(hwnd), attribute, label);
            return WindowEffectResult::success("Windows DWM attribute applied.");
        }

        /// Sets the legacy blur for this `Windowing`.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        /// The running Windows build number (RtlGetVersion is not subject to the manifest-based version lie of
        /// GetVersionEx). 0 when it cannot be read.
        [[nodiscard]] DWORD windows_build_number() noexcept {
            static const DWORD build = [] {
                using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
                const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                if (ntdll == nullptr) {
                    return DWORD{0};
                }
                const auto get_version = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")));
                if (get_version == nullptr) {
                    return DWORD{0};
                }
                OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                return get_version(&info) == 0 ? info.dwBuildNumber : DWORD{0};
            }();
            return build;
        }

        /// Builds a GDI region from the blur rectangles (device pixels). The caller owns (and must delete) it.
        [[nodiscard]] HRGN make_blur_region(const WindowEffectOptions &options) noexcept {
            const std::vector<EffectRect> rects = resolve_blur_region(options);
            if (rects.empty()) {
                return nullptr; // null = the whole window
            }
            const f32 scale = options.pixel_scale > 0.0f ? options.pixel_scale : 1.0f;
            HRGN combined = CreateRectRgn(0, 0, 0, 0);
            for (const EffectRect &rect : rects) {
                const int left = static_cast<int>(std::lround(static_cast<f32>(rect.x) * scale));
                const int top = static_cast<int>(std::lround(static_cast<f32>(rect.y) * scale));
                const int right = static_cast<int>(std::lround(static_cast<f32>(rect.x + rect.width) * scale));
                const int bottom = static_cast<int>(std::lround(static_cast<f32>(rect.y + rect.height) * scale));
                HRGN piece = CreateRectRgn(left, top, right, bottom);
                CombineRgn(combined, combined, piece, RGN_OR);
                DeleteObject(piece);
            }
            return combined;
        }

        // `SetWindowCompositionAttribute` is an undocumented user32 export (the accent policy Windows 10 shell and
        // Terminal use); the layout below is the community-documented one. Resolved at run time, so a Windows
        // without it simply reports the route unavailable.
        struct AccentPolicy {
            int accent_state;
            int accent_flags;
            unsigned int gradient_color; // ABGR
            int animation_id;
        };
        struct WindowCompositionAttributeData {
            int attribute;
            void *data;
            SIZE_T size;
        };
        constexpr int sturdy_wca_accent_policy = 19;
        constexpr int sturdy_accent_disabled = 0;
        constexpr int sturdy_accent_enable_blurbehind = 3;
        constexpr int sturdy_accent_enable_acrylicblurbehind = 4;
        constexpr int sturdy_accent_draw_left_border = 0x20;
        constexpr int sturdy_accent_draw_top_border = 0x40;
        constexpr int sturdy_accent_draw_right_border = 0x80;
        constexpr int sturdy_accent_draw_bottom_border = 0x100;

        [[nodiscard]] unsigned int abgr_from_argb(u32 argb) noexcept {
            const unsigned int a = (argb >> 24U) & 0xFFU;
            const unsigned int r = (argb >> 16U) & 0xFFU;
            const unsigned int g = (argb >> 8U) & 0xFFU;
            const unsigned int b = argb & 0xFFU;
            return (a << 24U) | (b << 16U) | (g << 8U) | r;
        }

        WindowEffectResult set_accent_policy(HWND hwnd, int accent_state, const WindowEffectOptions &options,
                                             bool enabled, const char *label) noexcept {
            using SetWindowCompositionAttributeFn = BOOL(WINAPI *)(HWND, WindowCompositionAttributeData *);
            static const SetWindowCompositionAttributeFn set_attribute = [] {
                const HMODULE user32 = GetModuleHandleW(L"user32.dll");
                return user32 != nullptr ? reinterpret_cast<SetWindowCompositionAttributeFn>(
                                               reinterpret_cast<void *>(GetProcAddress(user32, "SetWindowCompositionAttribute")))
                                         : nullptr;
            }();
            if (set_attribute == nullptr) {
                return WindowEffectResult::failed("SetWindowCompositionAttribute is not available on this Windows.");
            }
            AccentPolicy policy{};
            policy.accent_state = enabled ? accent_state : sturdy_accent_disabled;
            if (enabled) {
                policy.accent_flags = (options.accent_border_left ? sturdy_accent_draw_left_border : 0) |
                                      (options.accent_border_top ? sturdy_accent_draw_top_border : 0) |
                                      (options.accent_border_right ? sturdy_accent_draw_right_border : 0) |
                                      (options.accent_border_bottom ? sturdy_accent_draw_bottom_border : 0);
                policy.gradient_color = abgr_from_argb(options.tint_argb);
            }
            WindowCompositionAttributeData data{sturdy_wca_accent_policy, &policy, sizeof(policy)};
            if (!set_attribute(hwnd, &data)) {
                Detail::window_error("Windows accent policy failed: hwnd={} label='{}'", log_hwnd(hwnd), label);
                return WindowEffectResult::failed("SetWindowCompositionAttribute rejected the accent policy.");
            }
            Detail::window_info("Windows accent policy set: hwnd={} label='{}' enabled={}", log_hwnd(hwnd), label, enabled);
            return WindowEffectResult::success("Windows accent policy applied.");
        }

        WindowEffectResult set_legacy_blur(HWND hwnd, bool enabled, const WindowEffectOptions &options = {}) noexcept {
            DWM_BLURBEHIND blur{};
            blur.dwFlags = DWM_BB_ENABLE;
            blur.fEnable = enabled ? TRUE : FALSE;
            HRGN region = enabled ? make_blur_region(options) : nullptr;
            if (region != nullptr) {
                blur.dwFlags |= DWM_BB_BLURREGION;
                blur.hRgnBlur = region;
            }

            const HRESULT result = DwmEnableBlurBehindWindow(hwnd, &blur);
            if (region != nullptr) {
                DeleteObject(region);
            }
            if (FAILED(result)) [[unlikely]] {
                Detail::window_error("Windows DWM legacy blur failed: hwnd={} enabled={} hresult={}", log_hwnd(hwnd), enabled, static_cast<long>(result));
                return WindowEffectResult::failed("DwmEnableBlurBehindWindow failed.");
            }

            Detail::window_info("Windows DWM legacy blur set: hwnd={} enabled={}", log_hwnd(hwnd), enabled);
            return WindowEffectResult::success("Windows legacy DWM blur applied.");
        }

        /// Sets the system backdrop for this `Windowing`.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param enabled_backdrop Whether the associated behavior is enabled.
        /// @param enabled Whether the associated behavior is enabled.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        WindowEffectResult set_system_backdrop(HWND hwnd, int enabled_backdrop, bool enabled, const char *label,
                                               const WindowEffectOptions &options = {}) noexcept {
            const int backdrop = enabled ? enabled_backdrop : sturdy_dwmsbt_none;
            Detail::window_debug("Windows DWM backdrop request: hwnd={} label='{}' enabled={} backdrop={}", log_hwnd(hwnd), label, enabled, backdrop);
            WindowEffectResult result = set_dwm_attribute(hwnd, sturdy_dwmwa_system_backdrop_type, &backdrop, sizeof(backdrop), label);
            if (result.succeeded()) [[likely]] {
                return WindowEffectResult::success("Windows system backdrop applied.");
            }

            if (!enabled) [[unlikely]] {
                return result;
            }

            // Windows without system backdrops (before 11 22H2): the accent-policy acrylic is the closest match,
            // then the pre-Windows-10 DWM blur-behind. Each step is reported as a degradation, never silently.
            if (windows_build_number() >= sturdy_build_accent_acrylic) {
                WindowEffectResult accent =
                    set_accent_policy(hwnd, sturdy_accent_enable_acrylicblurbehind, options, true, label);
                if (accent.succeeded()) {
                    Detail::window_warn("Windows DWM backdrop degraded to accent acrylic: hwnd={} label='{}'", log_hwnd(hwnd), label);
                    return WindowEffectResult::degraded("The system backdrop is unavailable on this Windows; accent-policy acrylic was applied instead.");
                }
            }
            WindowEffectResult fallback = set_legacy_blur(hwnd, true, options);
            if (fallback.succeeded()) [[likely]] {
                Detail::window_warn("Windows DWM backdrop degraded to legacy blur: hwnd={} label='{}'", log_hwnd(hwnd), label);
                return WindowEffectResult::degraded("Requested Windows backdrop was unavailable; legacy blur was enabled instead.");
            }

            return WindowEffectResult::failed("Requested Windows backdrop failed and the accent-policy and legacy blur fallbacks also failed.");
        }

        /// Reports what each Windows effect needs, from the running build number.
        [[nodiscard]] WindowEffectSupport windows_effect_support(WindowEffectKind kind) noexcept {
            WindowEffectSupport support;
            const DWORD build = windows_build_number();
            const auto needs = [&](string_view name, DWORD required, string_view reason) {
                support.add(name, build >= required ? EffectAvailability::Available : EffectAvailability::Unavailable,
                            build >= required ? string_view{} : reason);
            };
            switch (kind) {
                case WindowEffectKind::Acrylic:
                case WindowEffectKind::Mica:
                case WindowEffectKind::MicaAlt:
                case WindowEffectKind::Tabbed:
                case WindowEffectKind::BackdropAuto:
                case WindowEffectKind::BackdropNone:
                    needs("DWMWA_SYSTEMBACKDROP_TYPE", sturdy_build_system_backdrop, "needs Windows 11 22H2 (build 22621)");
                    if (kind == WindowEffectKind::Acrylic || kind == WindowEffectKind::Mica ||
                        kind == WindowEffectKind::MicaAlt || kind == WindowEffectKind::Tabbed) {
                        needs("accent policy acrylic (fallback)", sturdy_build_accent_acrylic, "needs Windows 10 1803 (build 17134)");
                        support.add("DwmEnableBlurBehindWindow (fallback)", EffectAvailability::Unknown,
                                    "only visible where DWM glass exists");
                    }
                    break;
                case WindowEffectKind::AccentAcrylic:
                    needs("accent policy acrylic", sturdy_build_accent_acrylic, "needs Windows 10 1803 (build 17134)");
                    break;
                case WindowEffectKind::AccentBlur:
                    needs("accent policy blur-behind", sturdy_build_win10, "needs Windows 10");
                    break;
                case WindowEffectKind::Blur:
                    needs("accent policy blur-behind", sturdy_build_win10, "needs Windows 10");
                    support.add("DwmEnableBlurBehindWindow", EffectAvailability::Unknown, "only visible where DWM glass exists");
                    break;
                case WindowEffectKind::DarkMode:
                    needs("DWMWA_USE_IMMERSIVE_DARK_MODE", sturdy_build_dark_mode, "needs Windows 10 2004 (build 19041)");
                    break;
                case WindowEffectKind::BorderColor:
                case WindowEffectKind::CaptionColor:
                case WindowEffectKind::TextColor:
                case WindowEffectKind::CornerPreference:
                    needs("DWM window attribute", sturdy_build_win11, "needs Windows 11");
                    break;
                case WindowEffectKind::Transparent:
                    support.add("WS_EX_LAYERED + DwmExtendFrameIntoClientArea", EffectAvailability::Available);
                    break;
                case WindowEffectKind::VisualEffect:
                    support.add("none", EffectAvailability::Unavailable, "NSVisualEffectView is a macOS facility");
                    break;
            }
            return support;
        }


        /// Sets the transparent for this `Windowing`.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        WindowEffectResult set_transparent(HWND hwnd, bool enabled) noexcept {
            const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            const LONG_PTR new_ex_style = enabled ? (ex_style | WS_EX_LAYERED) : (ex_style & ~WS_EX_LAYERED);
            if (new_ex_style != ex_style) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_ex_style);
            }

            const MARGINS margins = enabled ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
            const HRESULT result = DwmExtendFrameIntoClientArea(hwnd, &margins);
            if (FAILED(result)) [[unlikely]] {
                Detail::window_error("Windows DWM extend-frame-into-client-area failed: hwnd={} enabled={} hresult={}", log_hwnd(hwnd), enabled, static_cast<long>(result));
                return WindowEffectResult::failed("DwmExtendFrameIntoClientArea failed.");
            }

            Detail::window_info("Windows window transparency set: hwnd={} enabled={}", log_hwnd(hwnd), enabled);
            return WindowEffectResult::success("Windows window transparency applied.");
        }
#endif

    } // namespace

    /// Returns the current or globally available current operating system value.
    ///
    /// @return Returns the current current operating system value.
    /// @note This function does not throw exceptions.
    OperatingSystem current_operating_system() noexcept {
#if defined(_WIN32)
        return OperatingSystem::Windows;
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
            case WindowEffectKind::Transparent:
            case WindowEffectKind::BackdropAuto:
            case WindowEffectKind::BackdropNone:
            case WindowEffectKind::AccentBlur:
            case WindowEffectKind::AccentAcrylic:
            case WindowEffectKind::CornerPreference:
                return true;
            case WindowEffectKind::VisualEffect:
                return false;
        }
#else
        (void)effect;
#endif

        return false;
    }

    /// Releases native window effects using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param release_display `release_display` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_native_window_effects(NativeWindowHandle handle, bool release_display) noexcept {
        (void)handle;
        (void)release_display;
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
        (void)handle;
#if defined(_WIN32)
        return windows_effect_support(kind);
#else
        (void)kind;
        WindowEffectSupport support;
        support.add("none", EffectAvailability::Unavailable, "not a Windows build");
        return support;
#endif
    }

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
#if defined(_WIN32)
        if (handle.system != NativeWindowSystem::Win32 || !handle.window) [[unlikely]] {
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

        if (!operating_system_may_support_window_effect(effect.kind)) [[unlikely]] {
            Detail::window_warn(
                "Windows window effect kind={} enabled={} is not supported on this OS; no-op.",
                static_cast<int>(effect.kind),
                effect.enabled);
            return WindowEffectResult::failed("This window effect is not supported on the current OS.");
        }

        HWND hwnd = static_cast<HWND>(handle.window);
        Detail::window_info(
            "Windows native effect requested: hwnd={} kind={} enabled={} color_argb=0x{:08X}",
            log_hwnd(hwnd),
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);

        switch (effect.kind) {
            case WindowEffectKind::Blur:
                {
                    // Generic blur: the accent-policy blur on Windows 10+ (the only one of the two that is visible
                    // without Aero glass), the legacy DWM blur-behind (with the region) before that.
                    if (windows_build_number() >= sturdy_build_win10) {
                        WindowEffectResult accent =
                            set_accent_policy(hwnd, sturdy_accent_enable_blurbehind, effect.options, effect.enabled, "Blur");
                        if (accent.succeeded()) {
                            return accent;
                        }
                    }
                    WindowEffectResult legacy = set_legacy_blur(hwnd, effect.enabled, effect.options);
                    return legacy.succeeded() && effect.enabled
                               ? WindowEffectResult::degraded("Accent-policy blur was unavailable; legacy DWM blur-behind was used.")
                               : legacy;
                }
            case WindowEffectKind::AccentBlur:
                return set_accent_policy(hwnd, sturdy_accent_enable_blurbehind, effect.options, effect.enabled, "AccentBlur");
            case WindowEffectKind::AccentAcrylic:
                return set_accent_policy(hwnd, sturdy_accent_enable_acrylicblurbehind, effect.options, effect.enabled, "AccentAcrylic");
            case WindowEffectKind::Acrylic:
                return set_system_backdrop(hwnd, sturdy_dwmsbt_transient_window, effect.enabled, "Acrylic", effect.options);
            case WindowEffectKind::Mica:
                return set_system_backdrop(hwnd, sturdy_dwmsbt_main_window, effect.enabled, "Mica", effect.options);
            case WindowEffectKind::MicaAlt:
            case WindowEffectKind::Tabbed:
                return set_system_backdrop(hwnd, sturdy_dwmsbt_tabbed_window, effect.enabled, "MicaAlt/Tabbed", effect.options);
            case WindowEffectKind::BackdropAuto:
                {
                    const int backdrop = effect.enabled ? sturdy_dwmsbt_auto : sturdy_dwmsbt_none;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_system_backdrop_type, &backdrop, sizeof(backdrop), "BackdropAuto");
                }
            case WindowEffectKind::BackdropNone:
                {
                    const int backdrop = sturdy_dwmsbt_none;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_system_backdrop_type, &backdrop, sizeof(backdrop), "BackdropNone");
                }
            case WindowEffectKind::CornerPreference:
                {
                    // DWM_WINDOW_CORNER_PREFERENCE: 0 default, 1 do not round, 2 round, 3 round small.
                    const int preference = effect.enabled ? static_cast<int>(effect.options.corner_preference) : 0;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_window_corner_preference, &preference, sizeof(preference), "CornerPreference");
                }
            case WindowEffectKind::VisualEffect:
                return WindowEffectResult::failed("NSVisualEffectView is a macOS facility.");
            case WindowEffectKind::DarkMode:
                {
                    const BOOL enabled = effect.enabled ? TRUE : FALSE;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_use_immersive_dark_mode, &enabled, sizeof(enabled), "DarkMode");
                }
            case WindowEffectKind::BorderColor:
                {
                    const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_border_color, &color, sizeof(color), "BorderColor");
                }
            case WindowEffectKind::CaptionColor:
                {
                    const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_caption_color, &color, sizeof(color), "CaptionColor");
                }
            case WindowEffectKind::TextColor:
                {
                    const COLORREF color = effect.enabled ? colorref_from_argb(effect.color_argb) : sturdy_dwm_color_default;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_text_color, &color, sizeof(color), "TextColor");
                }
            case WindowEffectKind::Transparent:
                return set_transparent(hwnd, effect.enabled);
        }

        return WindowEffectResult::failed("Unsupported Windows window effect.");
#else
        (void)handle;
        (void)effect;
        return WindowEffectResult::failed("Windows window effects are only available on Windows builds.");
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
#if defined(_WIN32)
        auto *window = static_cast<SDL_Window *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("SDL3 Win32 native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Win32 native handle requires a live window."});
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        NativeWindowHandle handle{
            NativeWindowSystem::Win32,
            nullptr,
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr),
        };
        if (!handle.window) [[unlikely]] {
            Detail::window_error("SDL3 Win32 native handle missing HWND: sdl_window={} properties={}", static_cast<void *>(window), properties);
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Win32 native handle is incomplete."});
        }
        Detail::window_debug("SDL3 Win32 native handle resolved: sdl_window={} properties={} hwnd={}", static_cast<void *>(window), properties, handle.window);
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "SDL3 Win32 native handles are only available on Windows builds."});
#endif
    }

} // namespace SFT::WindowManager::Detail
