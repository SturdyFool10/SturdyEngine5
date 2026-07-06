module;

#pragma region Imports
#include <expected>
#include <string_view>
#pragma endregion

export module Sturdy.Platform:WindowEffects;

import :WindowError;
import :WindowConfig;
import :WindowEffect;

using std::expected;
using std::string_view;
using std::unexpected;

export namespace SFT::Platform::Windowing {

    [[nodiscard]] OperatingSystem current_operating_system() noexcept;
    [[nodiscard]] bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept;
    [[nodiscard]] WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;

    [[nodiscard]] inline expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept {
        if (result.succeeded()) [[likely]] {
            return {};
        }

        return unexpected(WindowError{WindowErrorCode::OperationFailed, result.details.empty() ? string_view{"Window effect failed."} : result.details});
    }

    [[nodiscard]] inline expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        return window_result_from_effect_result(enable_native_window_effect(handle, effect));
    }

    [[nodiscard]] inline expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept {
        return set_native_window_effect(handle, WindowEffect::blur(enabled));
    }

} // namespace SFT::Platform::Windowing
