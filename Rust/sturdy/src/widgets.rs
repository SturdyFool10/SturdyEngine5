//! Stateful UI widgets: buttons, checkboxes/radio buttons/switches, sliders, text inputs and text
//! areas, scroll areas and dropdowns.
//!
//! They are methods on [`Ui`], usable inside the draw hook like the element primitives, and each
//! widget is identified by the `id` of the [`ElementDesc`] you pass it. The engine keeps per-widget
//! state between frames (hover/press animation, slider drag, text caret and selection, scrollbar
//! fade, dropdown open state); that state lives in the `*State` objects here — create one per widget
//! and keep it alive across frames (typically inside the closure you install with
//! [`crate::ui::install_draw_hook`]).
//!
//! Widgets do not own your data: a slider takes the current `value` and reports the new one, a
//! checkbox takes `checked` and reports `clicked`. Apply the change yourself.
//!
//! Colours are `glam::Vec4` RGBA; the `Default` styles are the engine's own.

use core::pin::Pin;

use cxx::UniquePtr;
use glam::Vec4;
use sturdy_sys::ui::EngineView;
use sturdy_sys::ui_widgets::{ffi, MasonryContent, ScrollContent};

use crate::ui::{ElementDesc, ElementScope, Ui};

// -- Styles ------------------------------------------------------------------------------------

/// Colours and timing for [`Ui::button`] (and a dropdown's trigger).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ButtonStyle {
    pub idle: Vec4,
    pub hovered: Vec4,
    pub pressed: Vec4,
    pub disabled: Vec4,
    pub corner_radius: f32,
    pub transition_seconds: f32,
}

impl Default for ButtonStyle {
    fn default() -> Self {
        Self {
            idle: Vec4::new(0.25, 0.27, 0.32, 1.0),
            hovered: Vec4::new(0.32, 0.35, 0.42, 1.0),
            pressed: Vec4::new(0.18, 0.19, 0.24, 1.0),
            disabled: Vec4::new(0.2, 0.2, 0.2, 0.5),
            corner_radius: 0.0,
            transition_seconds: 0.25,
        }
    }
}

impl ButtonStyle {
    fn to_ffi(self) -> ffi::ButtonStyleDesc {
        ffi::ButtonStyleDesc {
            idle: self.idle.into(),
            hovered: self.hovered.into(),
            pressed: self.pressed.into(),
            disabled: self.disabled.into(),
            corner_radius: self.corner_radius,
            transition_seconds: self.transition_seconds,
        }
    }
}

/// Colours and timing for checkboxes, radio buttons and switches.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ToggleStyle {
    pub idle: Vec4,
    pub hovered: Vec4,
    pub checked: Vec4,
    pub disabled: Vec4,
    pub mark_color: Vec4,
    pub transition_seconds: f32,
}

impl Default for ToggleStyle {
    fn default() -> Self {
        Self {
            idle: Vec4::new(0.2, 0.21, 0.26, 1.0),
            hovered: Vec4::new(0.28, 0.3, 0.36, 1.0),
            checked: Vec4::new(0.35, 0.55, 0.85, 1.0),
            disabled: Vec4::new(0.2, 0.2, 0.2, 0.5),
            mark_color: Vec4::ONE,
            transition_seconds: 0.25,
        }
    }
}

impl ToggleStyle {
    fn to_ffi(self) -> ffi::ToggleStyleDesc {
        ffi::ToggleStyleDesc {
            idle: self.idle.into(),
            hovered: self.hovered.into(),
            checked: self.checked.into(),
            disabled: self.disabled.into(),
            mark_color: self.mark_color.into(),
            transition_seconds: self.transition_seconds,
        }
    }
}

/// Range and behaviour of a [`Ui::slider`].
#[derive(Debug, Clone, PartialEq)]
pub struct SliderConfig {
    pub min: f64,
    pub max: f64,
    /// Snap increment; `None` is continuous.
    pub step: Option<f64>,
    pub keyboard_step: Option<f64>,
    pub page_step: Option<f64>,
    pub vertical: bool,
    pub reversed: bool,
    pub show_step_ticks: bool,
    /// Explicit tick marks at these values.
    pub ticks: Vec<f64>,
}

