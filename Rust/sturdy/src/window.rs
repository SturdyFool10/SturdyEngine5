//! Direct control of the engine's **primary window**: title, size, position, size limits,
//! minimize/maximize/restore/raise/show/hide/focus, opacity, cursor visibility, clipboard, HDR
//! properties and close requests.
//!
//! These go straight to the platform window (unlike the queued operations in [`crate::render`]),
//! so they take effect immediately and must be called from the game-logic thread. Fullscreen /
//! decorated / transparent / cursor icon / grab / blur are queued operations on
//! [`Engine`](crate::Engine) — see [`crate::render`]. Secondary windows have no direct accessor in
//! the engine yet.

use glam::{IVec2, UVec2};
use sturdy_sys::window::ffi;

use crate::{Engine, Error};

/// The primary window; get it with [`Engine::primary_window_control`].
pub struct PrimaryWindow<'a, 'e> {
    engine: &'a mut Engine<'e>,
}

fn status(s: ffi::WindowStatus) -> Result<(), Error> {
    if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
}

fn int2(r: ffi::Int2Result) -> Option<IVec2> {
    r.ok.then(|| IVec2::new(r.x, r.y))
}

/// HDR state of the window's display.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HdrProperties {
    pub hdr_enabled: bool,
    /// The SDR reference white level as a multiple of 1.0 (e.g. 2.5 = 203 nits on a 80-nit scale).
    pub sdr_white_level: f32,
    /// How far above SDR white the display can go.
    pub hdr_headroom: f32,
}

impl<'e> Engine<'e> {
    /// Direct control of the primary window; `None` if the engine has none.
    pub fn primary_window_control(&mut self) -> Option<PrimaryWindow<'_, 'e>> {
        ffi::window_has_primary(self.view_mut()).then_some(PrimaryWindow { engine: self })
    }
}

impl PrimaryWindow<'_, '_> {
    /// Whether the user asked to close the window (close button, Alt+F4).
    pub fn close_requested(&mut self) -> bool {
        ffi::window_close_requested(self.engine.view_mut())
    }

    /// Asks the window to close, as if the user had.
    pub fn request_close(&mut self) {
        ffi::window_request_close(self.engine.view_mut());
    }

    pub fn show(&mut self) -> Result<(), Error> {
        status(ffi::window_show(self.engine.view_mut()))
    }

    pub fn hide(&mut self) -> Result<(), Error> {
        status(ffi::window_hide(self.engine.view_mut()))
    }

    /// Gives the window input focus.
    pub fn focus(&mut self) -> Result<(), Error> {
        status(ffi::window_focus(self.engine.view_mut()))
    }

    /// Raises it above other windows.
    pub fn raise(&mut self) -> Result<(), Error> {
        status(ffi::window_raise(self.engine.view_mut()))
    }

    pub fn maximize(&mut self) -> Result<(), Error> {
        status(ffi::window_maximize(self.engine.view_mut()))
    }

    pub fn minimize(&mut self) -> Result<(), Error> {
        status(ffi::window_minimize(self.engine.view_mut()))
    }

    /// Undoes minimize/maximize.
    pub fn restore(&mut self) -> Result<(), Error> {
        status(ffi::window_restore(self.engine.view_mut()))
    }

    pub fn set_title(&mut self, title: &str) -> Result<(), Error> {
        status(ffi::window_set_title(self.engine.view_mut(), title))
    }

    /// Screen position of the window's top-left corner.
    pub fn position(&mut self) -> Option<IVec2> {
        int2(ffi::window_position(self.engine.view_mut()))
    }

    pub fn set_position(&mut self, position: IVec2) -> Result<(), Error> {
        status(ffi::window_set_position(self.engine.view_mut(), position.x, position.y))
    }

    /// The mouse cursor in screen coordinates, even outside the window.
    pub fn global_cursor_position(&mut self) -> Option<IVec2> {
        int2(ffi::window_global_cursor_position(self.engine.view_mut()))
    }

    /// Client-area size in window units.
    pub fn size(&mut self) -> Option<UVec2> {
        int2(ffi::window_size(self.engine.view_mut())).map(|v| v.as_uvec2())
    }

    pub fn set_size(&mut self, size: UVec2) -> Result<(), Error> {
        status(ffi::window_set_size(self.engine.view_mut(), size.x, size.y))
    }

    pub fn set_minimum_size(&mut self, size: UVec2) -> Result<(), Error> {
        status(ffi::window_set_minimum_size(self.engine.view_mut(), size.x, size.y))
    }

    pub fn set_maximum_size(&mut self, size: UVec2) -> Result<(), Error> {
        status(ffi::window_set_maximum_size(self.engine.view_mut(), size.x, size.y))
    }

    pub fn set_resizable(&mut self, resizable: bool) -> Result<(), Error> {
        status(ffi::window_set_resizable(self.engine.view_mut(), resizable))
    }

    /// The window's current fullscreen mode. Change it with `Engine::set_window_mode`.
    pub fn fullscreen_mode(&mut self) -> crate::WindowMode {
        match ffi::window_fullscreen_mode(self.engine.view_mut()) {
            1 => crate::WindowMode::BorderlessFullscreen,
            2 => crate::WindowMode::ExclusiveFullscreen,
            _ => crate::WindowMode::Windowed,
        }
    }

    /// Whole-window opacity, 0.0 (invisible) to 1.0.
    pub fn set_opacity(&mut self, opacity: f32) -> Result<(), Error> {
        status(ffi::window_set_opacity(self.engine.view_mut(), opacity))
    }

    pub fn opacity(&mut self) -> Result<f32, Error> {
        let mut value = 1.0;
        status(ffi::window_opacity(self.engine.view_mut(), &mut value))?;
        Ok(value)
    }

    pub fn set_cursor_visible(&mut self, visible: bool) -> Result<(), Error> {
        status(ffi::window_set_cursor_visible(self.engine.view_mut(), visible))
    }

    /// The system clipboard's text.
    pub fn clipboard_text(&mut self) -> String {
        ffi::window_clipboard_text(self.engine.view_mut())
    }

    pub fn set_clipboard_text(&mut self, text: &str) -> Result<(), Error> {
        status(ffi::window_set_clipboard_text(self.engine.view_mut(), text))
    }

    /// HDR state of the display the window is on, if the platform reports it.
    pub fn hdr_properties(&mut self) -> Option<HdrProperties> {
        let p = ffi::window_hdr_properties(self.engine.view_mut());
        p.available.then(|| HdrProperties { hdr_enabled: p.hdr_enabled, sdr_white_level: p.sdr_white_level, hdr_headroom: p.hdr_headroom })
    }
}
