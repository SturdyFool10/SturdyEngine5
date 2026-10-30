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
                current_mode: mode_from(d.current_mode[0], d.current_mode[1], d.current_refresh_hz, d.current_pixel_density),
                modes: d
                    .modes
                    .chunks_exact(2)
                    .zip(d.mode_refresh_hz.iter().zip(d.mode_pixel_density.iter()))
                    .map(|(size, (refresh, density))| mode_from(size[0], size[1], *refresh, *density))
                    .collect(),
                physical_size_mm: (d.physical_size_mm[0], d.physical_size_mm[1]),
                orientation: match d.orientation {
                    1 => DisplayOrientation::Landscape,
                    2 => DisplayOrientation::LandscapeFlipped,
                    3 => DisplayOrientation::Portrait,
                    4 => DisplayOrientation::PortraitFlipped,
                    _ => DisplayOrientation::Unknown,
                },
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
    /// The mode the desktop is running in.
    pub current_mode: DisplayMode,
    /// Every mode a fullscreen window may ask for on this display.
    pub modes: Vec<DisplayMode>,
    /// Panel size in millimetres; `(0, 0)` when the provider cannot tell.
    pub physical_size_mm: (u32, u32),
    pub orientation: DisplayOrientation,
}

/// One video mode of a display.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DisplayMode {
    pub width: u32,
    pub height: u32,
    pub refresh_rate_hz: f32,
    pub pixel_density: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DisplayOrientation {
    Unknown,
    Landscape,
    LandscapeFlipped,
    Portrait,
    PortraitFlipped,
}

/// The display and mode a fullscreen window uses. `None` means "the window's current display" / "the desktop mode".
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct FullscreenTarget {
    pub display_id: Option<u32>,
    pub mode: Option<DisplayMode>,
}

/// A capability a window may or may not have on this provider, OS and session type. The window layer is the union of
/// what its providers (SDL3, GLFW) can do, the way an RHI is the union of its backends: ask first with
/// [`PrimaryWindow::feature`], or just try and read the [`Unsupported`](WindowOperationError::Unsupported) verdict.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum WindowFeature {
    AlwaysOnTop = 0,
    AspectRatio,
    MousePassthrough,
    Focusable,
    AutoMinimizeOnFocusLoss,
    ParentWindow,
    ModalWindow,
    KeyboardGrab,
    MouseConfineRect,
    WarpCursor,
    CustomCursor,
    FrameExtents,
    SafeArea,
    FullscreenTarget,
    TextInputOptions,
    ScreenKeyboard,
    ClipboardMimeTypes,
    TypedClipboard,
    HitTestRegions,
    SystemMenu,
    WindowIcon,
    RequestAttention,
    TaskbarProgress,
    SystemTheme,
    DisplayList,
    DisplayModes,
    DisplayEvents,
    LiveResizeCallback,
    WaitForEvents,
    FileDrop,
    TextDrop,
    WindowTypeHint,
    WindowShape,
    AnimatedCursor,
    KeyboardQuery,
    WarpCursorGlobal,
    MouseCapture,
    IccProfile,
    SystemTray,
    TouchInput,
    PenInput,
}

/// Whether a feature is available, with the reason when it is not (or only partly).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FeatureSupport {
    pub supported: bool,
    /// Why not, or what is reduced when it is partial; empty when fully supported.
    pub note: String,
}

impl From<ffi::FeatureInfo> for FeatureSupport {
    fn from(info: ffi::FeatureInfo) -> Self {
        Self { supported: info.supported, note: info.note }
    }
}

/// How the window manager should treat a window.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum WindowType {
    #[default]
    Normal = 0,
    /// A tool palette: no taskbar entry, lighter frame.
    Utility,
    Tooltip,
    PopupMenu,
    DropdownMenu,
    Dialog,
    Splash,
}

/// What kind of text a field takes, for IMEs and on-screen keyboards.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TextInputOptions {
    pub kind: TextInputKind,
    pub capitalization: TextCapitalization,
    pub autocorrect: bool,
    pub multiline: bool,
}

