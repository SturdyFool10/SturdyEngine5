//! Composite widgets for application UIs, built only from the engine's element primitives (see [`crate::ui`]) and
//! stateful widgets ([`crate::widgets`]): progress bars, tooltips, popups and context menus that stay inside the
//! window, modal dialogs, tabs, and virtualised lists for very long content.
//!
//! Everything here is immediate mode and keeps its per-widget state in small structs you own, like the engine's own
//! widgets. Positions that depend on an element's size (a popup's, a tooltip's) are taken from the previous frame's
//! layout, the standard one-frame-behind trick of immediate-mode UIs: the first frame a popup opens it may sit
//! unclamped for a single frame, after which it is kept inside the window.
//!
//! In an on-demand app (see [`crate::app`]) animated pieces (an indeterminate progress bar, a tooltip delay) ask for
//! their next frame through the `redraw_after` they return; pass it to [`crate::Engine::request_redraw_after`].

use std::time::Duration;

use glam::{Vec2, Vec4};

use crate::ui::{
    AttachPoint, ChildAlignment, CornerRadius, Direction, ElementDesc, ElementScope, FloatTarget, Floating, Padding,
    Sizing, TextStyle, Ui,
};
use crate::Key;

// -- Geometry helpers -------------------------------------------------------------------------------------------

/// Where to put a box of `size` next to `anchor` (position, size) so that it stays inside `viewport`: below the anchor
/// if it fits, else above, and shifted horizontally to stay on screen. `gap` separates it from the anchor. Pure; the
/// popup/tooltip helpers below use it with last frame's bounds.
pub fn place_near(anchor: (Vec2, Vec2), size: Vec2, viewport: Vec2, gap: f32) -> Vec2 {
    let (anchor_pos, anchor_size) = anchor;
    let below = anchor_pos.y + anchor_size.y + gap;
    let above = anchor_pos.y - gap - size.y;
    let y = if below + size.y <= viewport.y || above < 0.0 { below } else { above };
    let x = anchor_pos.x.min(viewport.x - size.x).max(0.0);
    let y = y.min(viewport.y - size.y).max(0.0);
    Vec2::new(x, y)
}

/// Keeps a box of `size` at `position` inside `viewport` (a context menu opened near the window edge).
pub fn clamp_to_viewport(position: Vec2, size: Vec2, viewport: Vec2) -> Vec2 {
    Vec2::new(position.x.min(viewport.x - size.x).max(0.0), position.y.min(viewport.y - size.y).max(0.0))
}

fn root_floating(position: Vec2, z_index: i16) -> Floating {
    let mut floating = Floating::new(FloatTarget::Root);
    floating.element_attach_point = AttachPoint::LeftTop;
    floating.parent_attach_point = AttachPoint::LeftTop;
    floating.offset = position;
    floating.z_index = z_index;
    floating
}

// -- Progress bar -----------------------------------------------------------------------------------------------

/// Colours of a [`progress_bar`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ProgressStyle {
    pub track: Vec4,
    pub fill: Vec4,
    pub corner_radius: f32,
    /// Seconds for the indeterminate stripe to cross the track once.
    pub indeterminate_period: f32,
}

impl Default for ProgressStyle {
    fn default() -> Self {
        Self {
            track: Vec4::new(0.2, 0.21, 0.26, 1.0),
            fill: Vec4::new(0.35, 0.55, 0.85, 1.0),
            corner_radius: 3.0,
            indeterminate_period: 1.2,
        }
    }
}

/// A progress bar. `progress` is `Some(0..=1)` for a determinate bar, `None` for an indeterminate one (a stripe that
/// slides across, driven by `time_seconds`, any monotonically increasing clock). `desc` sizes the track (give it a
/// fixed height). Returns the delay before the next frame is needed (indeterminate bars animate), else `None`.
pub fn progress_bar(ui: &mut Ui<'_>, desc: &ElementDesc, progress: Option<f32>, style: &ProgressStyle, time_seconds: f64) -> Option<Duration> {
    let track = desc
        .clone()
        .background(style.track)
        .corner_radius(CornerRadius::all(style.corner_radius))
        .direction(Direction::LeftToRight);
    let _track = ui.element(&track);
    match progress {
        Some(fraction) => {
            let fraction = fraction.clamp(0.0, 1.0);
            let _fill = ui.element(
                &ElementDesc::new()
                    .width(Sizing::Percent(fraction))
                    .height(Sizing::Grow)
                    .background(style.fill)
                    .corner_radius(CornerRadius::all(style.corner_radius)),
            );
            None
        }
        None => {
            // A stripe a third of the track wide sliding from off-left to off-right, eased so it lingers less at
            // the ends. Built from a leading spacer plus the stripe; percent sizes keep it resolution independent.
            let period = f64::from(style.indeterminate_period.max(0.1));
            let phase = ((time_seconds % period) / period) as f32;
            let eased = phase * phase * (3.0 - 2.0 * phase);
            let stripe = 0.33f32;
            let lead = (eased * (1.0 + stripe) - stripe).clamp(0.0, 1.0);
            let visible = (eased * (1.0 + stripe)).min(1.0) - lead;
            if lead > 0.0 {
                let _spacer = ui.element(&ElementDesc::new().width(Sizing::Percent(lead)).height(Sizing::Grow));
            }
            if visible > 0.0 {
                let _fill = ui.element(
                    &ElementDesc::new()
                        .width(Sizing::Percent(visible))
                        .height(Sizing::Grow)
                        .background(style.fill)
                        .corner_radius(CornerRadius::all(style.corner_radius)),
                );
            }
            Some(Duration::from_millis(16))
        }
    }
}

// -- Tooltip ----------------------------------------------------------------------------------------------------

/// Hover timing for one [`tooltip`].
#[derive(Debug, Clone, Default)]
pub struct TooltipState {
    hover_seconds: f32,
}

/// Look of a [`tooltip`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TooltipStyle {
    pub background: Vec4,
    pub text: TextStyle,
    pub padding: Padding,
    pub corner_radius: f32,
    /// Seconds the pointer must rest on the anchor first.
    pub delay_seconds: f32,
    pub max_width: f32,
}