impl Default for SliderConfig {
    fn default() -> Self {
        Self {
            min: 0.0,
            max: 100.0,
            step: Some(1.0),
            keyboard_step: None,
            page_step: None,
            vertical: false,
            reversed: false,
            show_step_ticks: false,
            ticks: Vec::new(),
        }
    }
}

impl SliderConfig {
    pub fn range(min: f64, max: f64) -> Self {
        Self { min, max, ..Self::default() }
    }

    #[must_use]
    pub fn step(mut self, step: Option<f64>) -> Self {
        self.step = step;
        self
    }
}

/// A keyboard action applied to a slider this frame ([`SliderInput::keys`]).
pub use ffi::SliderKeyKind as SliderKey;

/// Per-frame slider input beyond the pointer.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SliderInput {
    pub keys: Vec<SliderKey>,
    pub request_focus: bool,
    pub request_blur: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SliderStyle {
    pub track: Vec4,
    pub fill: Vec4,
    pub thumb: Vec4,
    pub thumb_hovered: Vec4,
    pub thumb_dragging: Vec4,
    pub tick: Vec4,
    pub track_thickness: f32,
    pub thumb_size: f32,
}

impl Default for SliderStyle {
    fn default() -> Self {
        Self {
            track: Vec4::new(0.20, 0.21, 0.26, 1.0),
            fill: Vec4::new(0.35, 0.55, 0.85, 1.0),
            thumb: Vec4::new(0.92, 0.93, 0.95, 1.0),
            thumb_hovered: Vec4::ONE,
            thumb_dragging: Vec4::new(0.75, 0.85, 1.0, 1.0),
            tick: Vec4::new(0.48, 0.50, 0.56, 1.0),
            track_thickness: 6.0,
            thumb_size: 18.0,
        }
    }
}

/// Look of a text input / text area.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TextInputStyle {
    pub idle: Vec4,
    pub hovered: Vec4,
    pub focused: Vec4,
    pub disabled: Vec4,
    pub border_idle: Vec4,
    pub border_focused: Vec4,
    pub text_color: Vec4,
    pub placeholder_color: Vec4,
    pub selection_color: Vec4,
    pub caret_color: Vec4,
    pub corner_radius: f32,
    /// A font registered with [`Ui::register_font`].
    pub font_id: u16,
    pub font_size: u16,
    pub caret_blink_seconds: f32,
    /// Show a bullet instead of each character (password fields).
    pub mask_characters: bool,
    pub wrap_lines: bool,
}

impl Default for TextInputStyle {
    fn default() -> Self {
        Self {
            idle: Vec4::new(0.14, 0.15, 0.19, 1.0),
            hovered: Vec4::new(0.17, 0.18, 0.23, 1.0),
            focused: Vec4::new(0.12, 0.13, 0.17, 1.0),
            disabled: Vec4::new(0.14, 0.15, 0.19, 0.5),
            border_idle: Vec4::new(0.3, 0.32, 0.38, 1.0),
            border_focused: Vec4::new(0.35, 0.55, 0.85, 1.0),
            text_color: Vec4::new(0.92, 0.93, 0.95, 1.0),
            placeholder_color: Vec4::new(0.5, 0.52, 0.56, 1.0),
            selection_color: Vec4::new(0.28, 0.45, 0.75, 0.45),
            caret_color: Vec4::ONE,
            corner_radius: 6.0,
            font_id: 0,
            font_size: 16,
            caret_blink_seconds: 0.53,
            mask_characters: false,
            wrap_lines: false,
        }
    }
}

impl TextInputStyle {
    fn to_ffi(self) -> ffi::TextEditStyleDesc {
        ffi::TextEditStyleDesc {
            idle: self.idle.into(),
            hovered: self.hovered.into(),
            focused: self.focused.into(),
            disabled: self.disabled.into(),
            border_idle: self.border_idle.into(),
            border_focused: self.border_focused.into(),
            text_color: self.text_color.into(),
            placeholder_color: self.placeholder_color.into(),
            selection_color: self.selection_color.into(),
            caret_color: self.caret_color.into(),
            corner_radius: self.corner_radius,
            font_id: self.font_id,
            font_size: self.font_size,
            caret_blink_seconds: self.caret_blink_seconds,
            mask_characters: self.mask_characters,
            wrap_lines: self.wrap_lines,
        }
    }
}