impl Default for TextInputOptions {
    fn default() -> Self {
        Self { kind: TextInputKind::Text, capitalization: TextCapitalization::Sentences, autocorrect: true, multiline: false }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextInputKind {
    Text = 0,
    Name,
    Email,
    Username,
    PasswordHidden,
    PasswordVisible,
    Number,
    NumberPasswordHidden,
    NumberPasswordVisible,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextCapitalization {
    None = 0,
    Sentences,
    Words,
    Letters,
}

/// Decoration thickness around the client area.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FrameExtents {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

/// Frame, safe area and display of a window.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowGeometry {
    pub frame_extents: FrameExtents,
    /// The part of the client area not under notches or system bars.
    pub safe_area: WindowRect,
    /// The display the window is mostly on (`DisplayInfo::id`), when the provider can say.
    pub display_id: Option<u32>,
    pub has_screen_keyboard: bool,
    pub screen_keyboard_shown: bool,
}

/// One frame of an animated cursor: tightly packed RGBA8 pixels (`width * height * 4` bytes).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CursorFrame {
    pub rgba: Vec<u8>,
    pub duration_ms: u32,
}

/// Why a window operation did not happen. `Unsupported` is the provider or system saying it cannot, which is not a bug.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WindowOperationError {
    Unsupported(String),
    InvalidArgument(String),
    Failed(String),
}

/// A window operation, for [`Engine::window_command`] on any window (the primary window also has typed methods).
/// Each variant is one `Window` method; results arrive through [`Engine::window_operation_result`].
#[derive(Debug, Clone, PartialEq)]
pub enum WindowCommand {
    SetTitle(String),
    SetPosition(IVec2),
    SetSize(UVec2),
    SetMinimumSize(UVec2),
    SetMaximumSize(UVec2),
    SetResizable(bool),
    SetOpacity(f32),
    Maximize,
    Minimize,
    Restore,
    Raise,
    Focus,
    Show,
    Hide,
    SetAlwaysOnTop(bool),
    SetAspectRatio { min: f32, max: f32 },
    SetMousePassthrough(bool),
    SetFocusable(bool),
    SetAutoMinimizeOnFocusLoss(bool),
    /// Another window's id, or `None` to detach.
    SetParent { parent: Option<u64>, modal: bool },
    SetKeyboardGrab(bool),
    SetMouseConfineRect(Option<WindowRect>),
    SetCursorPosition(IVec2),
    SetGlobalCursorPosition(IVec2),
    SetMouseCapture(bool),
    SetCustomCursor { width: u32, height: u32, rgba: Vec<u8>, hot_spot: IVec2 },
    SetAnimatedCursor { width: u32, height: u32, frames: Vec<CursorFrame>, hot_spot: IVec2 },
    ClearCustomCursor,
    StartTextInput(TextInputOptions),
    SetFullscreenTarget(FullscreenTarget),
    SetWindowType(WindowType),
    /// Coverage bytes (above 127 = inside), `width * height` of them; empty restores the rectangle.
    SetShape { width: u32, height: u32, coverage: Vec<u8> },
    SetDisplayGamma { display_id: Option<u32>, gamma: f32 },
    SetIcon { width: u32, height: u32, rgba: Vec<u8> },
    RequestAttention(AttentionRequest),
    SetTaskbarProgress { state: TaskbarProgress, value: f32 },
    SetHitTestRegions(HitTestRegions),
    ShowSystemMenu(IVec2),
    /// Queries: read the answer with [`Engine::window_operation_result`] a frame later.
    QueryClipboardText,
    SetClipboardText(String),
    QueryClipboardMimeTypes,
    QueryClipboardData(String),
    SetClipboardData { mime: String, bytes: Vec<u8> },
    QueryKeyName(i32),
    QueryKeyDown(i32),
    QueryIccProfile,
}