impl Default for TooltipStyle {
    fn default() -> Self {
        Self {
            background: Vec4::new(0.08, 0.09, 0.11, 0.96),
            text: TextStyle { font_size: 13, ..TextStyle::default() },
            padding: Padding::symmetric(8, 5),
            corner_radius: 4.0,
            delay_seconds: 0.5,
            max_width: 320.0,
        }
    }
}

/// Shows a tooltip near element `anchor_id` (which must have an id) once the pointer has rested on it for the style's
/// delay. `content` builds the tooltip body (rich content is fine). Kept inside the window. Returns when to redraw
/// while waiting out the delay.
pub fn tooltip(
    ui: &mut Ui<'_>,
    anchor_id: &str,
    state: &mut TooltipState,
    style: &TooltipStyle,
    delta_seconds: f32,
    content: impl FnOnce(&mut Ui<'_>),
) -> Option<Duration> {
    if !ui.hovered(anchor_id) {
        state.hover_seconds = 0.0;
        return None;
    }
    state.hover_seconds += delta_seconds;
    if state.hover_seconds < style.delay_seconds {
        return Some(Duration::from_secs_f32((style.delay_seconds - state.hover_seconds).max(0.016)));
    }
    let Some(anchor) = ui.element_bounds(anchor_id) else { return None };
    let tip_id = format!("{anchor_id}::tooltip");
    let size = ui.element_bounds(&tip_id).map_or(Vec2::new(style.max_width, 24.0), |(_, s)| s);
    let position = place_near(anchor, size, ui.viewport_size(), 6.0);
    let mut floating = root_floating(position, 1000);
    floating.capture_pointer = false;
    let _tip = ui.element(
        &ElementDesc::new()
            .id(tip_id)
            .width(Sizing::Fit)
            .height(Sizing::Fit)
            .padding(style.padding)
            .background(style.background)
            .corner_radius(CornerRadius::all(style.corner_radius))
            .direction(Direction::TopToBottom)
            .floating(Some(floating)),
    );
    content(ui);
    None
}

/// The common case: a plain text tooltip.
pub fn text_tooltip(ui: &mut Ui<'_>, anchor_id: &str, text: &str, state: &mut TooltipState, style: &TooltipStyle, delta_seconds: f32) -> Option<Duration> {
    let text_style = style.text;
    tooltip(ui, anchor_id, state, style, delta_seconds, |ui| ui.text(text, &text_style))
}

// -- Popups and context menus ------------------------------------------------------------------------------------

/// Open/closed state of a popup or context menu.
#[derive(Debug, Clone, Default)]
pub struct PopupState {
    open: bool,
    /// Where a context menu was opened (pointer position); `None` = anchored to an element.
    at: Option<Vec2>,
    /// Frames since opening; the click that opened it must not also close it.
    frames_open: u32,
}

impl PopupState {
    pub fn is_open(&self) -> bool {
        self.open
    }

    /// Opens anchored to its element (a dropdown-style popup).
    pub fn open(&mut self) {
        self.open = true;
        self.at = None;
        self.frames_open = 0;
    }

    /// Opens at a point (a context menu at the pointer).
    pub fn open_at(&mut self, position: Vec2) {
        self.open = true;
        self.at = Some(position);
        self.frames_open = 0;
    }

    pub fn close(&mut self) {
        self.open = false;
    }

    pub fn toggle(&mut self) {
        if self.open { self.close() } else { self.open() }
    }
}

/// Look of a popup / context menu panel.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PopupStyle {
    pub background: Vec4,
    pub border: Vec4,
    pub corner_radius: f32,
    pub padding: Padding,
    pub gap: f32,
    pub z_index: i16,
}

impl Default for PopupStyle {
    fn default() -> Self {
        Self {
            background: Vec4::new(0.11, 0.12, 0.15, 0.98),
            border: Vec4::new(0.3, 0.32, 0.38, 1.0),
            corner_radius: 6.0,
            padding: Padding::all(6),
            gap: 4.0,
            z_index: 900,
        }
    }
}

/// A popup panel: anchored below/above element `anchor_id` (or at the point from [`PopupState::open_at`]), kept inside
/// the window, closed by a click outside or Escape. `content` builds the body and may call `state.close()` (after a
/// menu choice, say). Does nothing while closed. `popup_id` names the panel element (needed for click-outside).
pub fn popup(
    ui: &mut Ui<'_>,
    popup_id: &str,
    anchor_id: &str,
    state: &mut PopupState,
    style: &PopupStyle,
    content: impl FnOnce(&mut Ui<'_>, &mut PopupState),
) {
    if !state.open {
        return;
    }
    state.frames_open = state.frames_open.saturating_add(1);
    if state.frames_open > 1 {
        let escape = ui.engine().input().key_just_pressed(Key::Escape);
        if escape || ui.clicked_outside(popup_id) {
            state.close();
            return;
        }
    }
    let viewport = ui.viewport_size();
    let size = ui.element_bounds(popup_id).map_or(Vec2::new(200.0, 40.0), |(_, s)| s);
    let position = match state.at {
        Some(point) => clamp_to_viewport(point, size, viewport),
        None => match ui.element_bounds(anchor_id) {
            Some(anchor) => place_near(anchor, size, viewport, style.gap),
            None => clamp_to_viewport(Vec2::ZERO, size, viewport),
        },
    };
    let _panel = ui.element(
        &ElementDesc::new()
            .id(popup_id.to_owned())
            .width(Sizing::Fit)
            .height(Sizing::Fit)
            .padding(style.padding)
            .child_gap(2)
            .direction(Direction::TopToBottom)
            .background(style.background)
            .border(crate::ui::Border::all(style.border, 1))
            .corner_radius(CornerRadius::all(style.corner_radius))
            .floating(Some(root_floating(position, style.z_index))),
    );
    content(ui, state);
}

/// One row of a [`context_menu`].
#[derive(Debug, Clone, PartialEq)]
pub struct MenuItem {
    pub label: String,
    pub enabled: bool,
    /// Draws a separator line above this item.
    pub separator_before: bool,
}

impl MenuItem {
    pub fn new(label: impl Into<String>) -> Self {
        Self { label: label.into(), enabled: true, separator_before: false }
    }

    #[must_use]
    pub fn enabled(mut self, enabled: bool) -> Self {
        self.enabled = enabled;
        self
    }

    #[must_use]
    pub fn separated(mut self) -> Self {
        self.separator_before = true;
        self
    }
}

/// A context menu of text rows at the point it was opened ([`PopupState::open_at`], usually on right-click). Returns
/// the chosen item's index (and closes the menu).
pub fn context_menu(ui: &mut Ui<'_>, menu_id: &str, state: &mut PopupState, items: &[MenuItem], style: &PopupStyle, text: &TextStyle) -> Option<usize> {
    let mut chosen = None;
    popup(ui, menu_id, menu_id, state, style, |ui, state| {
        for (index, item) in items.iter().enumerate() {
            if item.separator_before {
                let _line = ui.element(
                    &ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(1.0)).background(style.border),
                );
            }
            let row_id = format!("{menu_id}::{index}");
            let hovered = item.enabled && ui.hovered(&row_id);
            let mut row_text = *text;
            if !item.enabled {
                row_text.color.w *= 0.45;
            }
            {
                let _row = ui.element(
                    &ElementDesc::new()
                        .id(row_id.clone())
                        .width(Sizing::Grow)
                        .height(Sizing::Fit)
                        .padding(Padding::symmetric(10, 5))
                        .corner_radius(CornerRadius::all(4.0))
                        .background(if hovered { style.border } else { Vec4::ZERO }),
                );
                ui.text(&item.label, &row_text);
            }
            if item.enabled && ui.clicked(&row_id) {
                chosen = Some(index);
                state.close();
            }
        }
    });
    chosen
}

