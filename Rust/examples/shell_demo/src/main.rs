//! A launcher-shaped immediate-mode app on SE5: the reference for porting Vertex.
//!
//! Everything is built every frame inside one draw hook, like UI Workbench does in C++. The engine renders only when
//! something asks (`FrameMode::OnDemand`), so an idle window costs ~0% CPU/GPU; `AnimationKeepAlive` keeps frames
//! coming while hover fades and scrollbars settle. Every control on the Settings tab applies live: there is no
//! "restart to apply" anywhere.
//!
//! * Custom title bar: undecorated window, drag/resize regions handed to the OS (snap layouts work).
//! * Home: progress bars, file drop, tooltip, right-click context menu, Markdown changelog with link clicks.
//! * Library: 50 000 rows, only the visible ones are built (`ui_kit::visible_rows`).
//! * Settings: graphics API + GPU switch, vsync, frame limiter, HDR, window opacity/blur, font family, text size,
//!   theme, taskbar progress.
//! * About: a modal dialog.
//!
//! Ctrl+1..4 switch tabs; Ctrl+Q quits.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use glam::{Vec2, Vec4};
use sturdy::app::FrameMode;
use sturdy::device::{Adapter, Backend, DevicePreference, DeviceRequest, SwitchOutcome};
use sturdy::markdown::{MarkdownEvent, MarkdownStyle, MarkdownView};
use sturdy::prelude::*;
use sturdy::render::{FrameRateLimit, LinuxBlurProtocol, SurfaceHandle, WindowEffectKind, WindowEffectOptions, WindowEffectParams};
use sturdy::ui::{
    self, AlignX, AlignY, Border, ChildAlignment, Clip, CornerRadius, Cursor, Direction, ElementDesc, FontSource, FontStack,
    Padding, Sizing, TextStyle, Ui,
};
use sturdy::ui_kit::{
    self, AnimationKeepAlive, MenuItem, ModalStyle, PopupState, PopupStyle, ProgressStyle, Shortcut, TabStyle, TooltipState, TooltipStyle,
};
use sturdy::widgets::{
    ButtonState, ButtonStyle, DropdownState, DropdownStyle, SliderConfig, SliderInput, SliderState, SliderStyle, TextEditState,
    TextInputStyle, ToggleState, ToggleStyle,
};
use sturdy::window::{DropEvent, HitTestRegions, TaskbarProgress, WindowRect};
use sturdy::{Key, MouseButton};

const CHANGELOG: &str = "\
# Shell Demo 0.1

A reference immediate-mode app for **SE5**. Everything here is rebuilt every frame.

## What is in it

- Custom *title bar* with OS drag and resize
- Virtualised lists (50 000 rows)
- Live settings: ~~restart to apply~~ nothing needs a restart
  1. Graphics API and GPU
  2. Window blur and transparency
  3. Fonts and text size

> Select this text with the mouse and press Ctrl+C: Markdown blocks are selectable.

| Setting | Applies |
|---|---|
| vsync | next frame |
| frame limiter | next frame |
| fonts | next frame |

