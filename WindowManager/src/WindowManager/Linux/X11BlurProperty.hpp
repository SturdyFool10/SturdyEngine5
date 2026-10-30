#pragma once

#include <optional>

#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>

namespace SFT::WindowManager::Detail {

    /// Enables or removes background blur on an X11 window through the `_KDE_NET_WM_BLUR_BEHIND_REGION` property
    /// (read by KWin and other Plasma-aware compositors). The region is `effect.options` resolved to device pixels;
    /// no region means the whole window. Xlib is loaded at run time (SDL already has it loaded), so there is no link
    /// dependency on libX11.
    [[nodiscard]] WindowEffectResult set_x11_kde_blur(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Sets or clears `_NET_WM_WINDOW_OPACITY` (whole-window opacity, `effect.options.opacity`).
    [[nodiscard]] WindowEffectResult set_x11_window_opacity(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Rounds the window outline (`effect.options.window_corner_radius`) with the X Shape extension, or resets it to a
    /// rectangle. The edges are hard (the shape is a 1-bit mask).
    [[nodiscard]] WindowEffectResult set_x11_corner_radius(NativeWindowHandle handle, WindowEffect effect) noexcept;

    /// Window behaviours built on X properties and requests; each is a no-op request the window manager may ignore.
    /// Click-through: an empty (or restored) X Shape input region.
    [[nodiscard]] WindowEffectResult set_x11_mouse_passthrough(NativeWindowHandle handle, bool enabled) noexcept;
    /// The ICCCM `WM_HINTS` input flag: false asks the window manager never to focus the window.
    [[nodiscard]] WindowEffectResult set_x11_focusable(NativeWindowHandle handle, bool focusable) noexcept;
    /// `XGrabKeyboard`: every key event goes to the window, window-manager shortcuts included.
    [[nodiscard]] WindowEffectResult set_x11_keyboard_grab(NativeWindowHandle handle, bool enabled) noexcept;
    /// `WM_TRANSIENT_FOR` (and `_NET_WM_STATE_MODAL` when `modal`); an empty parent detaches.
    [[nodiscard]] WindowEffectResult set_x11_window_parent(NativeWindowHandle child, std::optional<NativeWindowHandle> parent,
                                                           bool modal) noexcept;

    /// `_NET_WM_WINDOW_TYPE`: what the window is for (palette, tooltip, popup menu, dialog, splash).
    [[nodiscard]] WindowEffectResult set_x11_window_type(NativeWindowHandle handle, WindowType type) noexcept;
    /// The X Shape bounding region from a coverage mask scaled to `pixel_size`; an empty mask restores the rectangle.
    [[nodiscard]] WindowEffectResult set_x11_window_shape(NativeWindowHandle handle, const WindowShapeMask &mask,
                                                          WindowExtent pixel_size) noexcept;

    /// Sets or clears `_GTK_THEME_VARIANT` ("dark") so window-manager-drawn decorations use the dark theme.
    [[nodiscard]] WindowEffectResult set_x11_dark_mode(NativeWindowHandle handle, bool enabled) noexcept;

    /// Whether the X11 blur route can be used from this process. X11 gives no way to ask whether the window manager
    /// reads the property, so a usable Xlib is reported `Unknown` rather than `Available`.
    [[nodiscard]] WindowEffectSupport query_x11_blur_support(NativeWindowHandle handle) noexcept;

} // namespace SFT::WindowManager::Detail