// -- Modal ------------------------------------------------------------------------------------------------------

/// Look of a [`modal`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ModalStyle {
    /// Dims everything behind the dialog.
    pub scrim: Vec4,
    pub background: Vec4,
    pub corner_radius: f32,
    pub padding: Padding,
    pub width: Sizing,
    pub z_index: i16,
}

impl Default for ModalStyle {
    fn default() -> Self {
        Self {
            scrim: Vec4::new(0.0, 0.0, 0.0, 0.55),
            background: Vec4::new(0.12, 0.13, 0.16, 1.0),
            corner_radius: 10.0,
            padding: Padding::all(18),
            width: Sizing::Fixed(520.0),
            z_index: 2000,
        }
    }
}

/// What happened to a modal this frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ModalResponse {
    /// Escape was pressed or the scrim was clicked (when `dismissible`); close it.
    pub dismissed: bool,
}

/// A modal dialog: a full-window scrim that swallows pointer input to everything behind it, with a centred panel
/// built by `content`. Escape or a click on the scrim reports `dismissed` when `dismissible`. Keyboard focus is the
/// caller's: focus the dialog's first field when it opens (`Ui::focus`) so typing goes there.
pub fn modal(ui: &mut Ui<'_>, modal_id: &str, style: &ModalStyle, dismissible: bool, content: impl FnOnce(&mut Ui<'_>)) -> ModalResponse {
    let mut response = ModalResponse::default();
    let panel_id = format!("{modal_id}::panel");
    let scrim_id = format!("{modal_id}::scrim");
    if dismissible {
        let escape = ui.engine().input().key_just_pressed(Key::Escape);
        let scrim_clicked = ui.clicked(&scrim_id) && !ui.hovered(&panel_id);
        response.dismissed = escape || scrim_clicked;
    }
    let mut scrim_floating = root_floating(Vec2::ZERO, style.z_index);
    scrim_floating.capture_pointer = true;
    let _scrim = ui.element(
        &ElementDesc::new()
            .id(scrim_id)
            .width(Sizing::Grow)
            .height(Sizing::Grow)
            .align(ChildAlignment::center())
            .background(style.scrim)
            .floating(Some(scrim_floating)),
    );
    let _panel = ui.element(
        &ElementDesc::new()
            .id(panel_id)
            .width(style.width)
            .height(Sizing::Fit)
            .padding(style.padding)
            .child_gap(12)
            .direction(Direction::TopToBottom)
            .background(style.background)
            .corner_radius(CornerRadius::all(style.corner_radius)),
    );
    content(ui);
    response
}

// -- Tabs -------------------------------------------------------------------------------------------------------

/// Look of a [`tabs`] strip.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TabStyle {
    pub idle: Vec4,
    pub hovered: Vec4,
    pub selected: Vec4,
    pub text: TextStyle,
    pub selected_text: Vec4,
    pub corner_radius: f32,
    pub padding: Padding,
}

impl Default for TabStyle {
    fn default() -> Self {
        Self {
            idle: Vec4::ZERO,
            hovered: Vec4::new(1.0, 1.0, 1.0, 0.06),
            selected: Vec4::new(0.35, 0.55, 0.85, 0.25),
            text: TextStyle { font_size: 14, ..TextStyle::default() },
            selected_text: Vec4::ONE,
            corner_radius: 6.0,
            padding: Padding::symmetric(12, 6),
        }
    }
}

/// A horizontal (or, with `vertical`, a sidebar-style vertical) strip of tabs. Returns the selected index after this
/// frame's clicks.
pub fn tabs(ui: &mut Ui<'_>, strip_id: &str, labels: &[&str], selected: usize, vertical: bool, style: &TabStyle) -> usize {
    let mut selected = selected.min(labels.len().saturating_sub(1));
    let _strip = ui.element(
        &ElementDesc::new()
            .id(strip_id.to_owned())
            .width(if vertical { Sizing::Grow } else { Sizing::Fit })
            .height(Sizing::Fit)
            .child_gap(4)
            .direction(if vertical { Direction::TopToBottom } else { Direction::LeftToRight }),
    );
    for (index, label) in labels.iter().enumerate() {
        let tab_id = format!("{strip_id}::{index}");
        if ui.clicked(&tab_id) {
            selected = index;
        }
        let is_selected = index == selected;
        let background = if is_selected {
            style.selected
        } else if ui.hovered(&tab_id) {
            style.hovered
        } else {
            style.idle
        };
        let mut text = style.text;
        if is_selected {
            text.color = style.selected_text;
        }
        let _tab = ui.element(
            &ElementDesc::new()
                .id(tab_id)
                .width(if vertical { Sizing::Grow } else { Sizing::Fit })
                .height(Sizing::Fit)
                .padding(style.padding)
                .background(background)
                .corner_radius(CornerRadius::all(style.corner_radius))
                .cursor(crate::ui::Cursor::Pointer),
        );
        ui.text(label, &text);
    }
    selected
}

