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

    /// Releases wayland background effects using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param release_display `release_display` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_wayland_background_effects(
        NativeWindowHandle handle, bool release_display) noexcept;

} // namespace SFT::WindowManager::Detail
