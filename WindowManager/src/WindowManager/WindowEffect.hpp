#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string_view>
#pragma endregion

using std::string_view;

namespace SFT::WindowManager {

    enum class WindowEffectKind {
        Blur,
        Acrylic,
        Mica,
        MicaAlt,
        Tabbed,
        DarkMode,
        BorderColor,
        CaptionColor,
        TextColor,


        Transparent,

        // Appended (the Rust binding mirrors this order). Windows 11 22H2+ `DWMWA_SYSTEMBACKDROP_TYPE` values that
        // `Acrylic`/`Mica`/`Tabbed` do not cover: let DWM choose, or none at all.
        BackdropAuto,
        BackdropNone,
        /// Windows 10+: `SetWindowCompositionAttribute` accent policy, blur-behind (`ACCENT_ENABLE_BLURBEHIND`).
        AccentBlur,
        /// Windows 10 1803+: accent policy acrylic (`ACCENT_ENABLE_ACRYLICBLURBEHIND`) with a tint colour. The
        /// fallback for Windows versions that predate system backdrops.
        AccentAcrylic,
        /// Windows 11: window corner rounding (`DWMWA_WINDOW_CORNER_PREFERENCE`).
        CornerPreference,
        /// macOS: an `NSVisualEffectView` behind the content (material, blending, state, emphasis, appearance).
        VisualEffect,
    };


