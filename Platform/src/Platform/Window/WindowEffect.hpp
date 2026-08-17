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


        Transparent,
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
        }
        return "Unknown";
    }

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

} // namespace SFT::Platform::Windowing
