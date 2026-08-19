#include <WindowManager/WindowEffects.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::WindowManager {

/// Performs the window result from effect result operation for `Windowing` using the supplied arguments.
///
/// @param result `result` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept {
        ZoneScopedN("Windowing::window_result_from_effect_result");
        if (result.succeeded()) [[likely]] {
            return {};
        }

        return unexpected(WindowError{WindowErrorCode::OperationFailed, result.details.empty() ? string_view{"Window effect failed."} : result.details});
    }

/// Sets the native window effect for this `Windowing`.
///
/// @param handle Handle identifying the target object or resource.
/// @param effect `effect` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_native_window_effect");
        return window_result_from_effect_result(enable_native_window_effect(handle, effect));
    }

/// Sets the native window blur enabled for this `Windowing`.
///
/// @param handle Handle identifying the target object or resource.
/// @param enabled Whether the associated behavior is enabled.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept {
        ZoneScopedN("Windowing::set_native_window_blur_enabled");
        return set_native_window_effect(handle, WindowEffect::blur(enabled));
    }

} // namespace SFT::WindowManager