See [the engine repository](https://github.com/sturdyfool10/sturdyengine5) or `run_demo --help`.

```rust
fn main() {
    println!(\"hello\");
}
```
";

#[derive(Clone, Copy)]
struct Theme {
    background: Vec4,
    panel: Vec4,
    raised: Vec4,
    text: Vec4,
    muted: Vec4,
    accent: Vec4,
    border: Vec4,
    danger: Vec4,
}

impl Theme {
    fn dark() -> Self {
        Self {
            background: Vec4::new(0.085, 0.09, 0.11, 0.94),
            panel: Vec4::new(0.11, 0.12, 0.145, 1.0),
            raised: Vec4::new(0.16, 0.17, 0.21, 1.0),
            text: Vec4::new(0.93, 0.94, 0.96, 1.0),
            muted: Vec4::new(0.62, 0.64, 0.7, 1.0),
            accent: Vec4::new(0.36, 0.58, 0.95, 1.0),
            border: Vec4::new(1.0, 1.0, 1.0, 0.1),
            danger: Vec4::new(0.9, 0.3, 0.3, 1.0),
        }
    }

    fn light() -> Self {
        Self {
            background: Vec4::new(0.95, 0.955, 0.97, 0.96),
            panel: Vec4::new(0.9, 0.91, 0.935, 1.0),
            raised: Vec4::new(1.0, 1.0, 1.0, 1.0),
            text: Vec4::new(0.1, 0.11, 0.14, 1.0),
            muted: Vec4::new(0.4, 0.42, 0.48, 1.0),
            accent: Vec4::new(0.16, 0.4, 0.85, 1.0),
            border: Vec4::new(0.0, 0.0, 0.0, 0.12),
            danger: Vec4::new(0.8, 0.15, 0.15, 1.0),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WindowLook {
    Opaque,
    Transparent,
    Blurred,
}

struct App {
    window_id: Arc<AtomicU64>,
    keep_alive: AnimationKeepAlive,
    started: Instant,
    tab: usize,
    dark: bool,
    font_size: u16,
    status: String,

    // Home
    changelog: MarkdownView,
    dropped: Vec<String>,
    tooltip: TooltipState,
    menu: PopupState,
    context_target: String,
    install_progress: f32,
    install_running: bool,

    // Library
    selected_row: Option<usize>,
    row_count: usize,

    // Settings
    backends: Vec<Backend>,
    adapters: Vec<Adapter>,
    backend_pick: usize,
    adapter_pick: usize,
    backend_dropdown: DropdownState,
    adapter_dropdown: DropdownState,
    apply_device: ButtonState,
    vsync: ToggleState,
    vsync_on: bool,
    limiter: ToggleState,
    limiter_on: bool,
    limiter_slider: SliderState,
    limiter_fps: f64,
    hdr: ToggleState,
    hdr_on: bool,
    look: WindowLook,
    look_dropdown: DropdownState,
    font_names: Vec<String>,
    font_pick: usize,
    font_dropdown: DropdownState,
    size_slider: SliderState,
    theme_switch: ToggleState,
    search: TextEditState,
    install_button: ButtonState,

    // About
    show_modal: bool,
    modal_button: ButtonState,
    about_button: ButtonState,
}

impl App {
    fn new(window_id: Arc<AtomicU64>) -> Self {
        Self {
            window_id,
            keep_alive: AnimationKeepAlive::default(),
            started: Instant::now(),
            tab: 0,
            dark: true,
            font_size: 15,
            status: String::new(),
            changelog: MarkdownView::new(CHANGELOG),
            dropped: Vec::new(),
            tooltip: TooltipState::default(),
            menu: PopupState::default(),
            context_target: "none yet".into(),
            install_progress: 0.0,
            install_running: false,
            selected_row: None,
            row_count: 50_000,
            backends: Vec::new(),
            adapters: Vec::new(),
            backend_pick: 0,
            adapter_pick: 0,
            backend_dropdown: DropdownState::new(),
            adapter_dropdown: DropdownState::new(),
            apply_device: ButtonState::new(),
            vsync: ToggleState::new(),
            vsync_on: true,
            limiter: ToggleState::new(),
            limiter_on: false,
            limiter_slider: SliderState::new(),
            limiter_fps: 60.0,
            hdr: ToggleState::new(),
            hdr_on: false,
            look: WindowLook::Blurred,
            look_dropdown: DropdownState::new(),
            font_names: Vec::new(),
            font_pick: 0,
            font_dropdown: DropdownState::new(),
            size_slider: SliderState::new(),
            theme_switch: ToggleState::new(),
            search: TextEditState::with_text(""),
            install_button: ButtonState::new(),
            show_modal: false,
            modal_button: ButtonState::new(),
            about_button: ButtonState::new(),
        }
    }

    fn surface(&self) -> SurfaceHandle {
        SurfaceHandle::from_window_id(self.window_id.load(Ordering::Relaxed))
    }

    fn theme(&self) -> Theme {
        if self.dark { Theme::dark() } else { Theme::light() }
    }

    fn text_style(&self, theme: &Theme, scale: f32) -> TextStyle {
        TextStyle { color: theme.text, font_size: (f32::from(self.font_size) * scale).round() as u16, ..TextStyle::default() }
    }

    fn muted_style(&self, theme: &Theme) -> TextStyle {
        TextStyle { color: theme.muted, font_size: self.font_size.saturating_sub(2).max(10), ..TextStyle::default() }
    }

    // -- Frame ---------------------------------------------------------------------------------------------------

    fn draw(&mut self, ui: &mut Ui<'_>) {
        let theme = self.theme();
        let now = self.started.elapsed().as_secs_f64();
        self.keep_alive.update(ui, Duration::from_millis(600));

        for event in ui.engine().take_drop_events() {
            if let DropEvent::File { path, .. } = event {
                self.dropped.push(path.display().to_string());
                self.dropped.truncate(8);
                self.keep_alive.keep_awake_for(Duration::from_secs(1));
            }
        }
        self.shortcuts(ui);
        if self.install_running {
            self.install_progress += 0.004;
            if self.install_progress >= 1.0 {
                self.install_progress = 0.0;
                self.install_running = false;
                self.status = "Install finished".into();
                let _ = ui.engine().primary_window_control().map(|mut w| w.set_taskbar_progress(TaskbarProgress::None, 0.0));
            } else {
                let progress = self.install_progress;
                let _ = ui.engine().primary_window_control().map(|mut w| w.set_taskbar_progress(TaskbarProgress::Normal, progress));
                self.keep_alive.keep_awake_for(Duration::from_millis(100));
            }
        }

        let maximized = ui.engine().primary_window_control().is_some_and(|mut w| w.is_maximized());
        let _root = ui.element(
            &ElementDesc::new()
                .id("root".into())
                .width(Sizing::Grow)
                .height(Sizing::Grow)
                .direction(Direction::TopToBottom)
                .background(theme.background)
                .corner_radius(CornerRadius::all(if maximized { 0.0 } else { 10.0 }))
                .border(Border::all(theme.border, 1)),
        );
        self.title_bar(ui, &theme, maximized);
        {
            let _body = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Grow).direction(Direction::LeftToRight));
            self.sidebar(ui, &theme);
            self.content(ui, &theme, now);
        }
        self.modal(ui, &theme);
        self.update_hit_regions(ui);
    }

    fn shortcuts(&mut self, ui: &mut Ui<'_>) {
        let engine = ui.engine();
        for (index, key) in [Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4].into_iter().enumerate() {
            if Shortcut::ctrl(key).triggered(&engine) {
                self.tab = index;
            }
        }
        if Shortcut::ctrl(Key::Q).triggered(&engine) {
            drop(engine);
            let _ = ui.engine().primary_window_control().map(|mut w| w.request_close());
        }
    }

    // -- Title bar -----------------------------------------------------------------------------------------------

    fn title_bar(&mut self, ui: &mut Ui<'_>, theme: &Theme, maximized: bool) {
        let _bar = ui.element(
            &ElementDesc::new()
                .id("titlebar".into())
                .width(Sizing::Grow)
                .height(Sizing::Fixed(38.0))
                .padding(Padding { left: 14, ..Padding::default() })
                .align(ChildAlignment::new(AlignX::Left, AlignY::Center))
                .direction(Direction::LeftToRight)
                .background(theme.panel)
                .corner_radius(CornerRadius { top_left: 10.0, top_right: 10.0, bottom_left: 0.0, bottom_right: 0.0 }),
        );
        ui.text("Shell Demo", &self.text_style(theme, 1.0));
        let _fill = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Grow));
        drop(_fill);
        let search = ui.text_field(
            &ElementDesc::new().id("title-search".into()).width(Sizing::Fixed(220.0)).height(Sizing::Fixed(26.0)),
            &TextInputStyle { font_size: self.font_size.saturating_sub(2), ..TextInputStyle::default() },
            &mut self.search,
            "Search…",
            true,
        );
        let _ = search;
        let spacer = ui.element(&ElementDesc::new().width(Sizing::Fixed(12.0)).height(Sizing::Grow));
        drop(spacer);
        for (id, label, danger) in [("tb-min", "-", false), ("tb-max", if maximized { "[-]" } else { "[ ]" }, false), ("tb-close", "x", true)] {
            let hovered = ui.hovered(id);
            let background = match (hovered, danger) {
                (true, true) => theme.danger,
                (true, false) => theme.raised,
                _ => Vec4::ZERO,
            };
            {
                let _button = ui.element(
                    &ElementDesc::new()
                        .id(id.into())
                        .width(Sizing::Fixed(46.0))
                        .height(Sizing::Grow)
                        .align(ChildAlignment::center())
                        .background(background)
                        .cursor(Cursor::Pointer),
                );
                ui.text(label, &self.text_style(theme, 1.0));
            }
            if ui.clicked(id) {
                let mut engine = ui.engine();
                if let Some(mut window) = engine.primary_window_control() {
                    match id {
                        "tb-min" => {
                            let _ = window.minimize();
                        }
                        "tb-max" => {
                            let _ = window.toggle_maximized();
                        }
                        _ => window.request_close(),
                    }
                }
            }
        }
    }

    /// The title bar drags the window; its own controls stay clickable. Rebuilt every frame from last frame's layout.
    fn update_hit_regions(&mut self, ui: &mut Ui<'_>) {
        let rect = |bounds: (Vec2, Vec2)| WindowRect {
            x: bounds.0.x as i32,
            y: bounds.0.y as i32,
            width: bounds.1.x as i32,
            height: bounds.1.y as i32,
        };
        let Some(bar) = ui.element_bounds("titlebar") else { return };
        let regions = HitTestRegions {
            enabled: true,
            resize_border: 6,
            drag: vec![rect(bar)],
            no_drag: ["tb-min", "tb-max", "tb-close", "title-search"].iter().filter_map(|id| ui.element_bounds(id)).map(rect).collect(),
        };
        let _ = ui.engine().primary_window_control().map(|mut w| w.set_hit_test_regions(&regions));
    }

    // -- Sidebar and tabs ------------------------------------------------------------------------------------------

    fn sidebar(&mut self, ui: &mut Ui<'_>, theme: &Theme) {
        let _sidebar = ui.element(
            &ElementDesc::new()
                .width(Sizing::Fixed(180.0))
                .height(Sizing::Grow)
                .padding(Padding::all(10))
                .direction(Direction::TopToBottom)
                .background(theme.panel),
        );
        let style = TabStyle {
            selected: theme.accent.with_w(0.28),
            hovered: theme.raised,
            text: self.text_style(theme, 1.0),
            selected_text: theme.text,
            ..TabStyle::default()
        };
        self.tab = ui_kit::tabs(ui, "sidebar-tabs", &["Home", "Library", "Settings", "About"], self.tab, true, &style);
    }

    fn content(&mut self, ui: &mut Ui<'_>, theme: &Theme, now: f64) {
        let _content = ui.element(
            &ElementDesc::new()
                .id("content".into())
                .width(Sizing::Grow)
                .height(Sizing::Grow)
                .padding(Padding::all(20))
                .child_gap(14)
                .direction(Direction::TopToBottom),
        );
        match self.tab {
            0 => self.home(ui, theme, now),
            1 => self.library(ui, theme),
            2 => self.settings(ui, theme),
            _ => self.about(ui, theme),
        }
        if !self.status.is_empty() {
            ui.text(&self.status.clone(), &self.muted_style(theme));
        }
    }

    // -- Home ------------------------------------------------------------------------------------------------------

    fn home(&mut self, ui: &mut Ui<'_>, theme: &Theme, now: f64) {
        ui.text("Home", &self.text_style(theme, 1.5));
        let bar = ProgressStyle { fill: theme.accent, track: theme.raised, ..ProgressStyle::default() };

        ui.text("Install progress", &self.muted_style(theme));
        let track = ElementDesc::new().id("progress-determinate".into()).width(Sizing::Grow).height(Sizing::Fixed(8.0));
        let _ = ui_kit::progress_bar(ui, &track, Some(self.install_progress), &bar, now);
        ui.text("Working…", &self.muted_style(theme));
        let track = ElementDesc::new().id("progress-indeterminate".into()).width(Sizing::Grow).height(Sizing::Fixed(8.0));
        if let Some(next) = ui_kit::progress_bar(ui, &track, None, &bar, now) {
            self.keep_alive.keep_awake_for(next * 4);
        }

        {
            let _row = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(10).direction(Direction::LeftToRight));
            let label = if self.install_running { "Installing…" } else { "Start install" };
            let (result, scope) = ui.button(
                &ElementDesc::new().id("install".into()).width(Sizing::Fixed(130.0)).height(Sizing::Fixed(32.0)).align(ChildAlignment::center()),
                &mut self.install_button,
                &ButtonStyle { idle: theme.accent, corner_radius: 6.0, ..ButtonStyle::default() },
                !self.install_running,
            );
            if let Some(scope) = scope {
                ui.text(label, &self.text_style(theme, 1.0));
                drop(scope);
            }
            if result.clicked {
                self.install_running = true;
                self.install_progress = 0.0;
            }
            if let Some(delay) = ui_kit::text_tooltip(
                ui,
                "install",
                "Drives the taskbar progress too",
                &mut self.tooltip,
                &TooltipStyle::default(),
                0.016,
            ) {
                self.keep_alive.keep_awake_for(delay);
            }

            let _target = ui.element(
                &ElementDesc::new()
                    .id("ctx-target".into())
                    .width(Sizing::Fixed(190.0))
                    .height(Sizing::Fixed(32.0))
                    .align(ChildAlignment::center())
                    .background(theme.raised)
                    .corner_radius(CornerRadius::all(6.0))
                    .cursor(Cursor::Pointer),
            );
            ui.text("Right-click me", &self.text_style(theme, 1.0));
        }
        if ui_kit::pressed(ui, "ctx-target", MouseButton::Right) {
            let at = ui.pointer_position();
            self.menu.open_at(at);
        }
        let items = [MenuItem::new("Open"), MenuItem::new("Duplicate"), MenuItem::new("Delete").separated(), MenuItem::new("Disabled").enabled(false)];
        let popup = PopupStyle { background: theme.raised, border: theme.border, ..PopupStyle::default() };
        let menu_text = self.text_style(theme, 1.0);
        if let Some(choice) = ui_kit::context_menu(ui, "ctx-menu", &mut self.menu, &items, &popup, &menu_text) {
            self.context_target = items[choice].label.clone();
        }
        ui.text(&format!("Last context action: {}", self.context_target), &self.muted_style(theme));

        let _zone = ui.element(
            &ElementDesc::new()
                .id("dropzone".into())
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .padding(Padding::all(12))
                .child_gap(4)
                .direction(Direction::TopToBottom)
                .border(Border::all(theme.accent.with_w(0.6), 1))
                .corner_radius(CornerRadius::all(8.0)),
        );
        ui.text("Drop .mrpack / .zip files here", &self.text_style(theme, 1.0));
        for path in self.dropped.iter().take(3) {
            let shown = ui.ellipsize(&self.muted_style(theme), path, 640.0);
            ui.text(&shown, &self.muted_style(theme));
        }
        drop(_zone);

        let _scroll = ui.element(
            &ElementDesc::new().id("home-scroll".into()).width(Sizing::Grow).height(Sizing::Grow).clip(Clip::vertical()).direction(Direction::TopToBottom),
        );
        let style = MarkdownStyle { text: theme.text, muted: theme.muted, link: theme.accent, font_size: self.font_size, ..MarkdownStyle::default() };
        for event in self.changelog.show(ui, "changelog", &style) {
            let MarkdownEvent::LinkClicked(url) = event;
            self.status = format!("Link clicked: {url}");
        }
    }

    // -- Library ---------------------------------------------------------------------------------------------------

    fn library(&mut self, ui: &mut Ui<'_>, theme: &Theme) {
        ui.text(&format!("Library ({} instances, {} built)", self.row_count, "only the visible ones"), &self.text_style(theme, 1.5));
        let _list = ui.element(
            &ElementDesc::new()
                .id("library-list".into())
                .width(Sizing::Grow)
                .height(Sizing::Grow)
                .clip(Clip::vertical())
                .direction(Direction::TopToBottom)
                .background(theme.panel)
                .corner_radius(CornerRadius::all(8.0)),
        );
        const ROW: f32 = 30.0;
        let visible = ui_kit::visible_rows(ui, "library-list", ROW, self.row_count, 3);
        ui_kit::spacer(ui, visible.before);
        for index in visible.rows.clone() {
            let id = format!("row-{index}");
            if ui.clicked(&id) {
                self.selected_row = Some(index);
            }
            let selected = self.selected_row == Some(index);
            let background = if selected {
                theme.accent.with_w(0.3)
            } else if ui.hovered(&id) {
                theme.raised
            } else {
                Vec4::ZERO
            };
            let _row = ui.element(
                &ElementDesc::new()
                    .id(id)
                    .width(Sizing::Grow)
                    .height(Sizing::Fixed(ROW))
                    .padding(Padding::symmetric(12, 4))
                    .align(ChildAlignment::new(AlignX::Left, AlignY::Center))
                    .background(background)
                    .cursor(Cursor::Pointer),
            );
            let name = format!("Instance {index:05} with a deliberately long name to show ellipsis truncation in a row");
            let shown = ui.ellipsize(&self.text_style(theme, 1.0), &name, 520.0);
            ui.text(&shown, &self.text_style(theme, 1.0));
        }
        ui_kit::spacer(ui, visible.after);
    }

    // -- Settings --------------------------------------------------------------------------------------------------

    fn settings(&mut self, ui: &mut Ui<'_>, theme: &Theme) {
        if self.adapters.is_empty() {
            let mut engine = ui.engine();
            self.backends = engine.available_backends();
            self.adapters = engine.available_adapters();
            self.font_names = std::iter::once("Embedded default".to_owned()).chain(ui::system_font_families().into_iter().take(40)).collect();
            if let Some(active) = self.adapters.iter().position(|a| a.backends.iter().any(|b| b.active)) {
                self.adapter_pick = active;
            }
            if let Some(active) = self.backends.iter().position(|b| self.adapters.iter().any(|a| a.backends.iter().any(|o| o.active && o.backend == *b))) {
                self.backend_pick = active;
            }
        }
        ui.text("Settings", &self.text_style(theme, 1.5));
        ui.text("Every control applies immediately.", &self.muted_style(theme));
        let surface = self.surface();
        let label = |ui: &mut Ui<'_>, text: &str, style: &TextStyle| ui.text(text, style);
        let muted = self.muted_style(theme);

        // Graphics device.
        label(ui, "Graphics API", &muted);
        let backend_names: Vec<String> = self.backends.iter().map(|b| format!("{b:?}")).collect();
        let backend_refs: Vec<&str> = backend_names.iter().map(String::as_str).collect();
        let dropdown_style = DropdownStyle::default();
        let backend = ui.dropdown(
            "backend-dropdown",
            &ElementDesc::new().id("backend-trigger".into()).width(Sizing::Fixed(240.0)).height(Sizing::Fixed(30.0)),
            &dropdown_style,
            &mut self.backend_dropdown,
            self.backend_pick,
            &backend_refs,
            0,
            self.font_size,
        );
        if backend.changed {
            self.backend_pick = backend.selected_index;
        }
        label(ui, "GPU", &muted);
        let adapter_names: Vec<String> = self.adapters.iter().map(|a| a.name.clone()).collect();
        let adapter_refs: Vec<&str> = adapter_names.iter().map(String::as_str).collect();
        let adapter = ui.dropdown(
            "adapter-dropdown",
            &ElementDesc::new().id("adapter-trigger".into()).width(Sizing::Fixed(360.0)).height(Sizing::Fixed(30.0)),
            &dropdown_style,
            &mut self.adapter_dropdown,
            self.adapter_pick,
            &adapter_refs,
            0,
            self.font_size,
        );
        if adapter.changed {
            self.adapter_pick = adapter.selected_index;
        }
        let (apply, scope) = ui.button(
            &ElementDesc::new().id("apply-device".into()).width(Sizing::Fixed(180.0)).height(Sizing::Fixed(32.0)).align(ChildAlignment::center()),
            &mut self.apply_device,
            &ButtonStyle { idle: theme.accent, corner_radius: 6.0, ..ButtonStyle::default() },
            !self.adapters.is_empty() && !self.backends.is_empty(),
        );
        if let Some(scope) = scope {
            ui.text("Apply graphics device", &self.text_style(theme, 1.0));
            drop(scope);
        }
        if apply.clicked {
            let request = DeviceRequest::new()
                .backend(self.backends[self.backend_pick])
                .adapter(self.adapters[self.adapter_pick].stable_id.clone())
                .preference(DevicePreference::HighPerformance);
            let result = ui.engine().switch_device(&request);
            self.status = match result.outcome {
                SwitchOutcome::Switched => format!("Switched to {} ({:?})", result.adapter_name, result.backend),
                SwitchOutcome::SwitchedWithFallback(reason) => format!("Switched with fallback: {reason}"),
                SwitchOutcome::Failed(reason) => format!("Switch failed, still on {}: {reason}", result.adapter_name),
            };
            // Anything created through the raw RHI would be rebuilt here when `device_generation()` changes.
            self.keep_alive.keep_awake_for(Duration::from_secs(1));
        }

        // Presentation.
        let mut presentation = ui.engine().presentation_settings(surface);
        let mut changed = false;
        let toggle = ToggleStyle { checked: theme.accent, ..ToggleStyle::default() };
        {
            let _row = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(10).direction(Direction::LeftToRight).align(ChildAlignment::new(AlignX::Left, AlignY::Center)));
            let r = ui.switch(&ElementDesc::new().id("vsync".into()).width(Sizing::Fixed(46.0)).height(Sizing::Fixed(24.0)), &mut self.vsync, &toggle, self.vsync_on, true);
            if r.clicked {
                self.vsync_on = !self.vsync_on;
                presentation = presentation.vsync(if self.vsync_on { VSync::On } else { VSync::Off });
                changed = true;
            }
            ui.text("VSync", &self.text_style(theme, 1.0));
        }
        {
            let _row = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(10).direction(Direction::LeftToRight).align(ChildAlignment::new(AlignX::Left, AlignY::Center)));
            let r = ui.switch(&ElementDesc::new().id("limiter".into()).width(Sizing::Fixed(46.0)).height(Sizing::Fixed(24.0)), &mut self.limiter, &toggle, self.limiter_on, true);
            if r.clicked {
                self.limiter_on = !self.limiter_on;
                changed = true;
            }
            ui.text(&format!("Frame limiter: {:.0} fps", self.limiter_fps), &self.text_style(theme, 1.0));
        }
        let slider = ui.slider(
            &ElementDesc::new().id("limit-slider".into()).width(Sizing::Fixed(280.0)).height(Sizing::Fixed(22.0)),
            &SliderConfig { min: 30.0, max: 240.0, step: Some(5.0), ..SliderConfig::default() },
            &SliderStyle { fill: theme.accent, ..SliderStyle::default() },
            &mut self.limiter_slider,
            self.limiter_fps,
            &SliderInput::default(),
            self.limiter_on,
        );
        if slider.changed {
            self.limiter_fps = slider.value;
            changed = true;
        }
        if changed {
            presentation.frame_rate_limit = if self.limiter_on { FrameRateLimit::Fps(self.limiter_fps) } else { FrameRateLimit::Unlimited };
        }
        {
            let _row = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(10).direction(Direction::LeftToRight).align(ChildAlignment::new(AlignX::Left, AlignY::Center)));
            let r = ui.switch(&ElementDesc::new().id("hdr".into()).width(Sizing::Fixed(46.0)).height(Sizing::Fixed(24.0)), &mut self.hdr, &toggle, self.hdr_on, true);
            if r.clicked {
                self.hdr_on = !self.hdr_on;
                presentation = presentation.hdr_enabled(self.hdr_on);
                changed = true;
            }
            ui.text("HDR output", &self.text_style(theme, 1.0));
        }
        if changed {
            if let Err(error) = ui.engine().set_presentation_settings(surface, presentation) {
                self.status = format!("Presentation change refused: {error}");
            }
        }

        // Window look: the three states of a window-transparency setting, all live.
        label(ui, "Window", &muted);
        let looks = ["Opaque", "Transparent", "Blurred"];
        let current = match self.look {
            WindowLook::Opaque => 0,
            WindowLook::Transparent => 1,
            WindowLook::Blurred => 2,
        };
        let look = ui.dropdown(
            "look-dropdown",
            &ElementDesc::new().id("look-trigger".into()).width(Sizing::Fixed(240.0)).height(Sizing::Fixed(30.0)),
            &dropdown_style,
            &mut self.look_dropdown,
            current,
            &looks,
            0,
            self.font_size,
        );
        if look.changed {
            self.look = [WindowLook::Opaque, WindowLook::Transparent, WindowLook::Blurred][look.selected_index];
            self.apply_window_look(ui, surface);
        }
        let blur_status = ui.engine().window_effect_status(surface, WindowEffectKind::Blur);
        let blur_text = match blur_status {
            Some(status) => format!("Blur: {:?} {}", status.outcome, status.details),
            None => "Blur: not requested".to_owned(),
        };
        label(ui, &blur_text, &muted);

        // Text.
        label(ui, "Font", &muted);
        let font_refs: Vec<&str> = self.font_names.iter().map(String::as_str).collect();
        let font = ui.dropdown(
            "font-dropdown",
            &ElementDesc::new().id("font-trigger".into()).width(Sizing::Fixed(300.0)).height(Sizing::Fixed(30.0)),
            &dropdown_style,
            &mut self.font_dropdown,
            self.font_pick,
            &font_refs,
            0,
            self.font_size,
        );
        if font.changed {
            self.font_pick = font.selected_index;
            let name = self.font_names[self.font_pick].clone();
            let result = if self.font_pick == 0 {
                ui.engine().unregister_ui_font(0);
                Ok(())
            } else {
                // A live font change: the same id, a different stack; text re-shapes on the next frame.
                ui.engine().register_ui_font_stack(0, &FontStack::new(FontSource::system(name.clone())))
            };
            self.status = match result {
                Ok(()) => format!("Font: {name}"),
                Err(error) => format!("Font change failed: {error}"),
            };
        }
        let size = ui.slider(
            &ElementDesc::new().id("size-slider".into()).width(Sizing::Fixed(280.0)).height(Sizing::Fixed(22.0)),
            &SliderConfig { min: 11.0, max: 24.0, step: Some(1.0), ..SliderConfig::default() },
            &SliderStyle { fill: theme.accent, ..SliderStyle::default() },
            &mut self.size_slider,
            f64::from(self.font_size),
            &SliderInput::default(),
            true,
        );
        if size.changed {
            self.font_size = size.value as u16;
        }
        label(ui, &format!("Text size: {}", self.font_size), &muted);
        {
            let _row = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(10).direction(Direction::LeftToRight).align(ChildAlignment::new(AlignX::Left, AlignY::Center)));
            let r = ui.switch(&ElementDesc::new().id("theme".into()).width(Sizing::Fixed(46.0)).height(Sizing::Fixed(24.0)), &mut self.theme_switch, &toggle, !self.dark, true);
            if r.clicked {
                self.dark = !self.dark;
            }
            ui.text("Light theme", &self.text_style(theme, 1.0));
        }
    }

    /// Opaque / transparent / blurred, applied to the running window with no recreation.
    fn apply_window_look(&mut self, ui: &mut Ui<'_>, surface: SurfaceHandle) {
        let see_through = self.look != WindowLook::Opaque;
        let mut engine = ui.engine();
        engine.set_transparent(surface, see_through);
        let presentation = engine.presentation_settings(surface).transparent_composition(see_through);
        if let Err(error) = engine.set_presentation_settings(surface, presentation) {
            self.status = format!("Transparency refused: {error}");
        }
        let params = WindowEffectParams {
            protocol: LinuxBlurProtocol::Automatic,
            options: WindowEffectOptions { corner_radius: 10.0, ..WindowEffectOptions::default() },
            ..WindowEffectParams::default()
        };
        engine.set_window_effect_with(surface, WindowEffectKind::Blur, self.look == WindowLook::Blurred, &params);
    }

    // -- About and the modal ---------------------------------------------------------------------------------------

    fn about(&mut self, ui: &mut Ui<'_>, theme: &Theme) {
        ui.text("About", &self.text_style(theme, 1.5));
        ui.text("Shell Demo is the SE5 reference for porting Vertex Launcher.", &self.text_style(theme, 1.0));
        let (result, scope) = ui.button(
            &ElementDesc::new().id("about-open".into()).width(Sizing::Fixed(160.0)).height(Sizing::Fixed(32.0)).align(ChildAlignment::center()),
            &mut self.about_button,
            &ButtonStyle { idle: theme.accent, corner_radius: 6.0, ..ButtonStyle::default() },
            true,
        );
        if let Some(scope) = scope {
            ui.text("Show dialog", &self.text_style(theme, 1.0));
            drop(scope);
        }
        if result.clicked {
            self.show_modal = true;
        }
    }

    fn modal(&mut self, ui: &mut Ui<'_>, theme: &Theme) {
        if !self.show_modal {
            return;
        }
        let style = ModalStyle { background: theme.panel, ..ModalStyle::default() };
        let title = self.text_style(theme, 1.3);
        let body = self.text_style(theme, 1.0);
        let accent = theme.accent;
        let button_state = &mut self.modal_button;
        let mut ok_clicked = false;
        let response = ui_kit::modal(ui, "about-modal", &style, true, |ui| {
            ui.text("A modal dialog", &title);
            ui.text("The scrim blocks everything behind it. Esc or a click outside dismisses it.", &body);
            let (ok, scope) = ui.button(
                &ElementDesc::new().id("modal-ok".into()).width(Sizing::Fixed(120.0)).height(Sizing::Fixed(32.0)).align(ChildAlignment::center()),
                button_state,
                &ButtonStyle { idle: accent, corner_radius: 6.0, ..ButtonStyle::default() },
                true,
            );
            if let Some(scope) = scope {
                ui.text("OK", &body);
                drop(scope);
            }
            ok_clicked = ok.clicked;
        });
        if response.dismissed || ok_clicked {
            self.show_modal = false;
        }
    }
}

struct Shell {
    window_id: Arc<AtomicU64>,
}

impl GameLogic for Shell {
    fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
        engine.set_frame_mode(FrameMode::OnDemand);
        let mut app = App::new(self.window_id.clone());
        ui::install_draw_hook(move |ui| app.draw(ui));
        Ok(())
    }

    fn frame(&mut self, _engine: &mut Engine<'_>, frame: &Frame) -> Option<FrameDescription> {
        self.window_id.store(frame.window_id, Ordering::Relaxed);
        Some(FrameDescription::ui_only())
    }
}

fn main() -> std::process::ExitCode {
    let config = RuntimeConfig::new("Shell Demo")
        .size(1100, 720)
        .decorated(false)
        .transparent(true)
        .transparent_composition(true)
        .app_id("org.sturdyfool10.ShellDemo");
    sturdy::run(config, Shell { window_id: Arc::new(AtomicU64::new(0)) })
}
