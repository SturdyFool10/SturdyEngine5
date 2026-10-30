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
        constexpr DWORD sturdy_dwmwa_transitions_forcedisabled = 3;
        // Windows 11 21H2 (build 22000..22620) predates `DWMWA_SYSTEMBACKDROP_TYPE`; Mica there is this boolean attribute.
        constexpr DWORD sturdy_dwmwa_mica_effect = 1029;

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
        constexpr COLORREF sturdy_dwm_color_none = 0xFFFFFFFE; // DWMWA_COLOR_NONE

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

            // Windows 11 21H2 has Mica only as the older boolean attribute.
            if (backdrop == sturdy_dwmsbt_main_window && windows_build_number() >= sturdy_build_win11) {
                const BOOL mica = TRUE;
                if (set_dwm_attribute(hwnd, sturdy_dwmwa_mica_effect, &mica, sizeof(mica), label).succeeded()) {
                    return WindowEffectResult::degraded("The system backdrop is unavailable on this Windows 11; the legacy Mica attribute was applied.");
                }
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
                case WindowEffectKind::Opacity:
                    support.add("WS_EX_LAYERED + SetLayeredWindowAttributes", EffectAvailability::Available);
                    break;
                case WindowEffectKind::CornerRadius:
                    support.add("SetWindowRgn (hard-edged)", EffectAvailability::Available);
                    break;
                case WindowEffectKind::MousePassthrough:
                    support.add("WS_EX_LAYERED | WS_EX_TRANSPARENT", EffectAvailability::Available);
                    break;
                case WindowEffectKind::Focusable:
                    support.add("WS_EX_NOACTIVATE", EffectAvailability::Available);
                    break;
                case WindowEffectKind::KeyboardGrab:
                    support.add("none", EffectAvailability::Unavailable, "Windows has no per-window keyboard grab for applications");
                    break;
                case WindowEffectKind::TransitionsDisabled:
                    needs("DWMWA_TRANSITIONS_FORCEDISABLED", sturdy_build_win10, "needs Windows 10");
                    break;
                case WindowEffectKind::VisualEffect:
                case WindowEffectKind::GlassEffect:
                case WindowEffectKind::TitlebarTransparent:
                case WindowEffectKind::WindowShadow:
                    support.add("none", EffectAvailability::Unavailable, "this is a macOS facility");
                    break;
            }
            return support;
        }


        /// Toggles one extended style bit; `SWP_FRAMECHANGED` makes the window manager re-read it.
        WindowEffectResult set_ex_style(HWND hwnd, LONG_PTR bit, bool enabled, const char *success) noexcept {
            const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            const LONG_PTR updated = enabled ? (ex_style | bit) : (ex_style & ~bit);
            if (updated != ex_style) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, updated);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return WindowEffectResult::success(success);
        }

        /// Click-through needs a layered window: `WS_EX_TRANSPARENT` alone only changes painting order. A freshly
        /// layered window shows nothing until it has an alpha, so give it full opacity.
        WindowEffectResult set_mouse_passthrough(HWND hwnd, bool enabled) noexcept {
            if (enabled && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
                if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
                    return WindowEffectResult::failed("This window cannot be layered (a DirectComposition swapchain window cannot).");
                }
            }
            return set_ex_style(hwnd, WS_EX_TRANSPARENT, enabled, "Windows click-through set.");
        }

        /// Applies `DWMWA_WINDOW_CORNER_PREFERENCE`, then reads it back and checks whether DWM can act on it. DWM
        /// returns S_OK for the attribute on any window but only rounds windows that have a frame, so a frameless
        /// (WS_POPUP, no WS_CAPTION/WS_THICKFRAME) window is reported as degraded rather than silently doing nothing.
        WindowEffectResult set_corner_preference(HWND hwnd, int preference) noexcept {
            if (windows_build_number() < sturdy_build_win11) {
                return WindowEffectResult::failed("Window corner preference needs Windows 11 (build 22000).");
            }
            WindowEffectResult applied =
                set_dwm_attribute(hwnd, sturdy_dwmwa_window_corner_preference, &preference, sizeof(preference), "CornerPreference");
            if (!applied.succeeded()) {
                return applied;
            }
            int stored = -1;
            if (FAILED(DwmGetWindowAttribute(hwnd, sturdy_dwmwa_window_corner_preference, &stored, sizeof(stored))) ||
                stored != preference) {
                Detail::window_warn("Windows corner preference read back as {} after requesting {}: hwnd={}", stored, preference, log_hwnd(hwnd));
                return WindowEffectResult::failed("DWM did not keep the corner preference.");
            }
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            const bool framed = (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
            if (!framed) {
                Detail::window_warn("Windows corner preference set on a frameless window; DWM may not round it: hwnd={}", log_hwnd(hwnd));
                return WindowEffectResult::degraded(
                    "The preference was stored, but this window has no frame (WS_CAPTION/WS_THICKFRAME), which DWM needs to round or square it; use the CornerRadius effect for frameless windows.");
            }
            if (IsZoomed(hwnd)) {
                return WindowEffectResult::degraded("The preference was stored; DWM draws maximized windows square.");
            }
            return WindowEffectResult::success("Windows corner preference applied.");
        }

        /// Rounds the window outline with a window region. Hard-edged (no anti-aliasing: a region is a 1-bit mask) but
        /// independent of the window's frame, so it works on frameless windows and on Windows 10. Re-applied on resize.
        WindowEffectResult set_corner_radius(HWND hwnd, bool enabled, const WindowEffectOptions &options) noexcept {
            RECT rect{};
            const bool round = enabled && options.window_corner_radius > 0.0f && !IsZoomed(hwnd) &&
                               GetWindowRect(hwnd, &rect) != FALSE && rect.right > rect.left && rect.bottom > rect.top;
            if (!round) {
                SetWindowRgn(hwnd, nullptr, TRUE);
                return WindowEffectResult::success("Windows window region cleared.");
            }
            const f32 scale = options.pixel_scale > 0.0f ? options.pixel_scale : 1.0f;
            const int diameter = static_cast<int>(std::lround(options.window_corner_radius * scale * 2.0f));
            // CreateRoundRectRgn's right/bottom edges are exclusive, so +1 keeps the full window area.
            HRGN region = CreateRoundRectRgn(0, 0, rect.right - rect.left + 1, rect.bottom - rect.top + 1, diameter, diameter);
            if (region == nullptr) [[unlikely]] {
                return WindowEffectResult::failed("CreateRoundRectRgn failed.");
            }
            // On success the system owns the region; only free it when it was not taken.
            if (SetWindowRgn(hwnd, region, TRUE) == 0) [[unlikely]] {
                DeleteObject(region);
                return WindowEffectResult::failed("SetWindowRgn failed.");
            }
            Detail::window_info("Windows window region rounded: hwnd={} diameter={}", log_hwnd(hwnd), diameter);
            return WindowEffectResult::success("Windows window region applied (hard-edged corners).");
        }

        /// Sets the whole-window opacity (0..1) through a layered window; 1 restores full opacity.
        WindowEffectResult set_opacity(HWND hwnd, f32 opacity) noexcept {
            const f32 clamped = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
            const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((ex_style & WS_EX_LAYERED) == 0) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
            }
            const BYTE alpha = static_cast<BYTE>(std::lround(clamped * 255.0f));
            if (!SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)) [[unlikely]] {
                Detail::window_error("Windows SetLayeredWindowAttributes failed: hwnd={} alpha={}", log_hwnd(hwnd), static_cast<int>(alpha));
                return WindowEffectResult::failed("SetLayeredWindowAttributes failed.");
            }
            Detail::window_info("Windows window opacity set: hwnd={} alpha={}", log_hwnd(hwnd), static_cast<int>(alpha));
            return WindowEffectResult::success("Windows window opacity applied.");
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
            case WindowEffectKind::Opacity:
            case WindowEffectKind::TransitionsDisabled:
            case WindowEffectKind::CornerRadius:
            case WindowEffectKind::MousePassthrough:
            case WindowEffectKind::Focusable:
                return true;
            case WindowEffectKind::KeyboardGrab:
                return false;
            case WindowEffectKind::VisualEffect:
            case WindowEffectKind::GlassEffect:
            case WindowEffectKind::TitlebarTransparent:
            case WindowEffectKind::WindowShadow:
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

    WindowEffectResult set_native_window_type(NativeWindowHandle handle, WindowType type) noexcept {
#if defined(_WIN32)
        if (handle.system != NativeWindowSystem::Win32 || handle.window == nullptr) {
            return WindowEffectResult::failed("Windows window types require a Win32 HWND.");
        }
        const HWND hwnd = static_cast<HWND>(handle.window);
        // The extended styles that separate a palette, popup or dialog from a document window.
        const bool tool = type == WindowType::Utility || type == WindowType::Tooltip || type == WindowType::PopupMenu ||
                          type == WindowType::DropdownMenu || type == WindowType::Splash;
        const bool no_activate = type == WindowType::Tooltip || type == WindowType::PopupMenu || type == WindowType::DropdownMenu;
        const bool topmost = tool && type != WindowType::Utility;
        LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex_style = tool ? (ex_style | WS_EX_TOOLWINDOW) : (ex_style & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW));
        ex_style = no_activate ? (ex_style | WS_EX_NOACTIVATE) : (ex_style & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
        ex_style = type == WindowType::Dialog ? (ex_style | WS_EX_DLGMODALFRAME) : (ex_style & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME));
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
        SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return WindowEffectResult::success("Windows window styles set.");