impl WindowCommand {
    /// `Engine::WindowOperation` of this command.
    pub fn operation(&self) -> WindowOperation {
        use WindowCommand::*;
        match self {
            SetTitle(_) => WindowOperation::Title,
            SetPosition(_) => WindowOperation::Position,
            SetSize(_) => WindowOperation::Size,
            SetMinimumSize(_) => WindowOperation::MinimumSize,
            SetMaximumSize(_) => WindowOperation::MaximumSize,
            SetResizable(_) => WindowOperation::Resizable,
            SetOpacity(_) => WindowOperation::Opacity,
            Maximize => WindowOperation::Maximize,
            Minimize => WindowOperation::Minimize,
            Restore => WindowOperation::Restore,
            Raise => WindowOperation::Raise,
            Focus => WindowOperation::Focus,
            Show => WindowOperation::Show,
            Hide => WindowOperation::Hide,
            SetAlwaysOnTop(_) => WindowOperation::AlwaysOnTop,
            SetAspectRatio { .. } => WindowOperation::AspectRatio,
            SetMousePassthrough(_) => WindowOperation::MousePassthrough,
            SetFocusable(_) => WindowOperation::Focusable,
            SetAutoMinimizeOnFocusLoss(_) => WindowOperation::AutoMinimize,
            SetParent { .. } => WindowOperation::Parent,
            SetKeyboardGrab(_) => WindowOperation::KeyboardGrab,
            SetMouseConfineRect(_) => WindowOperation::MouseConfineRect,
            SetCursorPosition(_) => WindowOperation::CursorPosition,
            SetGlobalCursorPosition(_) => WindowOperation::GlobalCursorPosition,
            SetMouseCapture(_) => WindowOperation::MouseCapture,
            SetCustomCursor { .. } => WindowOperation::CustomCursor,
            SetAnimatedCursor { .. } => WindowOperation::AnimatedCursor,
            ClearCustomCursor => WindowOperation::ClearCustomCursor,
            StartTextInput(_) => WindowOperation::TextInputOptions,
            SetFullscreenTarget(_) => WindowOperation::FullscreenTarget,
            SetWindowType(_) => WindowOperation::WindowType,
            SetShape { .. } => WindowOperation::Shape,
            SetDisplayGamma { .. } => WindowOperation::DisplayGamma,
            SetIcon { .. } => WindowOperation::Icon,
            RequestAttention(_) => WindowOperation::Attention,
            SetTaskbarProgress { .. } => WindowOperation::TaskbarProgress,
            SetHitTestRegions(_) => WindowOperation::HitTestRegions,
            ShowSystemMenu(_) => WindowOperation::SystemMenu,
            QueryClipboardText => WindowOperation::ClipboardText,
            SetClipboardText(_) => WindowOperation::SetClipboardText,
            QueryClipboardMimeTypes => WindowOperation::ClipboardMimeTypes,
            QueryClipboardData(_) => WindowOperation::ClipboardData,
            SetClipboardData { .. } => WindowOperation::SetClipboardData,
            QueryKeyName(_) => WindowOperation::KeyName,
            QueryKeyDown(_) => WindowOperation::KeyDown,
            QueryIccProfile => WindowOperation::IccProfile,
        }
    }

