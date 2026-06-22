#pragma once

#include "Window.hpp"

namespace SFT::Platform::Windowing {

    [[nodiscard]] OperatingSystem current_operating_system() noexcept;
    [[nodiscard]] bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept;
    [[nodiscard]] WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;

    [[nodiscard]] inline WindowResult window_result_from_effect_result(WindowEffectResult result) noexcept {
        if (result.succeeded()) {
            return {};
        }

        return std::unexpected(WindowError{WindowErrorCode::OperationFailed, result.details.empty() ? std::string_view{"Window effect failed."} : result.details});
    }

    [[nodiscard]] inline WindowResult set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        return window_result_from_effect_result(enable_native_window_effect(handle, effect));
    }

    [[nodiscard]] inline WindowResult set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept {
        return set_native_window_effect(handle, WindowEffect::blur(enabled));
    }

} // namespace SFT::Platform::Windowing
