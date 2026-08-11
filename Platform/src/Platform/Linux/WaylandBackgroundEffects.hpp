#pragma once

#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowEffect.hpp>

namespace SFT::Platform::Windowing::Detail {

    [[nodiscard]] WindowEffectResult set_wayland_background_blur(
        NativeWindowHandle handle, WindowEffect effect) noexcept;

    void release_wayland_background_effects(
        NativeWindowHandle handle, bool release_display) noexcept;

} // namespace SFT::Platform::Windowing::Detail
