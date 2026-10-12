#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string_view>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>

using std::expected;
using std::string_view;
using std::unexpected;

namespace SFT::WindowManager {

    /// Returns the current or globally available current operating system value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] OperatingSystem current_operating_system() noexcept;
    /// Performs the operating system may support window effect operation using the supplied arguments.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept;
    /// Enables native window effect using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;


    /// Releases native window effects using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param release_display `release_display` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_native_window_effects(NativeWindowHandle handle, bool release_display = false) noexcept;

    /// Performs the window result from effect result operation using the supplied arguments.
    ///
    /// @param result `result` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<void, WindowError> window_result_from_effect_result(WindowEffectResult result) noexcept;

    /// Sets the native window effect from the supplied value.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<void, WindowError> set_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Sets the native window blur enabled from the supplied value.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<void, WindowError> set_native_window_blur_enabled(NativeWindowHandle handle, bool enabled) noexcept;

    /// What `handle`'s window can do for `kind` on this OS / compositor, route by route. Changes nothing and creates
    /// no effect. Implemented per platform (Linux reads the compositor's advertised globals, Windows checks the OS
    /// build, macOS checks the AppKit classes).
    [[nodiscard]] WindowEffectSupport query_native_window_effect_support(NativeWindowHandle handle,
                                                                         WindowEffectKind kind) noexcept;

    /// The rectangles (window-local logical pixels) a blur request covers: its explicit rectangles, or the whole
    /// window when none are given, with each rounded by `options.corner_radius`. An empty result means "the whole
    /// window, square corners" (no region needed). Pure; the rounded corners are scanline rectangles.
    [[nodiscard]] std::vector<EffectRect> resolve_blur_region(const WindowEffectOptions &options);

} // namespace SFT::WindowManager
