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

    OperatingSystem current_operating_system() noexcept {
        return OperatingSystem::Web;
    }

    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
        (void)effect;
        return false;
    }

    void release_native_window_effects(NativeWindowHandle handle, bool release_display) noexcept {
        (void)handle;
        (void)release_display;
    }

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


    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "Web builds have no native window handle; create WebGPU surfaces from the HTML canvas instead."});
    }

} // namespace SFT::Platform::Windowing::Detail
