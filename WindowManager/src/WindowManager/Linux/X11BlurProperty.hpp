#pragma once

#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>

namespace SFT::WindowManager::Detail {

    /// Enables or removes background blur on an X11 window through the `_KDE_NET_WM_BLUR_BEHIND_REGION` property
    /// (read by KWin and other Plasma-aware compositors). The region is `effect.options` resolved to device pixels;
    /// no region means the whole window. Xlib is loaded at run time (SDL already has it loaded), so there is no link
    /// dependency on libX11.
    [[nodiscard]] WindowEffectResult set_x11_kde_blur(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Whether the X11 blur route can be used from this process. X11 gives no way to ask whether the window manager
    /// reads the property, so a usable Xlib is reported `Unknown` rather than `Available`.
    [[nodiscard]] WindowEffectSupport query_x11_blur_support(NativeWindowHandle handle) noexcept;

} // namespace SFT::WindowManager::Detail
