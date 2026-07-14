#include <Foundation/Foundation.hpp>

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

// Browser windows have no OS-level chrome to apply blur/acrylic/mica effects to and no native
// window handle in the X11/Wayland/Win32/Cocoa sense (a WebGPU surface is created from the HTML
// canvas element directly, not from a NativeWindowHandle) — every entry point here degrades
// gracefully to "unsupported" rather than modeling browser-specific behavior that does not exist.
namespace SFT::Platform::Windowing {

    OperatingSystem current_operating_system() noexcept {
        return OperatingSystem::Web;
    }

    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
        (void)effect;
        return false;
    }

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        (void)handle;
        Detail::window_warn(
            "Web native window effect requested but unsupported: kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("Window effects are not available for Web builds.");
    }

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing::Detail {

    expected<NativeWindowHandle, WindowError> native_window_handle_from_glfw(void *window_handle) noexcept {
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW is not built for Web; SDL3 is the only windowing backend."});
    }

    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "Web builds have no native window handle; create WebGPU surfaces from the HTML canvas instead."});
    }

} // namespace SFT::Platform::Windowing::Detail