    fn to_ffi(&self) -> ffi::WindowCommand {
        use WindowCommand::*;
        let mut c = ffi::WindowCommand { op: self.operation() as u32, ..Default::default() };
        let flat = |rects: &[WindowRect]| rects.iter().flat_map(|r| [r.x, r.y, r.width, r.height]).collect::<Vec<_>>();
        match self {
            SetTitle(text) | SetClipboardText(text) | QueryClipboardData(text) => c.text = text.clone(),
            SetPosition(p) | SetCursorPosition(p) | SetGlobalCursorPosition(p) | ShowSystemMenu(p) => {
                c.a = p.x as i64;
                c.b = p.y as i64;
            }
            SetSize(s) | SetMinimumSize(s) | SetMaximumSize(s) => {
                c.a = s.x as i64;
                c.b = s.y as i64;
            }
            SetResizable(f) | SetAlwaysOnTop(f) | SetMousePassthrough(f) | SetFocusable(f) | SetAutoMinimizeOnFocusLoss(f) | SetKeyboardGrab(f)
            | SetMouseCapture(f) => c.flag = *f,
            SetOpacity(v) => c.x = *v,
            SetAspectRatio { min, max } => {
                c.x = *min;
                c.y = *max;
            }
            SetParent { parent, modal } => {
                c.flag = *modal;
                c.has_other = parent.is_some();
                c.other_window = parent.unwrap_or(0);
            }
            SetMouseConfineRect(rect) => {
                if let Some(r) = rect {
                    c.flag = true;
                    (c.a, c.b, c.c, c.d) = (r.x as i64, r.y as i64, r.width as i64, r.height as i64);
                }
            }
            SetCustomCursor { width, height, rgba, hot_spot } => {
                (c.a, c.b, c.c, c.d) = (*width as i64, *height as i64, hot_spot.x as i64, hot_spot.y as i64);
                c.bytes = rgba.clone();
            }
            SetAnimatedCursor { width, height, frames, hot_spot } => {
                (c.a, c.b, c.c, c.d) = (*width as i64, *height as i64, hot_spot.x as i64, hot_spot.y as i64);
                c.bytes = frames.iter().flat_map(|f| f.rgba.iter().copied()).collect();
                c.durations = frames.iter().map(|f| f.duration_ms).collect();
            }
            StartTextInput(o) => {
                c.a = o.kind as i64;
                c.b = o.capitalization as i64;
                c.flag = o.autocorrect;
                c.c = o.multiline as i64;
            }
            SetFullscreenTarget(t) => {
                c.a = t.display_id.map_or(-1, |id| id as i64);
                if let Some(m) = t.mode {
                    (c.b, c.c, c.x, c.y) = (m.width as i64, m.height as i64, m.refresh_rate_hz, m.pixel_density);
                }
            }
            SetWindowType(t) => c.a = *t as i64,
            SetShape { width, height, coverage } => {
                (c.a, c.b) = (*width as i64, *height as i64);
                c.bytes = coverage.clone();
            }
            SetDisplayGamma { display_id, gamma } => {
                c.a = display_id.map_or(-1, |id| id as i64);
                c.x = *gamma;
            }
            SetIcon { width, height, rgba } => {
                (c.a, c.b) = (*width as i64, *height as i64);
                c.bytes = rgba.clone();
            }
            RequestAttention(r) => c.a = *r as i64,
            SetTaskbarProgress { state, value } => {
                c.a = *state as i64;
                c.x = *value;
            }
            SetHitTestRegions(r) => {
                c.flag = r.enabled;
                c.a = r.resize_border as i64;
                c.rects = flat(&r.drag);
                c.no_drag_rects = flat(&r.no_drag);
            }
            SetClipboardData { mime, bytes } => {
                c.text = mime.clone();
                c.bytes = bytes.clone();
            }
            QueryKeyName(key) | QueryKeyDown(key) => c.a = *key as i64,
            Maximize | Minimize | Restore | Raise | Focus | Show | Hide | ClearCustomCursor | QueryClipboardText | QueryClipboardMimeTypes | QueryIccProfile => {}
        }
        c
    }
}

/// `Engine::WindowOperation`: each operation a window can be asked to run; the key to [`Engine::window_operation_result`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum WindowOperation {
    Title = 0,
    Position,
    Size,
    MinimumSize,
    MaximumSize,
    Resizable,
    Opacity,
    Maximize,
    Minimize,
    Restore,
    Raise,
    Focus,
    Show,
    Hide,
    AlwaysOnTop,
    AspectRatio,
    MousePassthrough,
    Focusable,
    AutoMinimize,
    Parent,
    KeyboardGrab,
    MouseConfineRect,
    CursorPosition,
    GlobalCursorPosition,
    MouseCapture,
    CustomCursor,
    AnimatedCursor,
    ClearCustomCursor,
    TextInputOptions,
    FullscreenTarget,
    WindowType,
    Shape,
    DisplayGamma,
    Icon,
    Attention,
    TaskbarProgress,
    HitTestRegions,
    SystemMenu,
    ClipboardText,
    SetClipboardText,
    ClipboardMimeTypes,
    ClipboardData,
    SetClipboardData,
    KeyName,
    KeyDown,
    IccProfile,
    /// A file dialog parented to the window ([`FileDialog::open_for`](crate::desktop::FileDialog::open_for)).
    FileDialog,
    /// A message box over the window ([`Engine::show_message_box_for`]); the pressed button is [`OperationResult::number`].
    MessageBox,
}

/// The last outcome of one operation on one window, with a query's output.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OperationResult {
    pub outcome: Result<(), WindowOperationError>,
    /// A query's boolean answer (`QueryKeyDown`; whether `QueryClipboardData`'s format was present).
    pub flag: bool,
    /// A numeric answer: the index of the button a message box returned (-1 when closed another way).
    pub number: i32,
    pub text: String,
    pub bytes: Vec<u8>,
    pub list: Vec<String>,
}