    /// Returns a human-readable name for the supplied window effect kind value.
    ///
    /// @param kind `kind` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr string_view window_effect_kind_name(WindowEffectKind kind) noexcept {
        switch (kind) {
            case WindowEffectKind::Blur: return "Blur";
            case WindowEffectKind::Acrylic: return "Acrylic";
            case WindowEffectKind::Mica: return "Mica";
            case WindowEffectKind::MicaAlt: return "Mica Alt";
            case WindowEffectKind::Tabbed: return "Tabbed";
            case WindowEffectKind::DarkMode: return "Dark Mode";
            case WindowEffectKind::BorderColor: return "Border Color";
            case WindowEffectKind::CaptionColor: return "Caption Color";
            case WindowEffectKind::TextColor: return "Text Color";
            case WindowEffectKind::Transparent: return "Transparent";
            case WindowEffectKind::BackdropAuto: return "Backdrop (Auto)";
            case WindowEffectKind::BackdropNone: return "Backdrop (None)";
            case WindowEffectKind::AccentBlur: return "Accent Blur";
            case WindowEffectKind::AccentAcrylic: return "Accent Acrylic";
            case WindowEffectKind::CornerPreference: return "Corner Preference";
            case WindowEffectKind::VisualEffect: return "Visual Effect";
        }
        return "Unknown";
    }

    /// Whether this build's platform implements `kind` at all: a compile-time upper bound. The real answer for a
    /// given window, OS version and compositor is `Window::window_effect_support`; the per-request verdict is
    /// `WindowRequests::effect_status`.
    [[nodiscard]] constexpr bool window_effect_supported_on_platform(WindowEffectKind kind) noexcept {
#if defined(_WIN32)
        return kind != WindowEffectKind::VisualEffect;
#elif defined(__linux__)
        return kind == WindowEffectKind::Blur;
#elif defined(__APPLE__)
        return kind == WindowEffectKind::VisualEffect || kind == WindowEffectKind::Blur ||
               kind == WindowEffectKind::Transparent;
#else
        (void)kind;
        return false;
#endif
    }

    /// The route a Linux blur request takes. `Automatic` prefers `ext-background-effect-v1`, falls back to KDE's
    /// `org_kde_kwin_blur` on Wayland, and uses the `_KDE_NET_WM_BLUR_BEHIND_REGION` property on X11.
    enum class LinuxBlurProtocol {
        Automatic,
        ExtBackgroundEffect,
        KdeBlur,
        /// X11 only: the `_KDE_NET_WM_BLUR_BEHIND_REGION` window property KWin (and Plasma-aware compositors) read.
        KdeX11,
    };

    /// An integer rectangle in window-local logical pixels (origin top-left).
    struct EffectRect {
        i32 x = 0;
        i32 y = 0;
        i32 width = 0;
        i32 height = 0;
    };

    /// Windows 11 `DWM_WINDOW_CORNER_PREFERENCE`.
    enum class WindowCornerPreference : u8 {
        Default,
        DoNotRound,
        Round,
        RoundSmall,
    };

    /// `NSVisualEffectMaterial` (macOS 10.14 semantic materials).
    enum class MacVisualEffectMaterial : u8 {
        Titlebar,
        Selection,
        Menu,
        Popover,
        Sidebar,
        HeaderView,
        Sheet,
        WindowBackground,
        HudWindow,
        FullScreenUi,
        ToolTip,
        ContentBackground,
        UnderWindowBackground,
        UnderPageBackground,
    };

    /// `NSVisualEffectBlendingMode`.
    enum class MacBlendingMode : u8 {
        BehindWindow,
        WithinWindow,
    };

    /// `NSVisualEffectState`.
    enum class MacVisualEffectState : u8 {
        FollowsWindowActiveState,
        Active,
        Inactive,
    };

    /// `NSAppearanceName` override for the effect view (`Inherit` keeps the window's appearance).
    enum class MacAppearance : u8 {
        Inherit,
        Aqua,
        DarkAqua,
        VibrantLight,
        VibrantDark,
    };

    /// Per-effect settings. Every field has a meaning for specific effects and is ignored by the others; the doc on
    /// each says which. Plain data (fixed capacity, no heap) so a request can be copied across threads freely.
    struct WindowEffectOptions {
        static constexpr usize max_rects = 8;

        /// Blur (all platforms but macOS): the region to blur in window-local logical pixels. Empty = the whole window.
        EffectRect blur_rects[max_rects]{};
        u8 blur_rect_count = 0;
        /// Blur: rounds the corners of the blurred region by this many logical pixels (0 = square). Applied by
        /// building the region from rectangles, so it works on every blur route. Needs `window_width/height`.
        f32 corner_radius = 0.0f;

        /// Filled in by the window before the request reaches the platform code; callers leave these at zero.
        u32 window_width = 0;
        u32 window_height = 0;
        /// Physical pixels per logical pixel (X11 blur regions are in device pixels).
        f32 pixel_scale = 1.0f;

        /// `AccentBlur` / `AccentAcrylic`: tint colour, ARGB (`AccentAcrylic` honours the alpha channel as its
        /// tint opacity; `AccentBlur` ignores it).
        u32 tint_argb = 0;
        /// `AccentBlur` / `AccentAcrylic`: draw the window border on these sides (accent-policy flag bits).
        bool accent_border_left = true;
        bool accent_border_top = true;
        bool accent_border_right = true;
        bool accent_border_bottom = true;

        /// `CornerPreference`.
        WindowCornerPreference corner_preference = WindowCornerPreference::Default;

        /// `VisualEffect` (macOS).
        MacVisualEffectMaterial mac_material = MacVisualEffectMaterial::UnderWindowBackground;
        MacBlendingMode mac_blending = MacBlendingMode::BehindWindow;
        MacVisualEffectState mac_state = MacVisualEffectState::FollowsWindowActiveState;
        bool mac_emphasized = false;
        MacAppearance mac_appearance = MacAppearance::Inherit;

        /// Whether the blur region depends on the window size, so it must be rebuilt when the window resizes.
        [[nodiscard]] constexpr bool region_follows_window_size() const noexcept {
            return corner_radius > 0.0f;
        }
    };

    struct WindowEffect {
        WindowEffectKind kind = WindowEffectKind::Blur;
        bool enabled = true;
        u32 color_argb = 0;
        LinuxBlurProtocol linux_blur_protocol = LinuxBlurProtocol::Automatic;
        WindowEffectOptions options{};

        /// Performs the blur operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the linux ext background effect blur operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect linux_ext_background_effect_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::ExtBackgroundEffect};
        }

        /// Performs the linux kde blur operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect linux_kde_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::KdeBlur};
        }

        /// Performs the acrylic operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect acrylic(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Acrylic, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the mica operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect mica(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Mica, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the mica alt operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect mica_alt(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::MicaAlt, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the tabbed operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect tabbed(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Tabbed, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the transparent operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect transparent(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Transparent, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the dark mode operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect dark_mode(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::DarkMode, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        /// Performs the border color operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param color_argb `color_argb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect border_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::BorderColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        /// Performs the caption color operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param color_argb `color_argb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect caption_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::CaptionColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        /// Performs the text color operation for `WindowEffect` using the supplied arguments.
        ///
        /// @param color_argb `color_argb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffect text_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::TextColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }
    };

    enum class EffectAvailability : u8 {
        /// The OS / compositor advertises what the effect needs.
        Available,
        /// It does not (and `reason` says why).
        Unavailable,
        /// Cannot be determined without trying (for example X11 blur: the window manager never says whether it
        /// reads `_KDE_NET_WM_BLUR_BEHIND_REGION`).
        Unknown,
    };

    /// One way of producing an effect, and whether this system offers it. Strings are static literals.
    struct EffectRoute {
        string_view name;
        EffectAvailability availability = EffectAvailability::Unknown;
        string_view reason;
    };

    /// What this window on this system can do for an effect kind, route by route. Querying creates no effect and
    /// changes nothing on the window.
    struct WindowEffectSupport {
        static constexpr usize max_routes = 4;

        /// `Available` when at least one route is, `Unavailable` when every route is, else `Unknown`.
        EffectAvailability availability = EffectAvailability::Unavailable;
        EffectRoute routes[max_routes]{};
        u8 route_count = 0;

        void add(string_view name, EffectAvailability route_availability, string_view reason = {}) noexcept {
            if (route_count < max_routes) {
                routes[route_count++] = EffectRoute{name, route_availability, reason};
            }
            if (route_availability == EffectAvailability::Available) {
                availability = EffectAvailability::Available;
            } else if (route_availability == EffectAvailability::Unknown && availability != EffectAvailability::Available) {
                availability = EffectAvailability::Unknown;
            }
        }
    };

    enum class WindowEffectResultKind {
        Success,
        Degraded,
        Failed,
    };

    struct WindowEffectResult {
        WindowEffectResultKind kind = WindowEffectResultKind::Failed;
        string_view details = {};

        /// Performs the success operation for `WindowEffectResult` using the supplied arguments.
        ///
        /// @param details `details` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffectResult success(string_view details = {}) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Success, details};
        }

        /// Performs the degraded operation for `WindowEffectResult` using the supplied arguments.
        ///
        /// @param details `details` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffectResult degraded(string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Degraded, details};
        }

        /// Performs the failed operation for `WindowEffectResult` using the supplied arguments.
        ///
        /// @param details `details` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr WindowEffectResult failed(string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Failed, details};
        }

        /// Returns the current or globally available succeeded value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool succeeded() const noexcept {
            return kind == WindowEffectResultKind::Success || kind == WindowEffectResultKind::Degraded;
        }

        /// Converts the `WindowEffectResult` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return succeeded();
        }
    };

} // namespace SFT::WindowManager
