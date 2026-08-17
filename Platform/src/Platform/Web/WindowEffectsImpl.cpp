#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#pragma endregion

#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowEffect.hpp>
#include <Platform/Window/WindowEffects.hpp>
#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/WindowNative.hpp>

using std::expected;
using std::unexpected;


namespace SFT::Platform::Windowing {

    /// Returns the current or globally available current operating system value.
    ///
    /// @return Returns the current current operating system value.
    /// @note This function does not throw exceptions.
    OperatingSystem current_operating_system() noexcept {
        return OperatingSystem::Web;
    }

    /// Performs the operating system may support window effect operation for `Windowing` using the supplied arguments.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
        (void)effect;
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
    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        (void)handle;
        if (!operating_system_may_support_window_effect(effect.kind)) [[likely]] {
            Detail::window_warn(
                "Web window effect kind={} enabled={} is not supported; no-op.",
                static_cast<int>(effect.kind),
                effect.enabled);
            return WindowEffectResult::failed("This window effect is not supported on the current OS.");
        }
        return WindowEffectResult::failed("Window effects are not available for Web builds.");
    }

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing::Detail {


    /// Performs the native window handle from SDL operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "Web builds have no native window handle; create WebGPU surfaces from the HTML canvas instead."});
    }

} // namespace SFT::Platform::Windowing::Detail
