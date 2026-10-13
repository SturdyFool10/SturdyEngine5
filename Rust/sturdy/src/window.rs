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

    /// True when the clipboard offers `mime` (for example `"image/png"` or `"text/uri-list"`).
    pub fn clipboard_has(&mut self, mime: &str) -> bool {
        ffi::window_clipboard_has_mime(self.engine.view_mut(), mime)
    }

    /// The clipboard payload under `mime`; empty when absent or the provider has no typed clipboard.
    pub fn clipboard_data(&mut self, mime: &str) -> Vec<u8> {
        ffi::window_clipboard_data(self.engine.view_mut(), mime)
    }

    /// Replaces the clipboard with `bytes` under `mime` (the bytes are copied).
    pub fn set_clipboard_data(&mut self, mime: &str, bytes: &[u8]) -> Result<(), Error> {
        status(ffi::window_set_clipboard_data(self.engine.view_mut(), mime, bytes))
    }

    /// Paths of files on the clipboard (a `text/uri-list` of `file://` URIs), percent-decoded.
    pub fn clipboard_files(&mut self) -> Vec<std::path::PathBuf> {
        let raw = self.clipboard_data("text/uri-list");
        parse_uri_list(&String::from_utf8_lossy(&raw))
    }

    /// Puts file paths on the clipboard as a `text/uri-list`.
    pub fn set_clipboard_files(&mut self, paths: &[std::path::PathBuf]) -> Result<(), Error> {
        let list = uri_list(paths);
        self.set_clipboard_data("text/uri-list", list.as_bytes())
    }

    /// HDR state of the display the window is on, if the platform reports it.
    pub fn hdr_properties(&mut self) -> Option<HdrProperties> {
        let p = ffi::window_hdr_properties(self.engine.view_mut());
        p.available.then(|| HdrProperties { hdr_enabled: p.hdr_enabled, sdr_white_level: p.sdr_white_level, hdr_headroom: p.hdr_headroom })
    }

    // -- Custom title bar ------------------------------------------------------------------------

    /// Installs the drag and resize regions of a custom title bar (for an undecorated window). The OS then moves and
    /// resizes the window itself, so snapping, edge cursors and compositor resize all behave natively. Call it
    /// whenever the title bar layout changes; calling it every frame is fine.
    pub fn set_hit_test_regions(&mut self, regions: &HitTestRegions) -> Result<(), Error> {
        let flat = |rects: &[WindowRect]| rects.iter().flat_map(|r| [r.x, r.y, r.width, r.height]).collect();
        status(ffi::window_set_hit_test_regions(
            self.engine.view_mut(),
            &ffi::HitTestRegionsInfo {
                enabled: regions.enabled,
                resize_border: regions.resize_border,
                drag: flat(&regions.drag),
                no_drag: flat(&regions.no_drag),
            },
        ))
    }

    /// Opens the OS window menu (move/size/minimize/close) at `position` (window coordinates), as right-clicking a
    /// native title bar would. Windows and some Linux window managers.
    pub fn show_system_menu(&mut self, position: IVec2) -> Result<(), Error> {
        status(ffi::window_show_system_menu(self.engine.view_mut(), position.x, position.y))
    }

    pub fn is_maximized(&mut self) -> bool {
        ffi::window_is_maximized(self.engine.view_mut())
    }

    pub fn is_minimized(&mut self) -> bool {
        ffi::window_is_minimized(self.engine.view_mut())
    }

    /// Maximize if not maximized, restore otherwise (a custom title bar's maximize button).
    pub fn toggle_maximized(&mut self) -> Result<(), Error> {
        if self.is_maximized() { self.restore() } else { self.maximize() }
    }

    /// Window icon from tightly packed RGBA8 pixels (`width * height * 4` bytes). On Wayland the desktop file matching
    /// [`RuntimeConfig::app_id`](crate::RuntimeConfig::app_id) supplies the icon where the compositor ignores this.
    pub fn set_icon(&mut self, width: u32, height: u32, rgba: &[u8]) -> Result<(), Error> {
        status(ffi::window_set_icon(self.engine.view_mut(), width, height, rgba))
    }

    /// Ask for the user's attention (taskbar flash, dock bounce, urgency hint).
    pub fn request_attention(&mut self, request: AttentionRequest) -> Result<(), Error> {
        status(ffi::window_request_attention(self.engine.view_mut(), request as u8))
    }

    /// Taskbar / dock progress. `value` is 0..=1 and only used by `Normal`, `Paused` and `Error`.
    pub fn set_taskbar_progress(&mut self, state: TaskbarProgress, value: f32) -> Result<(), Error> {
        status(ffi::window_set_taskbar_progress(self.engine.view_mut(), state as u8, value))
    }

    /// The OS light/dark preference. A change arrives as a redraw (on-demand apps) and is visible here afterwards.
    pub fn system_theme(&mut self) -> SystemTheme {
        match ffi::window_system_theme(self.engine.view_mut()) {
            1 => SystemTheme::Light,
            2 => SystemTheme::Dark,
            _ => SystemTheme::Unknown,
        }
    }

    /// Every connected monitor.
    pub fn displays(&mut self) -> Vec<DisplayInfo> {
        ffi::window_displays(self.engine.view_mut())
            .into_iter()
            .map(|d| DisplayInfo {
                id: d.id,
                name: d.name,
                bounds: WindowRect { x: d.bounds[0], y: d.bounds[1], width: d.bounds[2], height: d.bounds[3] },
                usable_bounds: WindowRect {
                    x: d.usable_bounds[0],
                    y: d.usable_bounds[1],
                    width: d.usable_bounds[2],
                    height: d.usable_bounds[3],
                },
                content_scale: d.content_scale,
                refresh_rate_hz: d.refresh_rate_hz,
                hdr_enabled: d.hdr_enabled,
                primary: d.primary,
            })
            .collect()
    }
}