impl Engine<'_> {
    /// Queues `command` for any window; it runs on the window's own thread. Read the outcome (and a query's answer) with
    /// [`window_operation_result`](Self::window_operation_result) a frame later.
    pub fn window_command(&mut self, window_id: u64, command: &WindowCommand) -> Result<(), Error> {
        status(ffi::window_run_command(self.view_mut(), window_id, &command.to_ffi()))
    }

    /// The last outcome of `operation` on `window_id`; `None` until it has run.
    pub fn window_operation_result(&mut self, window_id: u64, operation: WindowOperation) -> Option<OperationResult> {
        let r = ffi::window_operation_result(self.view_mut(), window_id, operation as u32);
        if !r.completed {
            return None;
        }
        let outcome = match r.error_kind {
            _ if r.ok => Ok(()),
            1 => Err(WindowOperationError::Unsupported(r.message)),
            2 => Err(WindowOperationError::InvalidArgument(r.message)),
            _ => Err(WindowOperationError::Failed(r.message)),
        };
        Some(OperationResult { outcome, flag: r.flag, number: r.number, text: r.text, bytes: r.bytes, list: r.list })
    }

    /// Shows a message box over a window (see [`crate::desktop::MessageBox`]). Queued: it blocks that window's thread until
    /// dismissed, and the pressed button arrives as [`OperationResult::number`] of [`WindowOperation::MessageBox`].
    pub fn show_message_box_for(&mut self, window_id: u64, options: &crate::desktop::MessageBox) -> Result<(), Error> {
        status(ffi::window_show_message_box(
            self.view_mut(),
            window_id,
            options.kind as u8,
            &options.title,
            &options.message,
            &options.buttons,
            options.default_button,
            options.escape_button,
        ))
    }

    /// What any window can do, from the engine's snapshot of it.
    pub fn window_feature(&mut self, window_id: u64, feature: WindowFeature) -> FeatureSupport {
        ffi::window_feature_of(self.view_mut(), window_id, feature as u32).into()
    }
}

fn mode_from(width: u32, height: u32, refresh: f32, density: f32) -> DisplayMode {
    DisplayMode { width, height, refresh_rate_hz: refresh, pixel_density: density }
}

fn target_to_ffi(target: &FullscreenTarget) -> ffi::FullscreenTargetInfo {
    let mode = target.mode.unwrap_or(mode_from(0, 0, 0.0, 1.0));
    ffi::FullscreenTargetInfo {
        has_display: target.display_id.is_some(),
        display_id: target.display_id.unwrap_or(0),
        has_mode: target.mode.is_some(),
        width: mode.width,
        height: mode.height,
        refresh_hz: mode.refresh_rate_hz,
        pixel_density: mode.pixel_density,
    }
}

/// The union surface of the primary window: behaviour the providers share or back each other up on. Every method
/// reports `Err` with the provider's reason when it cannot (use [`feature`](Self::feature) to ask first).
impl PrimaryWindow<'_, '_> {
    /// Whether, and how fully, this window supports `feature`.
    pub fn feature(&mut self, feature: WindowFeature) -> FeatureSupport {
        ffi::window_feature(self.engine.view_mut(), feature as u32).into()
    }

    /// Frame, safe area and display.
    pub fn geometry(&mut self) -> WindowGeometry {
        let g = ffi::window_geometry(self.engine.view_mut());
        WindowGeometry {
            frame_extents: FrameExtents { left: g.frame_extents[0], top: g.frame_extents[1], right: g.frame_extents[2], bottom: g.frame_extents[3] },
            safe_area: WindowRect { x: g.safe_area[0], y: g.safe_area[1], width: g.safe_area[2], height: g.safe_area[3] },
            display_id: g.has_display.then_some(g.display_id),
            has_screen_keyboard: g.has_screen_keyboard,
            screen_keyboard_shown: g.screen_keyboard_shown,
        }
    }