#else
        (void)handle;
        (void)type;
        return WindowEffectResult::failed("Windows window types are only available on Windows builds.");
#endif
    }

    WindowEffectResult set_native_window_shape(NativeWindowHandle handle, const WindowShapeMask &mask, WindowExtent pixel_size) noexcept {
#if defined(_WIN32)
        if (handle.system != NativeWindowSystem::Win32 || handle.window == nullptr) {
            return WindowEffectResult::failed("Windows window shapes require a Win32 HWND.");
        }
        const HWND hwnd = static_cast<HWND>(handle.window);
        if (mask.coverage.empty()) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            return WindowEffectResult::success("Windows window region cleared.");
        }
        const std::vector<EffectRect> rects = resolve_shape_region(mask, pixel_size);
        if (rects.empty()) {
            return WindowEffectResult::failed("The shape mask covers nothing; a window needs at least one visible pixel.");
        }
        // One RGNDATA with every rectangle: far cheaper than combining thousands of regions one by one.
        std::vector<unsigned char> storage(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
        auto *data = reinterpret_cast<RGNDATA *>(storage.data());
        data->rdh.dwSize = sizeof(RGNDATAHEADER);
        data->rdh.iType = RDH_RECTANGLES;
        data->rdh.nCount = static_cast<DWORD>(rects.size());
        data->rdh.nRgnSize = static_cast<DWORD>(rects.size() * sizeof(RECT));
        data->rdh.rcBound = RECT{0, 0, static_cast<LONG>(pixel_size.x), static_cast<LONG>(pixel_size.y)};
        auto *buffer = reinterpret_cast<RECT *>(data->Buffer);
        for (usize i = 0; i < rects.size(); ++i) {
            buffer[i] = RECT{rects[i].x, rects[i].y, rects[i].x + rects[i].width, rects[i].y + rects[i].height};
        }
        HRGN region = ExtCreateRegion(nullptr, static_cast<DWORD>(storage.size()), data);
        if (region == nullptr) {
            return WindowEffectResult::failed("ExtCreateRegion failed.");
        }
        if (SetWindowRgn(hwnd, region, TRUE) == 0) {
            DeleteObject(region);
            return WindowEffectResult::failed("SetWindowRgn failed.");
        }
        return WindowEffectResult::success("Windows window region applied (hard-edged).");
#else
        (void)handle;
        (void)mask;
        (void)pixel_size;
        return WindowEffectResult::failed("Windows window shapes are only available on Windows builds.");
#endif
    }

    WindowEffectResult set_native_window_parent(NativeWindowHandle child, std::optional<NativeWindowHandle> parent, bool modal) noexcept {
#if defined(_WIN32)
        if (child.system != NativeWindowSystem::Win32 || child.window == nullptr) {
            return WindowEffectResult::failed("Windows window parenting requires a Win32 HWND.");
        }
        HWND owner = nullptr;
        if (parent) {
            if (parent->system != NativeWindowSystem::Win32 || parent->window == nullptr) {
                return WindowEffectResult::failed("The parent window is not a Win32 window.");
            }
            owner = static_cast<HWND>(parent->window);
        }
        // For a top-level window GWLP_HWNDPARENT sets the *owner*: the child stays above it, minimises with it and
        // closes with it, which is what "child window" means for a dialog.
        SetLastError(0);
        if (SetWindowLongPtrW(static_cast<HWND>(child.window), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner)) == 0 && GetLastError() != 0) {
            return WindowEffectResult::failed("SetWindowLongPtr(GWLP_HWNDPARENT) failed.");
        }
        // Real modality means disabling the owner until the child closes. Doing that from here could leave the owner
        // disabled for good if the child is destroyed without being detached first, so only the owner link is set.
        return parent && modal ? WindowEffectResult::degraded("The window is owned by its parent; the parent is not disabled, so modality is not enforced.")
                               : WindowEffectResult::success("Windows owner window set.");
