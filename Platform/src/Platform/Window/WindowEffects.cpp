#include "WindowEffects.hpp"

namespace SFT::Platform::Windowing {

expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept {
        if (result.succeeded()) [[likely]] {
            return {};
        }

        return unexpected(WindowError{WindowErrorCode::OperationFailed, result.details.empty() ? string_view{"Window effect failed."} : result.details});
    }

expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        return window_result_from_effect_result(enable_native_window_effect(handle, effect));
    }

expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept {
        return set_native_window_effect(handle, WindowEffect::blur(enabled));
    }

} // namespace SFT::Platform::Windowing