    pub fn set_always_on_top(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_always_on_top(self.engine.view_mut(), enabled))
    }

    /// Constrains resizing to width/height ratios in `[min, max]`; `0, 0` removes the constraint. GLFW holds one exact ratio.
    pub fn set_aspect_ratio(&mut self, min: f32, max: f32) -> Result<(), Error> {
        status(ffi::window_set_aspect_ratio(self.engine.view_mut(), min, max))
    }

    /// Mouse clicks fall through the window to what is behind it.
    pub fn set_mouse_passthrough(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_mouse_passthrough(self.engine.view_mut(), enabled))
    }

    /// A non-focusable window never takes keyboard focus (overlays, palettes).
    pub fn set_focusable(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_focusable(self.engine.view_mut(), enabled))
    }

    pub fn set_auto_minimize_on_focus_loss(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_auto_minimize_on_focus_loss(self.engine.view_mut(), enabled))
    }

    /// Makes this window a child of another window (by id); `None` detaches. A modal child blocks its parent where the
    /// system allows.
    pub fn set_parent(&mut self, parent_window: Option<u64>, modal: bool) -> Result<(), Error> {
        status(ffi::window_set_parent(self.engine.view_mut(), parent_window.unwrap_or(u64::MAX), modal))
    }

    /// Routes all keyboard input here, including window-manager shortcuts.
    pub fn set_keyboard_grab(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_keyboard_grab(self.engine.view_mut(), enabled))
    }

    /// Confines the cursor to a client-area rectangle; `None` releases it.
    pub fn set_mouse_confine_rect(&mut self, rect: Option<WindowRect>) -> Result<(), Error> {
        let r = rect.unwrap_or_default();
        status(ffi::window_set_mouse_confine_rect(self.engine.view_mut(), rect.is_some(), r.x, r.y, r.width, r.height))
    }

    /// Moves the cursor within the window (window coordinates).
    pub fn set_cursor_position(&mut self, position: IVec2) -> Result<(), Error> {
        status(ffi::window_set_cursor_position(self.engine.view_mut(), position.x, position.y))
    }

    /// Moves the cursor on the desktop.
    pub fn set_global_cursor_position(&mut self, position: IVec2) -> Result<(), Error> {
        status(ffi::window_set_global_cursor_position(self.engine.view_mut(), position.x, position.y))
    }

    /// Keeps delivering mouse events while a button is held after the cursor leaves the window.
    pub fn set_mouse_capture(&mut self, enabled: bool) -> Result<(), Error> {
        status(ffi::window_set_mouse_capture(self.engine.view_mut(), enabled))
    }

    /// A cursor drawn from an RGBA8 image (`width * height * 4` bytes), with its hot spot in image pixels.
    pub fn set_custom_cursor(&mut self, width: u32, height: u32, rgba: &[u8], hot_spot: IVec2) -> Result<(), Error> {
        status(ffi::window_set_custom_cursor(self.engine.view_mut(), width, height, rgba, hot_spot.x, hot_spot.y))
    }

    /// A cursor cycling through same-sized frames; the hot spot is shared by every frame.
    pub fn set_animated_cursor(&mut self, width: u32, height: u32, frames: &[CursorFrame], hot_spot: IVec2) -> Result<(), Error> {
        let pixels: Vec<u8> = frames.iter().flat_map(|f| f.rgba.iter().copied()).collect();
        let durations: Vec<u32> = frames.iter().map(|f| f.duration_ms).collect();
        status(ffi::window_set_animated_cursor(self.engine.view_mut(), width, height, &pixels, &durations, hot_spot.x, hot_spot.y))
    }

    /// Goes back to the shape set with [`Engine::set_cursor_icon`].
    pub fn clear_custom_cursor(&mut self) -> Result<(), Error> {
        status(ffi::window_clear_custom_cursor(self.engine.view_mut()))
    }

    /// A non-rectangular outline from `width * height` coverage bytes (above 127 = inside); an empty slice restores the
    /// rectangle. Hard-edged: the outline is a 1-bit region.
    pub fn set_shape(&mut self, width: u32, height: u32, coverage: &[u8]) -> Result<(), Error> {
        status(ffi::window_set_shape(self.engine.view_mut(), width, height, coverage))
    }

    /// Tells the window manager what the window is for (palette, tooltip, popup menu, dialog, splash).
    pub fn set_window_type(&mut self, window_type: WindowType) -> Result<(), Error> {
        status(ffi::window_set_window_type(self.engine.view_mut(), window_type as u8))
    }

    /// Sets a display's gamma (1.0 = unchanged); `None` uses the window's display.
    pub fn set_display_gamma(&mut self, display_id: Option<u32>, gamma: f32) -> Result<(), Error> {
        status(ffi::window_set_display_gamma(self.engine.view_mut(), display_id.is_some(), display_id.unwrap_or(0), gamma))
    }

    /// The display and mode `set_fullscreen` uses from now on; applies immediately when already fullscreen.
    pub fn set_fullscreen_target(&mut self, target: FullscreenTarget) -> Result<(), Error> {
        status(ffi::window_set_fullscreen_target(self.engine.view_mut(), &target_to_ffi(&target)))
    }

    /// Starts text input telling the IME / on-screen keyboard what is being entered.
    pub fn start_text_input_with(&mut self, options: TextInputOptions) -> Result<(), Error> {
        status(ffi::window_start_text_input_with(
            self.engine.view_mut(),
            options.kind as u8,
            options.capitalization as u8,
            options.autocorrect,
            options.multiline,
        ))
    }

    /// The clipboard formats currently available.
    pub fn clipboard_mime_types(&mut self) -> Vec<String> {
        ffi::window_clipboard_mime_types(self.engine.view_mut())
    }

    /// The layout's name for a key (`key` is the `key_code` value input events report); empty when it has none.
    pub fn key_name(&mut self, key: i32) -> String {
        ffi::window_key_name(self.engine.view_mut(), key)
    }

    /// Whether a key is held right now.
    pub fn key_down(&mut self, key: i32) -> bool {
        ffi::window_key_down(self.engine.view_mut(), key)
    }

    /// The colour profile (ICC bytes) of the display the window is on.
    pub fn icc_profile(&mut self) -> Result<Vec<u8>, Error> {
        let r = ffi::window_icc_profile(self.engine.view_mut());
        if r.ok { Ok(r.bytes) } else { Err(Error::new(r.error)) }
    }
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
mod union_tests {
    use super::*;