// -- Virtualised list -------------------------------------------------------------------------------------------

/// Which rows of a long, fixed-row-height list are visible, so only those are built. Use it inside a vertically
/// clipped container with id `container_id`:
///
/// ```ignore
/// let _list = ui.element(&ElementDesc::new().id("log".into()).width(Sizing::Grow).height(Sizing::Grow)
///     .clip(Clip::vertical()).direction(Direction::TopToBottom));
/// let range = visible_rows(ui, "log", 20.0, lines.len(), 4);
/// spacer(ui, range.before);
/// for line in &lines[range.rows.clone()] { /* one 20 px row */ }
/// spacer(ui, range.after);
/// ```
#[derive(Debug, Clone, PartialEq)]
pub struct VisibleRows {
    pub rows: std::ops::Range<usize>,
    /// Height of the rows above the visible range (draw as a spacer so the scrollbar stays right).
    pub before: f32,
    /// Height of the rows below it.
    pub after: f32,
}

/// See [`VisibleRows`]. `overscan` extra rows are built above and below to hide pop-in while scrolling. The first
/// frame (no layout yet) builds as many rows as fit the window.
pub fn visible_rows(ui: &Ui<'_>, container_id: &str, row_height: f32, row_count: usize, overscan: usize) -> VisibleRows {
    let (scrolled, viewport) = match ui.scroll_metrics(container_id) {
        Some(metrics) => ((-metrics.offset.y).max(0.0), metrics.container_size.y),
        None => (0.0, ui.viewport_size().y),
    };
    visible_rows_for(scrolled, viewport, row_height, row_count, overscan)
}

/// The arithmetic behind [`visible_rows`], for a known scroll position and viewport height. Pure.
pub fn visible_rows_for(scrolled: f32, viewport: f32, row_height: f32, row_count: usize, overscan: usize) -> VisibleRows {
    if row_count == 0 || row_height <= 0.0 {
        return VisibleRows { rows: 0..0, before: 0.0, after: 0.0 };
    }
    let first = ((scrolled / row_height).floor() as usize).saturating_sub(overscan).min(row_count);
    let visible = (viewport / row_height).ceil() as usize + 1 + 2 * overscan;
    let last = (first + visible).min(row_count);
    VisibleRows {
        rows: first..last,
        before: first as f32 * row_height,
        after: (row_count - last) as f32 * row_height,
    }
}

/// Row heights for a long list whose rows differ in height (wrapped text, mixed content). The app keeps one across
/// frames: rows start at an `estimate`, and each row that gets built reports its real height with
/// [`VariableRows::measure`], so offsets converge to the truth as the user scrolls. A scrollbar over unmeasured rows
/// is therefore approximate until they have been seen once, which is inherent to virtualising without laying out
/// everything.
///
/// ```ignore
/// rows.set_len(items.len());
/// let _list = ui.element(&ElementDesc::new().id("feed".into()).height(Sizing::Grow).clip(Clip::vertical())
///     .direction(Direction::TopToBottom));
/// let range = rows.visible(ui, "feed", 3);
/// spacer(ui, range.before);
/// for index in range.rows.clone() {
///     let id = format!("feed-row-{index}");
///     { let _row = ui.element(&ElementDesc::new().id(id.as_str().into()) /* ... */); /* row contents */ }
///     rows.measure(ui, index, &id);
/// }
/// spacer(ui, range.after);
/// ```
#[derive(Debug, Clone)]
pub struct VariableRows {
    estimate: f32,
    /// Measured height per row; `0.0` = not measured yet (the estimate applies).
    measured: Vec<f32>,
    /// `starts[i]` = top of row `i`; one extra entry holds the total. Rebuilt lazily after a change.
    starts: Vec<f32>,
    dirty: bool,
}

impl VariableRows {
    /// A list with every row assumed `estimate` pixels tall until measured.
    pub fn new(estimate: f32) -> Self {
        Self { estimate: estimate.max(1.0), measured: Vec::new(), starts: vec![0.0], dirty: false }
    }

    /// Number of rows. Growing keeps what is measured; shrinking drops the tail.
    pub fn set_len(&mut self, len: usize) {
        if self.measured.len() != len {
            self.measured.resize(len, 0.0);
            self.dirty = true;
        }
    }

    /// Forgets every measurement (the width changed and text re-wraps, the data was replaced).
    pub fn invalidate(&mut self) {
        self.measured.iter_mut().for_each(|h| *h = 0.0);
        self.dirty = true;
    }

    pub fn len(&self) -> usize {
        self.measured.len()
    }

    pub fn is_empty(&self) -> bool {
        self.measured.is_empty()
    }

    /// Records the real height of row `index`; ignores non-positive or non-finite values.
    pub fn set_height(&mut self, index: usize, height: f32) {
        if let Some(slot) = self.measured.get_mut(index) {
            if height.is_finite() && height > 0.0 && (*slot - height).abs() > 0.25 {
                *slot = height;
                self.dirty = true;
            }
        }
    }

    /// Reads row `index`'s height from the element with `element_id` (as of the last finished layout) and records it.
    pub fn measure(&mut self, ui: &Ui<'_>, index: usize, element_id: &str) {
        if let Some((_, size)) = ui.element_bounds(element_id) {
            self.set_height(index, size.y);
        }
    }

    /// Height of row `index` (its measurement, else the estimate).
    pub fn height(&self, index: usize) -> f32 {
        match self.measured.get(index) {
            Some(&h) if h > 0.0 => h,
            _ => self.estimate,
        }
    }

    fn rebuild(&mut self) {
        if !self.dirty && self.starts.len() == self.measured.len() + 1 {
            return;
        }
        self.starts.clear();
        self.starts.reserve(self.measured.len() + 1);
        let mut y = 0.0;
        for &h in &self.measured {
            self.starts.push(y);
            y += if h > 0.0 { h } else { self.estimate };
        }
        self.starts.push(y);
        self.dirty = false;
    }