#else
        (void)child;
        (void)parent;
        (void)modal;
        return WindowEffectResult::failed("Windows window parenting is only available on Windows builds.");
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
                    return set_corner_preference(hwnd, preference);
                }
            case WindowEffectKind::CornerRadius:
                return set_corner_radius(hwnd, effect.enabled, effect.options);
            case WindowEffectKind::MousePassthrough:
                return set_mouse_passthrough(hwnd, effect.enabled);
            case WindowEffectKind::Focusable:
                // Disabled focus is the NOACTIVATE bit: clicks no longer activate the window.
                return set_ex_style(hwnd, WS_EX_NOACTIVATE, !effect.enabled, "Windows activation behaviour set.");
            case WindowEffectKind::KeyboardGrab:
                return WindowEffectResult::failed("Windows has no per-window keyboard grab for applications.");
            case WindowEffectKind::Opacity:
                return set_opacity(hwnd, effect.enabled ? effect.options.opacity : 1.0f);
            case WindowEffectKind::TransitionsDisabled:
                {
                    const BOOL disabled = effect.enabled ? TRUE : FALSE;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_transitions_forcedisabled, &disabled, sizeof(disabled), "TransitionsDisabled");
                }
            case WindowEffectKind::VisualEffect:
            case WindowEffectKind::GlassEffect:
            case WindowEffectKind::TitlebarTransparent:
            case WindowEffectKind::WindowShadow:
                return WindowEffectResult::failed("This window effect is a macOS facility.");
            case WindowEffectKind::DarkMode:
                {
                    const BOOL enabled = effect.enabled ? TRUE : FALSE;
                    return set_dwm_attribute(hwnd, sturdy_dwmwa_use_immersive_dark_mode, &enabled, sizeof(enabled), "DarkMode");
                }
            case WindowEffectKind::BorderColor:
                {
                    const COLORREF color = !effect.enabled                ? sturdy_dwm_color_default
                                           : effect.options.hide_border ? sturdy_dwm_color_none
                                                                        : colorref_from_argb(effect.color_argb);
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