    #[test]
    fn enum_ordinals_match_the_engine() {
        // The Rust enums are cast to the engine's ordinals, so they must end where the engine's end.
        assert_eq!(WindowFeature::PenInput as u32 + 1, ffi::window_feature_count());
        assert_eq!(WindowOperation::MessageBox as u32 + 1, ffi::window_operation_count());
    }

    #[test]
    fn every_command_names_its_own_operation() {
        let samples = [
            (WindowCommand::SetTitle("x".into()), WindowOperation::Title),
            (WindowCommand::SetAlwaysOnTop(true), WindowOperation::AlwaysOnTop),
            (WindowCommand::SetParent { parent: Some(3), modal: true }, WindowOperation::Parent),
            (WindowCommand::QueryKeyDown(4), WindowOperation::KeyDown),
            (WindowCommand::QueryIccProfile, WindowOperation::IccProfile),
        ];
        for (command, operation) in samples {
            assert_eq!(command.operation(), operation);
            assert_eq!(command.to_ffi().op, operation as u32);
        }
    }

    #[test]
    fn commands_carry_their_arguments() {
        let c = WindowCommand::SetMouseConfineRect(Some(WindowRect { x: 1, y: 2, width: 30, height: 40 })).to_ffi();
        assert!(c.flag);
        assert_eq!((c.a, c.b, c.c, c.d), (1, 2, 30, 40));
        assert!(!WindowCommand::SetMouseConfineRect(None).to_ffi().flag);

        let cursor = WindowCommand::SetAnimatedCursor {
            width: 2,
            height: 2,
            frames: vec![CursorFrame { rgba: vec![1; 16], duration_ms: 40 }, CursorFrame { rgba: vec![2; 16], duration_ms: 60 }],
            hot_spot: IVec2::new(1, 1),
        }
        .to_ffi();
        assert_eq!(cursor.bytes.len(), 32);
        assert_eq!(cursor.durations, vec![40, 60]);

        let target = WindowCommand::SetFullscreenTarget(FullscreenTarget {
            display_id: Some(2),
            mode: Some(DisplayMode { width: 1920, height: 1080, refresh_rate_hz: 144.0, pixel_density: 1.0 }),
        })
        .to_ffi();
        assert_eq!((target.a, target.b, target.c), (2, 1920, 1080));
        assert_eq!(WindowCommand::SetFullscreenTarget(FullscreenTarget::default()).to_ffi().a, -1);

        let regions = WindowCommand::SetHitTestRegions(HitTestRegions {
            enabled: true,
            resize_border: 6,
            drag: vec![WindowRect { x: 0, y: 0, width: 100, height: 20 }],
            no_drag: Vec::new(),
        })
        .to_ffi();
        assert_eq!(regions.rects, vec![0, 0, 100, 20]);
    }
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