    /// Total content height.
    pub fn total_height(&mut self) -> f32 {
        self.rebuild();
        *self.starts.last().unwrap_or(&0.0)
    }

    /// Top of row `index` (the total height for `index == len`).
    pub fn offset_of(&mut self, index: usize) -> f32 {
        self.rebuild();
        self.starts[index.min(self.measured.len())]
    }

    /// The row containing vertical offset `y` (clamped to the last row).
    pub fn index_at(&mut self, y: f32) -> usize {
        self.rebuild();
        if self.measured.is_empty() {
            return 0;
        }
        let count = self.measured.len();
        // Last row whose start is <= y.
        let after = self.starts[..count].partition_point(|&start| start <= y);
        after.saturating_sub(1).min(count - 1)
    }

    /// Which rows to build for a known scroll position and viewport height. Pure given the measurements.
    pub fn visible_for(&mut self, scrolled: f32, viewport: f32, overscan: usize) -> VisibleRows {
        let count = self.measured.len();
        if count == 0 {
            return VisibleRows { rows: 0..0, before: 0.0, after: 0.0 };
        }
        let scrolled = scrolled.max(0.0);
        let total = self.total_height();
        if scrolled >= total {
            // Scrolled past the end (rows shrank): show the tail so the list is never empty.
            let first = count.saturating_sub(1 + overscan);
            return VisibleRows { rows: first..count, before: self.offset_of(first), after: 0.0 };
        }
        let first_visible = self.index_at(scrolled);
        let last_visible = self.index_at(scrolled + viewport.max(0.0));
        let first = first_visible.saturating_sub(overscan);
        let last = (last_visible + 1 + overscan).min(count);
        VisibleRows { rows: first..last, before: self.offset_of(first), after: total - self.offset_of(last) }
    }

    /// [`VariableRows::visible_for`] for the scroll position and viewport of the container with `container_id`. The
    /// first frame (no layout yet) uses the window height.
    pub fn visible(&mut self, ui: &Ui<'_>, container_id: &str, overscan: usize) -> VisibleRows {
        let (scrolled, viewport) = match ui.scroll_metrics(container_id) {
            Some(metrics) => ((-metrics.offset.y).max(0.0), metrics.container_size.y),
            None => (0.0, ui.viewport_size().y),
        };
        self.visible_for(scrolled, viewport, overscan)
    }
}

/// An empty element of `height` pixels (the before/after padding of a virtualised list).
pub fn spacer(ui: &mut Ui<'_>, height: f32) {
    if height > 0.0 {
        let _spacer: ElementScope = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(height)));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn place_near_prefers_below_and_flips_above_at_the_bottom() {
        let viewport = Vec2::new(800.0, 600.0);
        let below = place_near((Vec2::new(100.0, 100.0), Vec2::new(80.0, 30.0)), Vec2::new(200.0, 100.0), viewport, 4.0);
        assert_eq!(below, Vec2::new(100.0, 134.0));
        let flipped = place_near((Vec2::new(100.0, 550.0), Vec2::new(80.0, 30.0)), Vec2::new(200.0, 100.0), viewport, 4.0);
        assert_eq!(flipped.y, 446.0);
        let shifted = place_near((Vec2::new(750.0, 100.0), Vec2::new(40.0, 30.0)), Vec2::new(200.0, 100.0), viewport, 4.0);
        assert_eq!(shifted.x, 600.0);
    }

    #[test]
    fn clamp_keeps_boxes_on_screen() {
        let viewport = Vec2::new(800.0, 600.0);
        assert_eq!(clamp_to_viewport(Vec2::new(790.0, 590.0), Vec2::new(100.0, 50.0), viewport), Vec2::new(700.0, 550.0));
        assert_eq!(clamp_to_viewport(Vec2::new(-5.0, -5.0), Vec2::new(10.0, 10.0), viewport), Vec2::ZERO);
    }

    #[test]
    fn variable_rows_use_estimates_then_measurements() {
        let mut rows = VariableRows::new(20.0);
        rows.set_len(100);
        assert_eq!(rows.total_height(), 2000.0);
        assert_eq!(rows.offset_of(10), 200.0);
        rows.set_height(0, 100.0);
        rows.set_height(1, 40.0);
        assert_eq!(rows.offset_of(2), 140.0);
        assert_eq!(rows.total_height(), 2000.0 - 40.0 + 140.0);
        assert_eq!(rows.index_at(0.0), 0);
        assert_eq!(rows.index_at(99.9), 0);
        assert_eq!(rows.index_at(100.0), 1);
        assert_eq!(rows.index_at(140.0), 2);
        assert_eq!(rows.index_at(1.0e9), 99);
    }

    #[test]
    fn variable_rows_visible_range_is_consistent() {
        let mut rows = VariableRows::new(20.0);
        rows.set_len(1000);
        rows.set_height(50, 300.0);
        let total = rows.total_height();
        for scrolled in [0.0, 500.0, 1000.0, 5000.0, 19_000.0] {
            let r = rows.visible_for(scrolled, 200.0, 2);
            assert!(!r.rows.is_empty());
            let built: f32 = r.rows.clone().map(|i| rows.height(i)).sum();
            assert!((r.before + built + r.after - total).abs() < 0.5, "scrolled {scrolled}");
            assert!(r.before <= scrolled.max(0.0));
        }
        let past = rows.visible_for(1.0e9, 200.0, 2);
        assert_eq!(past.rows.end, 1000);
        assert_eq!(past.after, 0.0);
        assert_eq!(VariableRows::new(10.0).visible_for(0.0, 100.0, 2).rows, 0..0);
    }

    #[test]
    fn variable_rows_ignore_bad_measurements_and_keep_them_on_growth() {
        let mut rows = VariableRows::new(10.0);
        rows.set_len(5);
        rows.set_height(1, f32::NAN);
        rows.set_height(2, -3.0);
        rows.set_height(9, 50.0);
        assert_eq!(rows.total_height(), 50.0);
        rows.set_height(3, 30.0);
        rows.set_len(8);
        assert_eq!(rows.height(3), 30.0);
        rows.invalidate();
        assert_eq!(rows.height(3), 10.0);
    }

    #[test]
    fn visible_rows_cover_the_viewport_with_overscan() {
        let r = visible_rows_for(0.0, 100.0, 20.0, 1000, 2);
        assert_eq!(r.rows.start, 0);
        assert!(r.rows.end >= 5);
        assert_eq!(r.before, 0.0);
        let r = visible_rows_for(2000.0, 100.0, 20.0, 1000, 2);
        assert_eq!(r.rows.start, 98);
        assert_eq!(r.before, 98.0 * 20.0);
        assert!((r.before + (r.rows.len() as f32) * 20.0 + r.after - 20000.0).abs() < 0.01);
        let r = visible_rows_for(1_000_000.0, 100.0, 20.0, 10, 2);
        assert_eq!(r.rows, 10..10);
        assert_eq!(visible_rows_for(0.0, 100.0, 20.0, 0, 2).rows, 0..0);
    }
}

