#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string_view>
#pragma endregion

using std::string_view;

namespace SFT::Platform::Windowing {

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
    };

    enum class LinuxBlurProtocol {
        Automatic,
        ExtBackgroundEffect,
        KdeBlur,
    };

    struct WindowEffect {
        WindowEffectKind kind = WindowEffectKind::Blur;
        bool enabled = true;
        u32 color_argb = 0;
        LinuxBlurProtocol linux_blur_protocol = LinuxBlurProtocol::Automatic;

        [[nodiscard]] static constexpr WindowEffect blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect linux_ext_background_effect_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::ExtBackgroundEffect};
        }

        [[nodiscard]] static constexpr WindowEffect linux_kde_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::KdeBlur};
        }

        [[nodiscard]] static constexpr WindowEffect acrylic(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Acrylic, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect mica(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Mica, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect mica_alt(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::MicaAlt, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect tabbed(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Tabbed, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect dark_mode(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::DarkMode, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect border_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::BorderColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect caption_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::CaptionColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect text_color(u32 color_argb) noexcept {
            return WindowEffect{WindowEffectKind::TextColor, true, color_argb, LinuxBlurProtocol::Automatic};
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

        [[nodiscard]] static constexpr WindowEffectResult success(string_view details = {}) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Success, details};
        }

        [[nodiscard]] static constexpr WindowEffectResult degraded(string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Degraded, details};
        }

        [[nodiscard]] static constexpr WindowEffectResult failed(string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Failed, details};
        }

        [[nodiscard]] constexpr bool succeeded() const noexcept {
            return kind == WindowEffectResultKind::Success || kind == WindowEffectResultKind::Degraded;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return succeeded();
        }
    };

} // namespace SFT::Platform::Windowing