/// When a scrollbar is shown.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ScrollbarVisibility {
    #[default]
    WhenNeeded,
    AlwaysVisible,
    AlwaysHidden,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ScrollbarStyle {
    pub idle_thickness: f32,
    pub hovered_thickness: f32,
    pub track_color: Vec4,
    pub thumb_color: Vec4,
    pub thumb_hovered_color: Vec4,
    pub thumb_dragging_color: Vec4,
    pub corner_radius: f32,
    pub margin: f32,
    pub min_thumb_length: f32,
    pub visibility: ScrollbarVisibility,
}

impl Default for ScrollbarStyle {
    fn default() -> Self {
        Self {
            idle_thickness: 4.0,
            hovered_thickness: 8.0,
            track_color: Vec4::ZERO,
            thumb_color: Vec4::new(1.0, 1.0, 1.0, 0.28),
            thumb_hovered_color: Vec4::new(1.0, 1.0, 1.0, 0.45),
            thumb_dragging_color: Vec4::new(1.0, 1.0, 1.0, 0.65),
            corner_radius: 3.0,
            margin: 2.0,
            min_thumb_length: 24.0,
            visibility: ScrollbarVisibility::WhenNeeded,
        }
    }
}

impl ScrollbarStyle {
    fn to_ffi(self) -> ffi::ScrollbarStyleDesc {
        ffi::ScrollbarStyleDesc {
            idle_thickness: self.idle_thickness,
            hovered_thickness: self.hovered_thickness,
            track_color: self.track_color.into(),
            thumb_color: self.thumb_color.into(),
            thumb_hovered_color: self.thumb_hovered_color.into(),
            thumb_dragging_color: self.thumb_dragging_color.into(),
            corner_radius: self.corner_radius,
            margin: self.margin,
            min_thumb_length: self.min_thumb_length,
            visibility: match self.visibility {
                ScrollbarVisibility::WhenNeeded => 0,
                ScrollbarVisibility::AlwaysVisible => 1,
                ScrollbarVisibility::AlwaysHidden => 2,
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DropdownStyle {
    pub trigger: ButtonStyle,
    pub list_background: Vec4,
    pub option_hovered: Vec4,
    pub corner_radius: f32,
    pub option_padding: u16,
    pub show_arrow_indicator: bool,
    pub arrow_color: Vec4,
    pub arrow_font_id: u16,
}

impl Default for DropdownStyle {
    fn default() -> Self {
        Self {
            trigger: ButtonStyle::default(),
            list_background: Vec4::new(0.14, 0.15, 0.19, 0.98),
            option_hovered: Vec4::new(0.24, 0.26, 0.32, 1.0),
            corner_radius: 8.0,
            option_padding: 8,
            show_arrow_indicator: true,
            arrow_color: Vec4::ONE,
            arrow_font_id: 0,
        }
    }
}

// -- Results -----------------------------------------------------------------------------------

/// Interaction result of a button, checkbox, radio button or switch.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ToggleResult {
    pub hovered: bool,
    pub pressed: bool,
    /// Released over the widget this frame: flip your own state.
    pub clicked: bool,
}

impl From<ffi::ToggleOutcome> for ToggleResult {
    fn from(o: ffi::ToggleOutcome) -> Self {
        Self { hovered: o.hovered, pressed: o.pressed, clicked: o.clicked }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct SliderResult {
    /// The value after this frame's interaction (sanitized to the range and step).
    pub value: f64,
    pub changed: bool,
    /// The user finished a drag or key adjustment.
    pub committed: bool,
    pub adjusted: bool,
    pub cancelled: bool,
    pub hovered: bool,
    pub dragging: bool,
    pub focused: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TextInputResult {
    /// The text changed this frame; read it with [`TextEditState::text`].
    pub changed: bool,
    /// Enter was pressed (single-line inputs).
    pub submitted: bool,
    pub focused: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ScrollAreaResult {
    pub vertical_dragging: bool,
    pub horizontal_dragging: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DropdownResult {
    pub changed: bool,
    pub selected_index: usize,
}

// -- State objects -----------------------------------------------------------------------------

macro_rules! state_object {
    ($(#[$doc:meta])* $name:ident, $inner:ident, $new:ident) => {
        $(#[$doc])*
        pub struct $name(UniquePtr<ffi::$inner>);

        impl $name {
            pub fn new() -> Self {
                Self(ffi::$new())
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::new()
            }
        }
    };
}

state_object!(
    /// Hover/press colour animation state of one button.
    ButtonState, ButtonStateBox, button_state_new
);
state_object!(
    /// Animation state of one checkbox / radio button / switch.
    ToggleState, ToggleStateBox, toggle_state_new
);
state_object!(
    /// Drag/focus state of one slider.
    SliderState, SliderStateBox, slider_state_new
);
state_object!(
    /// Scrollbar fade/drag state of one scroll area (also used by text areas).
    ScrollAreaState, ScrollAreaStateBox, scroll_area_state_new
);
state_object!(
    /// Open/closed and trigger animation state of one dropdown.
    DropdownState, DropdownStateBox, dropdown_state_new
);
state_object!(
    /// The text, caret, selection and IME composition of one text input or text area. Positions
    /// are Unicode scalar indices, not byte offsets.
    TextEditState, TextEditStateBox, text_edit_state_new
);

impl SliderState {
    pub fn dragging(&self) -> bool {
        ffi::slider_state_dragging(&self.0)
    }
}

impl DropdownState {
    pub fn is_open(&self) -> bool {
        ffi::dropdown_state_is_open(&self.0)
    }

    pub fn set_open(&mut self, open: bool) {
        ffi::dropdown_state_set_open(self.0.pin_mut(), open);
    }
}

impl TextEditState {
    /// Creates a state already holding `text`.
    pub fn with_text(text: &str) -> Self {
        let mut state = Self::new();
        state.set_text(text);
        state
    }

    pub fn text(&self) -> String {
        ffi::text_edit_text(&self.0)
    }

    /// Replaces the text (caret and selection are clamped by the engine).
    pub fn set_text(&mut self, text: &str) {
        ffi::text_edit_set_text(self.0.pin_mut(), text);
    }

    pub fn caret(&self) -> usize {
        ffi::text_edit_caret(&self.0)
    }

    pub fn has_selection(&self) -> bool {
        ffi::text_edit_has_selection(&self.0)
    }

    /// The selection as `(start, end)` scalar indices.
    pub fn selection(&self) -> (usize, usize) {
        let [start, end] = ffi::text_edit_selection(&self.0);
        (start, end)
    }

    pub fn selected_text(&self) -> String {
        ffi::text_edit_selected_text(&self.0)
    }

    pub fn focused(&self) -> bool {
        ffi::text_edit_focused(&self.0)
    }

    pub fn set_focused(&mut self, focused: bool) {
        ffi::text_edit_set_focused(self.0.pin_mut(), focused);
    }

    /// Inserts at the caret, replacing any selection.
    pub fn insert(&mut self, text: &str) {
        ffi::text_edit_insert(self.0.pin_mut(), text);
    }

    pub fn delete_selection(&mut self) {
        ffi::text_edit_delete_selection(self.0.pin_mut());
    }

    pub fn select_all(&mut self) {
        ffi::text_edit_select_all(self.0.pin_mut());
    }

    pub fn select_range(&mut self, start: usize, end: usize) {
        ffi::text_edit_select_range(self.0.pin_mut(), start, end);
    }

    /// Moves the caret; `extend` grows the selection instead of collapsing it.
    pub fn set_caret(&mut self, scalar_index: usize, extend: bool) {
        ffi::text_edit_set_caret(self.0.pin_mut(), scalar_index, extend);
    }
}

fn cursor_from_u8(value: u8) -> crate::ui::Cursor {
    crate::ui::Cursor { repr: value }
}

/// Layout of [`Ui::masonry`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MasonryConfig {
    /// Flow left-to-right in rows of tracks instead of top-to-bottom in columns.
    pub horizontal: bool,
    pub track_count: u32,
    pub track_gap: u16,
    pub item_gap: u16,
}

impl Default for MasonryConfig {
    fn default() -> Self {
        Self { horizontal: false, track_count: 2, track_gap: 10, item_gap: 10 }
    }
}

/// Scroll behaviour of the UI context.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ScrollSettings {
    pub click_and_drag_scroll: bool,
    pub smooth_scrolling: bool,
    pub smoothing_rate: f32,
    pub wheel_multiplier: f32,
}

// -- Colour picker and easing ----------------------------------------------------------------

state_object!(
    /// Hue/saturation/value, colour space and drag state of one colour picker.
    ColorPickerState, ColorPickerStateBox, color_picker_state_new
);

/// Colour spaces a picker can present its component sliders in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
pub enum ColorPickerSpace {
    #[default]
    Srgb = 0,
    Linear,
    Xyz,
    AdobeRgb,
    DisplayP3,
    Rec2020,
    Hsl,
    Hsv,
    Hwb,
    Lab,
    Lch,
    Luv,
    Oklab,
    Oklch,
}

impl ColorPickerSpace {
    const ALL: [Self; 14] = [
        Self::Srgb, Self::Linear, Self::Xyz, Self::AdobeRgb, Self::DisplayP3, Self::Rec2020, Self::Hsl,
        Self::Hsv, Self::Hwb, Self::Lab, Self::Lch, Self::Luv, Self::Oklab, Self::Oklch,
    ];

    fn from_u8(value: u8) -> Self {
        Self::ALL.get(value as usize).copied().unwrap_or_default()
    }

    /// The engine's display name for the space.
    pub fn name(self) -> String {
        ffi::color_space_name(self as u8)
    }
}

impl ColorPickerState {
    pub fn color_space(&self) -> ColorPickerSpace {
        ColorPickerSpace::from_u8(ffi::color_picker_state_color_space(&self.0))
    }

    pub fn set_color_space(&mut self, space: ColorPickerSpace) {
        ffi::color_picker_state_set_color_space(self.0.pin_mut(), space as u8);
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorPickerConfig {
    pub show_color_space_dropdown: bool,
    pub show_alpha: bool,
    pub show_preview: bool,
    pub keyboard_step: f64,
    pub page_step: f64,
}

impl Default for ColorPickerConfig {
    fn default() -> Self {
        Self { show_color_space_dropdown: true, show_alpha: true, show_preview: true, keyboard_step: 0.01, page_step: 0.1 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorPickerStyle {
    pub plane_size: glam::Vec2,
    pub bar_height: f32,
    pub preview_height: f32,
    pub color_space_dropdown_height: f32,
    pub gap: u16,
}

impl Default for ColorPickerStyle {
    fn default() -> Self {
        Self {
            plane_size: glam::Vec2::new(220.0, 160.0),
            bar_height: 18.0,
            preview_height: 28.0,
            color_space_dropdown_height: 32.0,
            gap: 10,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct ColorPickerResult {
    /// The colour after this frame's interaction, straight-alpha sRGB RGBA.
    pub color: Vec4,
    pub changed: bool,
    pub committed: bool,
    pub cancelled: bool,
    pub hovered: bool,
    pub color_space_changed: bool,
    pub color_space: ColorPickerSpace,
}

/// Keyboard actions for a colour picker's focused control.
pub use ffi::SliderKeyKind as ColorPickerKey;

/// The engine's easing curves (`SFT::UI::Easing`), for animating anything you like.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Easing {
    Linear = 0,
    QuadIn, QuadOut, QuadInOut,
    CubicIn, CubicOut, CubicInOut,
    QuartIn, QuartOut, QuartInOut,
    QuintIn, QuintOut, QuintInOut,
    SineIn, SineOut, SineInOut,
    ExpoIn, ExpoOut, ExpoInOut,
    CircIn, CircOut, CircInOut,
    BackIn, BackOut, BackInOut,
    ElasticIn, ElasticOut, ElasticInOut,
    BounceIn, BounceOut, BounceInOut,
}

impl Easing {
    /// Evaluates the curve at `t` (0 to 1).
    pub fn apply(self, t: f32) -> f32 {
        ffi::ease(self as u8, t)
    }
}

// -- The widgets -------------------------------------------------------------------------------

impl Ui<'_> {
    /// A button. The returned [`ElementScope`] is the button's open content element: put its label
    /// (text, an image, ...) inside and drop it to close the button. `None` scope means no UI frame
    /// is open.
    pub fn button(
        &mut self,
        desc: &ElementDesc,
        state: &mut ButtonState,
        style: &ButtonStyle,
        enabled: bool,
    ) -> (ToggleResult, Option<ElementScope>) {
        let out = ffi::ui_button(self.view_shared(), &desc.to_ffi(), &style.to_ffi(), state.0.pin_mut(), enabled);
        (out.into(), out.ok.then(Ui::open_scope))
    }

    /// A checkbox showing `checked`; flip your own bool when [`ToggleResult::clicked`]. `font_id`
    /// is the font used for the check mark.
    pub fn checkbox(
        &mut self,
        desc: &ElementDesc,
        state: &mut ToggleState,
        style: &ToggleStyle,
        checked: bool,
        font_id: u16,
        enabled: bool,
    ) -> ToggleResult {
        ffi::ui_checkbox(self.view_shared(), &desc.to_ffi(), &style.to_ffi(), state.0.pin_mut(), checked, font_id, enabled)
            .into()
    }

    /// A radio button showing `selected`.
    pub fn radio_button(
        &mut self,
        desc: &ElementDesc,
        state: &mut ToggleState,
        style: &ToggleStyle,
        selected: bool,
        enabled: bool,
    ) -> ToggleResult {
        ffi::ui_radio_button(self.view_shared(), &desc.to_ffi(), &style.to_ffi(), state.0.pin_mut(), selected, enabled)
            .into()
    }

    /// An on/off switch showing `on`.
    pub fn switch(
        &mut self,
        desc: &ElementDesc,
        state: &mut ToggleState,
        style: &ToggleStyle,
        on: bool,
        enabled: bool,
    ) -> ToggleResult {
        ffi::ui_switch_toggle(self.view_shared(), &desc.to_ffi(), &style.to_ffi(), state.0.pin_mut(), on, enabled).into()
    }

    /// A slider showing `value`; store [`SliderResult::value`] as the new value each frame.
    #[allow(clippy::too_many_arguments)]
    pub fn slider(
        &mut self,
        desc: &ElementDesc,
        config: &SliderConfig,
        style: &SliderStyle,
        state: &mut SliderState,
        value: f64,
        input: &SliderInput,
        enabled: bool,
    ) -> SliderResult {
        let config = ffi::SliderConfigDesc {
            min: config.min,
            max: config.max,
            has_step: config.step.is_some(),
            step: config.step.unwrap_or(0.0),
            has_keyboard_step: config.keyboard_step.is_some(),
            keyboard_step: config.keyboard_step.unwrap_or(0.0),
            has_page_step: config.page_step.is_some(),
            page_step: config.page_step.unwrap_or(0.0),
            vertical: config.vertical,
            reversed: config.reversed,
            show_step_ticks: config.show_step_ticks,
            ticks: config.ticks.clone(),
            keys: input.keys.clone(),
            request_focus: input.request_focus,
            request_blur: input.request_blur,
        };
        let style = ffi::SliderStyleDesc {
            track: style.track.into(),
            fill: style.fill.into(),
            thumb: style.thumb.into(),
            thumb_hovered: style.thumb_hovered.into(),
            thumb_dragging: style.thumb_dragging.into(),
            tick: style.tick.into(),
            track_thickness: style.track_thickness,
            thumb_size: style.thumb_size,
        };
        let o = ffi::ui_slider(self.view_shared(), &desc.to_ffi(), &config, &style, state.0.pin_mut(), value, enabled);
        SliderResult {
            value: if o.ok { o.value } else { value },
            changed: o.changed,
            committed: o.committed,
            adjusted: o.adjusted,
            cancelled: o.cancelled,
            hovered: o.hovered,
            dragging: o.dragging,
            focused: o.focused,
        }
    }

    /// A single-line text field (see also [`Ui::text_input`], which reads this tick's raw typed text). Typed text, editing keys, IME composition and clipboard come from
    /// the engine's UI text-input state (see [`Ui::type_text`] to inject input).
    pub fn text_field(
        &mut self,
        desc: &ElementDesc,
        style: &TextInputStyle,
        state: &mut TextEditState,
        placeholder: &str,
        enabled: bool,
    ) -> TextInputResult {
        let o = ffi::ui_text_input(
            self.view_pin(),
            &desc.to_ffi(),
            &style.to_ffi(),
            state.0.pin_mut(),
            placeholder,
            enabled,
        );
        TextInputResult { changed: o.changed, submitted: o.submitted, focused: o.focused }
    }

    /// A multi-line text area with its own scrollbars.
    pub fn text_area(
        &mut self,
        desc: &ElementDesc,
        style: &TextInputStyle,
        scrollbar: &ScrollbarStyle,
        state: &mut TextEditState,
        scroll_state: &mut ScrollAreaState,
        placeholder: &str,
        enabled: bool,
    ) -> TextInputResult {
        let o = ffi::ui_text_area(
            self.view_pin(),
            &desc.to_ffi(),
            &style.to_ffi(),
            &scrollbar.to_ffi(),
            state.0.pin_mut(),
            scroll_state.0.pin_mut(),
            placeholder,
            enabled,
        );
        TextInputResult { changed: o.changed, submitted: false, focused: o.focused }
    }

    /// A scrollable region: `content` builds the scrolled elements through the [`Ui`] it is given.
    /// `id` names the scroll container (usable with [`Ui::scroll_metrics`]/[`Ui::set_scroll_offset`]).
    pub fn scroll_area(
        &mut self,
        id: &str,
        desc: &ElementDesc,
        style: &ScrollbarStyle,
        state: &mut ScrollAreaState,
        enabled: bool,
        content: impl FnOnce(&mut Ui<'_>) + 'static,
    ) -> ScrollAreaResult {
        let content = ScrollContent(Some(Box::new(move |view: Pin<&mut EngineView>| {
            let mut ui = Ui::new(view);
            content(&mut ui);
        })));
        let o = ffi::ui_scroll_area(
            self.view_pin(),
            id,
            &desc.to_ffi(),
            &style.to_ffi(),
            state.0.pin_mut(),
            enabled,
            Box::new(content),
        );
        ScrollAreaResult { vertical_dragging: o.vertical_dragging, horizontal_dragging: o.horizontal_dragging }
    }

    /// A colour picker (saturation/value plane, hue bar, optional alpha bar, preview and colour
    /// space dropdown); store [`ColorPickerResult::color`] as the new colour each frame.
    /// `requested_space` switches the picker's component space this frame.
    #[allow(clippy::too_many_arguments)]
    pub fn color_picker(
        &mut self,
        id: &str,
        desc: &ElementDesc,
        config: &ColorPickerConfig,
        style: &ColorPickerStyle,
        state: &mut ColorPickerState,
        color: Vec4,
        requested_space: Option<ColorPickerSpace>,
        enabled: bool,
    ) -> ColorPickerResult {
        let config = ffi::ColorPickerConfigDesc {
            show_color_space_dropdown: config.show_color_space_dropdown,
            show_alpha: config.show_alpha,
            show_preview: config.show_preview,
            keyboard_step: config.keyboard_step,
            page_step: config.page_step,
        };
        let style = ffi::ColorPickerStyleDesc {
            plane_size: style.plane_size.into(),
            bar_height: style.bar_height,
            preview_height: style.preview_height,
            color_space_dropdown_height: style.color_space_dropdown_height,
            gap: style.gap,
        };
        let o = ffi::ui_color_picker(
            self.view_shared(),
            id,
            &desc.to_ffi(),
            &config,
            &style,
            state.0.pin_mut(),
            color.into(),
            &[],
            requested_space.map_or(-1, |s| s as i32),
            enabled,
        );
        ColorPickerResult {
            color: if o.ok { Vec4::from(o.color) } else { color },
            changed: o.changed,
            committed: o.committed,
            cancelled: o.cancelled,
            hovered: o.hovered,
            color_space_changed: o.color_space_changed,
            color_space: ColorPickerSpace::from_u8(o.color_space),
        }
    }

    /// A masonry (waterfall) layout: `items` are `(extent, build)` pairs where `extent` is the
    /// item's size along the flow axis and `build` draws it; items are placed in the shortest track.
    pub fn masonry(
        &mut self,
        desc: &ElementDesc,
        config: &MasonryConfig,
        items: Vec<(f32, Box<dyn FnMut(&mut Ui<'_>)>)>,
    ) -> bool {
        let extents: Vec<f32> = items.iter().map(|(e, _)| *e).collect();
        let builders: Vec<Box<dyn FnMut(Pin<&mut EngineView>)>> = items
            .into_iter()
            .map(|(_, mut build)| {
                Box::new(move |view: Pin<&mut EngineView>| {
                    let mut ui = Ui::new(view);
                    build(&mut ui);
                }) as Box<dyn FnMut(Pin<&mut EngineView>)>
            })
            .collect();
        let config = ffi::MasonryConfigDesc {
            horizontal: config.horizontal,
            track_count: config.track_count,
            track_gap: config.track_gap,
            item_gap: config.item_gap,
        };
        ffi::ui_masonry(self.view_pin(), &desc.to_ffi(), &config, &extents, Box::new(MasonryContent(builders)))
    }

    /// The UI context's scroll behaviour.
    pub fn scroll_settings(&self) -> ScrollSettings {
        let s = ffi::ui_scroll_settings(self.view_shared());
        ScrollSettings {
            click_and_drag_scroll: s.click_and_drag_scroll,
            smooth_scrolling: s.smooth_scrolling,
            smoothing_rate: s.smoothing_rate,
            wheel_multiplier: s.wheel_multiplier,
        }
    }

    pub fn set_scroll_settings(&mut self, settings: &ScrollSettings) {
        ffi::ui_set_scroll_settings(
            self.view_pin(),
            &ffi::ScrollSettingsDesc {
                click_and_drag_scroll: settings.click_and_drag_scroll,
                smooth_scrolling: settings.smooth_scrolling,
                smoothing_rate: settings.smoothing_rate,
                wheel_multiplier: settings.wheel_multiplier,
            },
        );
    }

    /// The cursor icon the UI wants for the current pointer position.
    pub fn desired_cursor(&self) -> crate::ui::Cursor {
        cursor_from_u8(ffi::ui_desired_cursor(self.view_shared()))
    }

    /// Forces a cursor icon regardless of what is hovered.
    pub fn force_cursor(&mut self, icon: crate::ui::Cursor) {
        ffi::ui_force_cursor(self.view_pin(), icon.repr as u8);
    }

    pub fn cursor_management_enabled(&self) -> bool {
        ffi::ui_cursor_management_enabled(self.view_shared())
    }

    /// Turn off to stop the UI choosing cursor icons (the application manages the cursor itself).
    pub fn set_cursor_management_enabled(&mut self, enabled: bool) {
        ffi::ui_set_cursor_management_enabled(self.view_pin(), enabled);
    }

    /// A dropdown of text `options`; `trigger` describes the always-visible button (its label is
    /// the selected option's text — put it inside, or leave the engine default). Returns the newly
    /// chosen index when [`DropdownResult::changed`].
    #[allow(clippy::too_many_arguments)]
    pub fn dropdown(
        &mut self,
        id: &str,
        trigger: &ElementDesc,
        style: &DropdownStyle,
        state: &mut DropdownState,
        selected_index: usize,
        options: &[&str],
        font_id: u16,
        font_size: u16,
    ) -> DropdownResult {
        let style = ffi::DropdownStyleDesc {
            trigger: style.trigger.to_ffi(),
            list_background: style.list_background.into(),
            option_hovered: style.option_hovered.into(),
            corner_radius: style.corner_radius,
            option_padding: style.option_padding,
            show_arrow_indicator: style.show_arrow_indicator,
            arrow_color: style.arrow_color.into(),
            arrow_font_id: style.arrow_font_id,
        };
        let options: Vec<String> = options.iter().map(|s| (*s).to_owned()).collect();
        let o = ffi::ui_dropdown(
            self.view_shared(),
            id,
            &trigger.to_ffi(),
            &style,
            state.0.pin_mut(),
            selected_index,
            &options,
            font_id,
            font_size,
        );
        DropdownResult { changed: o.changed, selected_index: if o.ok { o.selected_index } else { selected_index } }
    }
}