// -- Focus navigation (keyboard and gamepad) ----------------------------------------------------------------------

/// A navigation request for one frame, from whatever input the app reads: [`NavInput::from_keyboard`] for the
/// keyboard, or the app's own gamepad layer (D-pad / stick to `direction`, A to `activate`, B to `back`).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct NavInput {
    pub direction: Option<NavDirection>,
    /// Press the focused element (Enter / Space / gamepad A).
    pub activate: bool,
    /// Go back / close (Escape / gamepad B). Reported by [`FocusNavigator::back_pressed`]; what it means is the app's.
    pub back: bool,
    /// Scroll the focused element's container (right stick / page keys), pixels this frame.
    pub scroll: Vec2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NavDirection {
    /// Tab order (declaration order).
    Next,
    Previous,
    /// Spatial: the nearest focusable whose centre lies in that direction.
    Up,
    Down,
    Left,
    Right,
}

impl NavInput {
    /// Tab / Shift+Tab, arrow keys, Enter / Space, Escape and Page Up / Page Down from the engine's keyboard state.
    /// Skip it while a text field has focus if the arrows should move its caret instead.
    pub fn from_keyboard(engine: &crate::Engine<'_>) -> Self {
        let input = engine.input();
        let shift = input.modifiers().shift;
        let direction = if input.key_just_pressed(Key::Tab) {
            Some(if shift { NavDirection::Previous } else { NavDirection::Next })
        } else if input.key_just_pressed(Key::Up) {
            Some(NavDirection::Up)
        } else if input.key_just_pressed(Key::Down) {
            Some(NavDirection::Down)
        } else if input.key_just_pressed(Key::Left) {
            Some(NavDirection::Left)
        } else if input.key_just_pressed(Key::Right) {
            Some(NavDirection::Right)
        } else {
            None
        };
        let page = if input.key_just_pressed(Key::PageDown) {
            -400.0
        } else if input.key_just_pressed(Key::PageUp) {
            400.0
        } else {
            0.0
        };
        Self {
            direction,
            activate: input.key_just_pressed(Key::Enter) || input.key_just_pressed(Key::Space),
            back: input.key_just_pressed(Key::Escape),
            scroll: Vec2::new(0.0, page),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.direction.is_none() && !self.activate && !self.back && self.scroll == Vec2::ZERO
    }
}

/// Moves keyboard/gamepad focus through the elements an app declares focusable, in tab order or spatially, and
/// reports which one was activated. Keep one per window across frames.
///
/// Each frame: call [`begin_frame`](Self::begin_frame) with the frame's [`NavInput`], then
/// [`focusable`](Self::focusable) for every focusable element id as it is built (in tab order; give each an
/// `ElementDesc` id), and check [`activated`](Self::activated) to treat an activation like a click. Draw a focus ring
/// where [`is_focused`](Self::is_focused) holds. Navigation uses the previous frame's layout, so it is always one
/// frame behind the tree, which is invisible in practice.
#[derive(Debug, Clone, Default)]
pub struct FocusNavigator {
    /// Focusable ids of the frame being built, in declaration order, with the scroll container each lives in.
    current: Vec<(String, Option<String>)>,
    /// Last frame's list (what navigation searches).
    previous: Vec<(String, Option<String>)>,
    focused: Option<String>,
    pending: NavInput,
    activated: Option<String>,
    back: bool,
    /// Set when focus moved, so the app can scroll it into view and request a redraw.
    moved: bool,
}

impl FocusNavigator {
    /// Starts a frame: resolves `input` against last frame's focusables (moving focus, scrolling, activating).
    pub fn begin_frame(&mut self, ui: &mut Ui<'_>, input: NavInput) {
        self.previous = std::mem::take(&mut self.current);
        self.pending = input;
        self.activated = None;
        self.back = input.back;
        self.moved = false;

        if self.focused.as_ref().is_some_and(|id| !self.previous.iter().any(|(known, _)| known == id)) {
            self.focused = None; // the focused element went away
        }
        if let Some(direction) = input.direction {
            let next = self.find(ui, direction);
            if next.is_some() && next != self.focused {
                self.focused = next;
                self.moved = true;
            }
        }
        if let Some(id) = self.focused.clone() {
            ui.focus(&id);
            let container = self.previous.iter().find(|(known, _)| *known == id).and_then(|(_, c)| c.clone());
            if let Some(container) = container {
                if self.moved {
                    ui.scroll_into_view(&container, &id);
                }
                if input.scroll != Vec2::ZERO {
                    if let Some(metrics) = ui.scroll_metrics(&container) {
                        ui.set_scroll_offset(&container, metrics.offset + input.scroll);
                    }
                }
            }
            if input.activate {
                self.activated = Some(id);
            }
        }
    }

    /// Declares `id` focusable this frame. `scroll_container` is the id of the clipped container it scrolls in, if
    /// any, so focus moves scroll it into view. Returns whether it has focus (draw the focus ring).
    pub fn focusable(&mut self, id: &str, scroll_container: Option<&str>) -> bool {
        self.current.push((id.to_owned(), scroll_container.map(str::to_owned)));
        self.is_focused(id)
    }

