#pragma once

#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>

namespace SFT::WindowManager::Detail {

    /// Sets the wayland background blur from the supplied value.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] WindowEffectResult set_wayland_background_blur(
        NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Sets or clears whole-surface opacity through `wp_alpha_modifier_v1` (`effect.options.opacity`).
    [[nodiscard]] WindowEffectResult set_wayland_window_opacity(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Click-through: an empty `wl_surface` input region (core protocol, so every compositor), or the whole surface again.
    [[nodiscard]] WindowEffectResult set_wayland_mouse_passthrough(NativeWindowHandle handle, bool enabled) noexcept;

    /// Whether the compositor advertises `wp_alpha_modifier_v1`.
    [[nodiscard]] WindowEffectSupport query_wayland_opacity_support(NativeWindowHandle handle) noexcept;

    /// Which Wayland blur protocols the compositor behind `handle` advertises right now.
    [[nodiscard]] WindowEffectSupport query_wayland_background_support(NativeWindowHandle handle) noexcept;

    /// Releases the per-surface effect objects; with `release_display` also the connection's own queue and globals.
    void release_wayland_background_effects(
        NativeWindowHandle handle, bool release_display) noexcept;

} // namespace SFT::WindowManager::Detail
