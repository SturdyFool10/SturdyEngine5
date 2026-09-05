#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <SDL3/SDL_video.h>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>
#include <WindowManager/WindowNative.hpp>

using std::expected;
using std::unexpected;


namespace SFT::WindowManager {

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

} // namespace SFT::WindowManager

namespace SFT::WindowManager::Detail {


    /// Performs the native window handle from SDL operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
        auto *window = static_cast<SDL_Window *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("SDL3 Web native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Web native handle requires a live window."});
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        // Despite its name, SDL already stores a full CSS selector here (it defaults to the literal
        // string "#canvas", not a bare id -- see SDL_emscriptenvideo.c's SDL_CreateWindow, which
        // reads SDL_PROP_WINDOW_CREATE_EMSCRIPTEN_CANVAS_ID_STRING with that same "#canvas" default
        // and stores it back verbatim). Do not prepend another '#'.
        const char *canvas_selector = SDL_GetStringProperty(properties, SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, nullptr);
        if (!canvas_selector || canvas_selector[0] == '\0') [[unlikely]] {
            Detail::window_error("SDL3 Web native handle missing canvas selector: sdl_window={} properties={}", static_cast<void *>(window), properties);
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Web native handle has no Emscripten canvas selector."});
        }

        // Lifetime: NativeWindowHandle::canvas_selector must stay valid for as long as the Window
        // that produced it (see WindowConfig.hpp). This static buffer satisfies that for the single
        // canvas a WASM build actually has; it is overwritten on the next call, which is fine because
        // nothing needs two live selectors from two different windows to compare simultaneously.
        static thread_local std::string selector_storage;
        selector_storage = canvas_selector;

        Detail::window_debug("SDL3 Web native handle resolved canvas: sdl_window={} properties={} selector={}", static_cast<void *>(window), properties, selector_storage);
        return NativeWindowHandle{
            NativeWindowSystem::Web,
            nullptr,
            nullptr,
            selector_storage.c_str(),
        };
    }

} // namespace SFT::WindowManager::Detail