    pub fn is_focused(&self, id: &str) -> bool {
        self.focused.as_deref() == Some(id)
    }

    /// Whether `id` was activated (Enter / Space / gamepad A) this frame.
    pub fn activated(&self, id: &str) -> bool {
        self.activated.as_deref() == Some(id)
    }

    /// Whether "back" was pressed this frame.
    pub fn back_pressed(&self) -> bool {
        self.back
    }

    /// Whether focus moved this frame (request a redraw in on-demand mode so the ring follows).
    pub fn moved(&self) -> bool {
        self.moved
    }

    /// Focuses `id` directly (a click, a dialog opening).
    pub fn set_focus(&mut self, id: Option<&str>) {
        self.focused = id.map(str::to_owned);
    }

    pub fn focused(&self) -> Option<&str> {
        self.focused.as_deref()
    }

    fn find(&self, ui: &Ui<'_>, direction: NavDirection) -> Option<String> {
        if self.previous.is_empty() {
            return None;
        }
        let index = self.focused.as_ref().and_then(|id| self.previous.iter().position(|(known, _)| known == id));
        match direction {
            NavDirection::Next => {
                let next = index.map_or(0, |i| (i + 1) % self.previous.len());
                Some(self.previous[next].0.clone())
            }
            NavDirection::Previous => {
                let previous = index.map_or(self.previous.len() - 1, |i| (i + self.previous.len() - 1) % self.previous.len());
                Some(self.previous[previous].0.clone())
            }
            spatial => {
                let Some(index) = index else { return Some(self.previous[0].0.clone()) };
                let from = ui.element_bounds(&self.previous[index].0)?;
                let candidates: Vec<(usize, (Vec2, Vec2))> = self
                    .previous
                    .iter()
                    .enumerate()
                    .filter(|(i, _)| *i != index)
                    .filter_map(|(i, (id, _))| ui.element_bounds(id).map(|b| (i, b)))
                    .collect();
                let rects: Vec<(Vec2, Vec2)> = candidates.iter().map(|(_, b)| *b).collect();
                nearest_in_direction(from, &rects, spatial).map(|pick| self.previous[candidates[pick].0].0.clone())
            }
        }
    }
}

/// Index into `candidates` (position, size) of the best target for moving focus from `from` in `direction`: the
/// candidate whose centre lies in that direction, scoring distance along the direction plus twice the sideways
/// offset (so a target straight ahead beats a closer diagonal one). Pure.
pub fn nearest_in_direction(from: (Vec2, Vec2), candidates: &[(Vec2, Vec2)], direction: NavDirection) -> Option<usize> {
    let centre = |(pos, size): (Vec2, Vec2)| pos + size * 0.5;
    let origin = centre(from);
    let axis = match direction {
        NavDirection::Up => Vec2::new(0.0, -1.0),
        NavDirection::Down => Vec2::new(0.0, 1.0),
        NavDirection::Left => Vec2::new(-1.0, 0.0),
        NavDirection::Right => Vec2::new(1.0, 0.0),
        NavDirection::Next | NavDirection::Previous => return None,
    };
    candidates
        .iter()
        .enumerate()
        .filter_map(|(i, rect)| {
            let delta = centre(*rect) - origin;
            let along = delta.dot(axis);
            if along <= 0.5 {
                return None;
            }
            let sideways = (delta - axis * along).length();
            Some((i, along + 2.0 * sideways))
        })
        .min_by(|a, b| a.1.total_cmp(&b.1))
        .map(|(i, _)| i)
}

#[cfg(test)]
mod focus_tests {
    use super::*;

    #[test]
    fn spatial_navigation_prefers_straight_ahead() {
        let from = (Vec2::new(0.0, 0.0), Vec2::new(100.0, 30.0));
        let candidates = [
            (Vec2::new(0.0, 60.0), Vec2::new(100.0, 30.0)),   // directly below
            (Vec2::new(150.0, 40.0), Vec2::new(100.0, 30.0)), // below-right, nearer vertically
            (Vec2::new(0.0, -60.0), Vec2::new(100.0, 30.0)),  // above
        ];
        assert_eq!(nearest_in_direction(from, &candidates, NavDirection::Down), Some(0));
        assert_eq!(nearest_in_direction(from, &candidates, NavDirection::Up), Some(2));
        assert_eq!(nearest_in_direction(from, &candidates, NavDirection::Right), Some(1));
        assert_eq!(nearest_in_direction(from, &candidates, NavDirection::Left), None);
    }
}

// -- Animated images (GIF / APNG / animated WebP) -----------------------------------------------------------------

/// An animated image playing in the UI: decodes every frame once, uploads the first as a texture, and swaps the
/// texture's pixels in place as frames come due (see [`crate::assets::Assets::update_texture`]). Draw it with
/// [`Ui::image`] using [`texture`](Self::texture); call [`advance`](Self::advance) once per frame and pass its result to
/// `request_redraw_after` so an on-demand app wakes exactly when the next frame is due.
pub struct AnimatedImage {
    image: crate::images::DecodedImage,
    asset: crate::assets::Asset,
    texture: crate::assets::RendererTextureHandle,
    frame: usize,
    /// Time shown on the current frame.
    elapsed_ms: f64,
    loops_done: u32,
    playing: bool,
}

impl AnimatedImage {
    /// Decodes `encoded` (any format the engine decodes; still images work too and simply never advance) and uploads
    /// its first frame. `label` names the texture in diagnostics.
    pub fn new(engine: &mut crate::Engine<'_>, encoded: &[u8], label: &str) -> Result<Self, String> {
        let options = crate::images::DecodeOptions { decode_all_frames: true, ..Default::default() };
        let image = crate::images::decode(encoded, options, label).map_err(|e| e.to_string())?;
        if image.format != crate::images::PixelFormat::Rgba8 {
            return Err(format!("{label}: animated images must decode to RGBA8"));
        }
        let mut assets = engine.assets();
        let asset = assets
            .create_texture(
                image.pixels(),
                image.width,
                image.height,
                crate::assets::TexturePixelFormat::Rgba8,
                crate::assets::TextureColorSpace::Srgb,
                crate::assets::TextureKind::ColorAlpha,
                label,
                false,
                false,
            )
            .map_err(|e| format!("{e:?}"))?;
        let texture = assets.texture_handle(asset).map_err(|e| format!("{e:?}"))?;
        Ok(Self { image, asset, texture, frame: 0, elapsed_ms: 0.0, loops_done: 0, playing: true })
    }

