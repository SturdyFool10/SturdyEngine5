#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <optional>
#include <string_view>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowFeatures.hpp>
#include <WindowManager/WindowGeometry.hpp>

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
    /// Makes `child` a child (owner / transient-for) window of `parent`, or detaches it when `parent` is empty; `modal`
    /// asks for the parent to be blocked while the child exists. Implemented per OS on the native handles, so a provider
    /// without its own parent API gets it for free. May return `Degraded` (parent set, modality not enforced).
    [[nodiscard]] WindowEffectResult set_native_window_parent(NativeWindowHandle child, std::optional<NativeWindowHandle> parent,
                                                              bool modal) noexcept;

    /// Marks the window as a palette / tooltip / popup menu / dialog / splash (X11 `_NET_WM_WINDOW_TYPE`, Windows
    /// extended styles, macOS window level). Wayland has no client request for it.
    [[nodiscard]] WindowEffectResult set_native_window_type(NativeWindowHandle handle, WindowType type) noexcept;

    /// Gives the window a non-rectangular outline from `mask` scaled to `pixel_size`; an empty mask restores the
    /// rectangle. X11 uses the Shape extension, Windows a window region; macOS and Wayland have no equivalent.
    [[nodiscard]] WindowEffectResult set_native_window_shape(NativeWindowHandle handle, const WindowShapeMask &mask,
                                                             WindowExtent pixel_size) noexcept;

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

    /// Turns a coverage mask (see `WindowShapeMask`) into the rectangles of a `pixel_size` window it covers: the mask is
    /// scaled with nearest sampling, runs of covered pixels become rectangles, and identical runs on consecutive rows
    /// are merged into one tall rectangle. Empty for an empty mask or size.
    [[nodiscard]] std::vector<EffectRect> resolve_shape_region(const WindowShapeMask &mask, WindowExtent pixel_size);

} // namespace SFT::WindowManager