/// A rectangle in window coordinates (logical pixels).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct WindowRect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// Drag and resize regions of a custom title bar. See [`PrimaryWindow::set_hit_test_regions`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HitTestRegions {
    pub enabled: bool,
    /// Invisible resize border inside each edge, logical pixels (0 = not resizable from the edges).
    pub resize_border: u32,
    /// Areas that drag the window (the title bar).
    pub drag: Vec<WindowRect>,
    /// Areas inside `drag` that stay clickable (title bar buttons).
    pub no_drag: Vec<WindowRect>,
}

impl Default for HitTestRegions {
    fn default() -> Self {
        Self { enabled: true, resize_border: 6, drag: Vec::new(), no_drag: Vec::new() }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AttentionRequest {
    Cancel = 0,
    Briefly = 1,
    UntilFocused = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TaskbarProgress {
    None = 0,
    Indeterminate = 1,
    Normal = 2,
    Paused = 3,
    Error = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SystemTheme {
    Unknown,
    Light,
    Dark,
}

/// One monitor. See [`PrimaryWindow::displays`].
#[derive(Debug, Clone, PartialEq)]
pub struct DisplayInfo {
    pub id: u32,
    pub name: String,
    pub bounds: WindowRect,
    /// Bounds minus taskbars and docks.
    pub usable_bounds: WindowRect,
    pub content_scale: f32,
    /// 0 when unknown.
    pub refresh_rate_hz: f32,
    pub hdr_enabled: bool,
    pub primary: bool,
}

/// One step of a drag and drop onto a window. See [`Engine::take_drop_events`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DropEvent {
    /// A drag entered the window.
    Begin { window_id: u64 },
    /// The drag moved (window coordinates); use it for a hover preview.
    Hover { window_id: u64, position: IVec2 },
    /// A file was dropped.
    File { window_id: u64, path: std::path::PathBuf },
    /// Text was dropped.
    Text { window_id: u64, text: String },
    /// The drop finished (or the drag left the window).
    Complete { window_id: u64 },
}

impl Engine<'_> {
    /// Drains the drag-and-drop steps since the last call, in order. Drops wake an on-demand app.
    pub fn take_drop_events(&mut self) -> Vec<DropEvent> {
        ffi::take_drop_events(self.view_mut())
            .into_iter()
            .map(|d| match d.kind {
                0 => DropEvent::Begin { window_id: d.window_id },
                1 => DropEvent::Hover { window_id: d.window_id, position: IVec2::new(d.x, d.y) },
                2 => DropEvent::File { window_id: d.window_id, path: std::path::PathBuf::from(d.data) },
                3 => DropEvent::Text { window_id: d.window_id, text: d.data },
                _ => DropEvent::Complete { window_id: d.window_id },
            })
            .collect()
    }
}

/// Parses a `text/uri-list` (RFC 2483): one URI per line, `#` comments, only `file://` entries are kept.
fn parse_uri_list(text: &str) -> Vec<std::path::PathBuf> {
    text.lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .filter_map(|line| line.strip_prefix("file://"))
        // `file://host/path` / `file:///path`: drop an optional authority.
        .map(|rest| rest.find('/').map_or("", |slash| &rest[slash..]))
        .filter(|path| !path.is_empty())
        .map(|path| std::path::PathBuf::from(percent_decode(path)))
        .collect()
}

fn percent_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            // `get` rejects ranges that split a multi-byte character, so malformed input cannot panic.
            if let Some(value) = text.get(i + 1..i + 3).and_then(|hex| u8::from_str_radix(hex, 16).ok()) {
                out.push(value);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn percent_encode_path(path: &str) -> String {
    let mut out = String::with_capacity(path.len());
    for byte in path.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'/' | b'-' | b'_' | b'.' | b'~' => out.push(byte as char),
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

/// Builds a `text/uri-list` with CRLF line endings from absolute paths.
fn uri_list(paths: &[std::path::PathBuf]) -> String {
    paths
        .iter()
        .map(|path| format!("file://{}\r\n", percent_encode_path(&path.to_string_lossy())))
        .collect()
}

#[cfg(test)]
mod clipboard_tests {
    use super::*;

    #[test]
    fn uri_list_round_trips_awkward_paths() {
        let paths = vec![
            std::path::PathBuf::from("/home/u/My Games/a#b%c.png"),
            std::path::PathBuf::from("/tmp/ünï.txt"),
        ];
        assert_eq!(parse_uri_list(&uri_list(&paths)), paths);
    }

    #[test]
    fn uri_list_skips_comments_and_foreign_schemes() {
        let text = "# comment\r\nhttps://example.com/x\r\nfile://localhost/tmp/a%20b\r\n\r\nfile:///tmp/c\n";
        assert_eq!(
            parse_uri_list(text),
            vec![std::path::PathBuf::from("/tmp/a b"), std::path::PathBuf::from("/tmp/c")]
        );
    }
}