    /// The texture to draw.
    pub fn texture(&self) -> crate::assets::RendererTextureHandle {
        self.texture
    }

    /// The backing asset (unload it with [`crate::assets::Assets::unload`] when done).
    pub fn asset(&self) -> crate::assets::Asset {
        self.asset
    }

    pub fn size(&self) -> (u32, u32) {
        (self.image.width, self.image.height)
    }

    pub fn is_animated(&self) -> bool {
        self.image.frames.len() > 1
    }

    pub fn set_playing(&mut self, playing: bool) {
        self.playing = playing;
    }

    /// Advances by `delta_seconds`, uploading a new frame when one comes due. Returns how long until the next frame
    /// change (for `request_redraw_after`), or `None` for a still image, a paused one, or a finished non-looping one.
    pub fn advance(&mut self, engine: &mut crate::Engine<'_>, delta_seconds: f64) -> Option<Duration> {
        if !self.playing || self.image.frames.len() < 2 {
            return None;
        }
        self.elapsed_ms += delta_seconds * 1000.0;
        let mut changed = false;
        loop {
            // Browsers treat 0 and very short delays as 100 ms; doing the same keeps GIFs at their intended speed.
            let duration = f64::from(match self.image.frames[self.frame].duration_ms {
                0..=10 => 100,
                ms => ms,
            });
            if self.elapsed_ms < duration {
                if changed {
                    let pixels = &self.image.frames[self.frame].pixels;
                    if let Err(error) = engine.assets().update_texture(self.asset, pixels) {
                        // A failed upload leaves the previous frame on screen; try again next frame.
                        let _ = error;
                    }
                }
                return Some(Duration::from_secs_f64(((duration - self.elapsed_ms) / 1000.0).max(0.001)));
            }
            self.elapsed_ms -= duration;
            self.frame += 1;
            changed = true;
            if self.frame == self.image.frames.len() {
                self.loops_done += 1;
                if self.image.loop_count != 0 && self.loops_done >= self.image.loop_count {
                    self.frame = self.image.frames.len() - 1;
                    self.playing = false;
                    let _ = engine.assets().update_texture(self.asset, &self.image.frames[self.frame].pixels);
                    return None;
                }
                self.frame = 0;
            }
        }
    }
}

// -- Pointer and keyboard conveniences ----------------------------------------------------------------------------

/// Whether the pointer pressed `button` over element `id` this frame. The engine's own `Ui::clicked` is the primary
/// button; use this for secondary-click context menus and middle-click actions:
///
/// ```ignore
/// if ui_kit::pressed(ui, "instance-row", MouseButton::Right) {
///     menu.open_at(ui.pointer_position());
/// }
/// ```
pub fn pressed(ui: &mut Ui<'_>, id: &str, button: crate::MouseButton) -> bool {
    ui.hovered(id) && ui.engine().input().mouse_just_pressed(button)
}

/// A keyboard shortcut: a key plus required modifiers (exactly these; extra modifiers do not match).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Shortcut {
    pub key: Key,
    pub control: bool,
    pub shift: bool,
    pub alt: bool,
}

impl Shortcut {
    pub const fn key(key: Key) -> Self {
        Self { key, control: false, shift: false, alt: false }
    }

    /// Ctrl (Cmd on macOS is the app's choice: map it here) plus `key`.
    pub const fn ctrl(key: Key) -> Self {
        Self { key, control: true, shift: false, alt: false }
    }

    #[must_use]
    pub const fn with_shift(mut self) -> Self {
        self.shift = true;
        self
    }

    #[must_use]
    pub const fn with_alt(mut self) -> Self {
        self.alt = true;
        self
    }

    /// Whether the shortcut was pressed this frame.
    pub fn triggered(&self, engine: &crate::Engine<'_>) -> bool {
        let input = engine.input();
        let modifiers = input.modifiers();
        input.key_just_pressed(self.key)
            && modifiers.control == self.control
            && modifiers.shift == self.shift
            && modifiers.alt == self.alt
    }
}

// -- Keeping an on-demand UI awake while it animates --------------------------------------------------------------

/// In [`crate::app::FrameMode::OnDemand`] the engine renders when input arrives, but the UI's own animations (hover
/// fades, the scrollbar fading out, a button easing back) outlive the last input event: without more frames they
/// freeze halfway. `AnimationKeepAlive` watches the pointer and, for `settle` after any activity, asks for a frame
/// every ~16 ms; then it stops asking and the app goes back to sleep.
///
/// Call [`update`](Self::update) once per frame inside the draw hook; call [`keep_awake_for`](Self::keep_awake_for)
/// for animations of your own (a notification sliding in).
#[derive(Debug, Default)]
pub struct AnimationKeepAlive {
    until: Option<std::time::Instant>,
    last_pointer: Option<(Vec2, bool)>,
}

impl AnimationKeepAlive {
    /// Stay awake for at least `duration` from now.
    pub fn keep_awake_for(&mut self, duration: Duration) {
        let candidate = std::time::Instant::now() + duration;
        self.until = Some(self.until.map_or(candidate, |current| current.max(candidate)));
    }

    /// Notes pointer activity and requests the next frame while awake. Returns whether a frame was requested.
    pub fn update(&mut self, ui: &mut Ui<'_>, settle: Duration) -> bool {
        let pointer = ui.pointer_state();
        let state = (pointer.position, pointer.down);
        let active = self.last_pointer != Some(state)
            || pointer.pressed_this_frame
            || pointer.released_this_frame
            || pointer.scroll_delta != Vec2::ZERO;
        self.last_pointer = Some(state);
        if active {
            self.keep_awake_for(settle);
        }
        match self.until {
            Some(until) if std::time::Instant::now() < until => {
                ui.engine().request_redraw_after(Duration::from_millis(16));
                true
            }
            _ => false,
        }
    }
}
