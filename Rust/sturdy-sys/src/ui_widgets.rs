//! Raw `cxx` bindings for the engine's stateful UI widgets (`Renderer/UI/{Button,Toggle,Slider,
//! TextInput,ScrollArea,Dropdown}.hpp`), layered on the element/text primitives in [`crate::ui`].
//!
//! Every widget call acts on the UI frame currently open inside the draw hook (same rules as
//! `crate::ui`'s element functions) and identifies itself by the id in its [`UiElementDesc`]. The
//! per-widget state the engine keeps between frames (hover/press animation, slider drag, text
//! caret and selection, scrollbar fade, dropdown open/closed) lives in opaque C++ objects owned by
//! Rust through `UniquePtr`; keep one per widget instance across frames.
//!
//! Styles cross as plain colour/number structs holding the engine's own defaults; fields the engine
//! has but this layer omits (composition slots, custom easing functions, highlighters) keep their
//! defaults.

#[cxx::bridge(namespace = "sturdy_rs::ui_widgets")]
pub mod ffi {
    /// `SFT::UI::ButtonStyle`.
    #[derive(Debug, Clone, Copy)]
    struct ButtonStyleDesc {
        idle: [f32; 4],
        hovered: [f32; 4],
        pressed: [f32; 4],
        disabled: [f32; 4],
        corner_radius: f32,
        transition_seconds: f32,
    }

    /// `SFT::UI::ToggleStyle`.
    #[derive(Debug, Clone, Copy)]
    struct ToggleStyleDesc {
        idle: [f32; 4],
        hovered: [f32; 4],
        checked: [f32; 4],
        disabled: [f32; 4],
        mark_color: [f32; 4],
        transition_seconds: f32,
    }

    /// Result of a button / checkbox / radio / switch. `ok == false` when there is no open UI
    /// frame or the engine threw.
    #[derive(Debug, Clone, Copy, Default)]
    struct ToggleOutcome {
        ok: bool,
        hovered: bool,
        pressed: bool,
        clicked: bool,
    }

    /// `SFT::UI::SliderKey`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum SliderKeyKind {
        Decrement,
        Increment,
        PageDecrement,
        PageIncrement,
        Minimum,
        Maximum,
    }

    /// `SFT::UI::SliderConfig` (+ `SliderInput`). The `has_*` flags stand for the engine's
    /// `optional<f64>`s.
    #[derive(Debug, Clone)]
    struct SliderConfigDesc {
        min: f64,
        max: f64,
        has_step: bool,
        step: f64,
        has_keyboard_step: bool,
        keyboard_step: f64,
        has_page_step: bool,
        page_step: f64,
        vertical: bool,
        reversed: bool,
        show_step_ticks: bool,
        ticks: Vec<f64>,
        /// Keyboard input to apply this frame, in order.
        keys: Vec<SliderKeyKind>,
        request_focus: bool,
        request_blur: bool,
    }

    /// `SFT::UI::SliderStyle`.
    #[derive(Debug, Clone, Copy)]
    struct SliderStyleDesc {
        track: [f32; 4],
        fill: [f32; 4],
        thumb: [f32; 4],
        thumb_hovered: [f32; 4],
        thumb_dragging: [f32; 4],
        tick: [f32; 4],
        track_thickness: f32,
        thumb_size: f32,
    }

    /// `SFT::UI::SliderResult`.
    #[derive(Debug, Clone, Copy, Default)]
    struct SliderOutcome {
        ok: bool,
        value: f64,
        changed: bool,
        committed: bool,
        adjusted: bool,
        cancelled: bool,
        hovered: bool,
        dragging: bool,
        focused: bool,
    }

    /// `SFT::UI::TextEditStyle` (the commonly tuned part).
    #[derive(Debug, Clone)]
    struct TextEditStyleDesc {
        idle: [f32; 4],
        hovered: [f32; 4],
        focused: [f32; 4],
        disabled: [f32; 4],
        border_idle: [f32; 4],
        border_focused: [f32; 4],
        text_color: [f32; 4],
        placeholder_color: [f32; 4],
        selection_color: [f32; 4],
        caret_color: [f32; 4],
        corner_radius: f32,
        font_id: u16,
        font_size: u16,
        caret_blink_seconds: f32,
        mask_characters: bool,
        wrap_lines: bool,
    }

    /// `SFT::UI::TextInputResult` / `TextAreaResult`.
    #[derive(Debug, Clone, Copy, Default)]
    struct TextEditOutcome {
        ok: bool,
        changed: bool,
        submitted: bool,
        focused: bool,
    }

    /// `SFT::UI::ScrollbarStyle`.
    #[derive(Debug, Clone, Copy)]
    struct ScrollbarStyleDesc {
        idle_thickness: f32,
        hovered_thickness: f32,
        track_color: [f32; 4],
        thumb_color: [f32; 4],
        thumb_hovered_color: [f32; 4],
        thumb_dragging_color: [f32; 4],
        corner_radius: f32,
        margin: f32,
        min_thumb_length: f32,
        /// 0 = when needed, 1 = always visible, 2 = always hidden.
        visibility: u8,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct ScrollAreaOutcome {
        ok: bool,
        vertical_dragging: bool,
        horizontal_dragging: bool,
    }

    /// `SFT::UI::DropdownStyle` (the commonly tuned part).
    #[derive(Debug, Clone, Copy)]
    struct DropdownStyleDesc {
        trigger: ButtonStyleDesc,
        list_background: [f32; 4],
        option_hovered: [f32; 4],
        corner_radius: f32,
        option_padding: u16,
        show_arrow_indicator: bool,
        arrow_color: [f32; 4],
        arrow_font_id: u16,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct DropdownOutcome {
        ok: bool,
        changed: bool,
        selected_index: usize,
    }

    /// `SFT::UI::ColorPickerConfig`.
    #[derive(Debug, Clone, Copy)]
    struct ColorPickerConfigDesc {
        show_color_space_dropdown: bool,
        show_alpha: bool,
        show_preview: bool,
        keyboard_step: f64,
        page_step: f64,
    }

    /// The commonly tuned part of `SFT::UI::ColorPickerStyle`.
    #[derive(Debug, Clone, Copy)]
    struct ColorPickerStyleDesc {
        plane_size: [f32; 2],
        bar_height: f32,
        preview_height: f32,
        color_space_dropdown_height: f32,
        gap: u16,
    }

    /// `SFT::UI::ColorPickerResult`. `color` is straight-alpha sRGB RGBA; `color_space`,
    /// `active_part` and `focused_part` are the numeric values of the engine's enums.
    #[derive(Debug, Clone, Copy, Default)]
    struct ColorPickerOutcome {
        ok: bool,
        color: [f32; 4],
        changed: bool,
        committed: bool,
        cancelled: bool,
        hovered: bool,
        color_space_changed: bool,
        color_space: u8,
        active_part: u8,
        focused_part: u8,
    }

    /// `SFT::UI::ScrollSettings`.
    #[derive(Debug, Clone, Copy)]
    struct ScrollSettingsDesc {
        click_and_drag_scroll: bool,
        smooth_scrolling: bool,
        smoothing_rate: f32,
        wheel_multiplier: f32,
    }

    /// `SFT::UI::MasonryConfig`.
    #[derive(Debug, Clone, Copy)]
    struct MasonryConfigDesc {
        horizontal: bool,
        track_count: u32,
        track_gap: u16,
        item_gap: u16,
    }

    // Rust closure that builds a scroll area's content; see `scroll_content_run`.
    extern "Rust" {
        type ScrollContent;

        /// Called by `ui_scroll_area` while the scroll area's content element is open. `engine` is
        /// the same engine view the widget call was made with.
        fn scroll_content_run(content: &mut ScrollContent, engine: Pin<&mut EngineView>);

        type MasonryContent;

        /// Called by `ui_masonry` once per item, with the item's index, while its slot is open.
        fn masonry_item_run(content: &mut MasonryContent, index: usize, engine: Pin<&mut EngineView>);
    }

    unsafe extern "C++" {
        include!("sturdy_rs/ui_widgets.hpp");

        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;
        #[namespace = "sturdy_rs::ui"]
        type UiElementDesc = crate::ui::ffi::UiElementDesc;

        /// Per-widget state objects (`ButtonState`, `ToggleState`, `SliderState`,
        /// `TextEditState`, `ScrollAreaState`, `DropdownState`).
        type ButtonStateBox;
        type ToggleStateBox;
        type SliderStateBox;
        type TextEditStateBox;
        type ScrollAreaStateBox;
        type DropdownStateBox;
        type ColorPickerStateBox;

        fn button_state_new() -> UniquePtr<ButtonStateBox>;
        fn toggle_state_new() -> UniquePtr<ToggleStateBox>;
        fn slider_state_new() -> UniquePtr<SliderStateBox>;
        fn text_edit_state_new() -> UniquePtr<TextEditStateBox>;
        fn scroll_area_state_new() -> UniquePtr<ScrollAreaStateBox>;
        fn dropdown_state_new() -> UniquePtr<DropdownStateBox>;
        fn color_picker_state_new() -> UniquePtr<ColorPickerStateBox>;
        /// `SFT::UI::ColorPickerColorSpace` as its numeric value (0 = sRGB .. 13 = OKLCH).
        fn color_picker_state_color_space(state: &ColorPickerStateBox) -> u8;
        fn color_picker_state_set_color_space(state: Pin<&mut ColorPickerStateBox>, color_space: u8);
        /// Name of a colour space value, e.g. "sRGB".
        fn color_space_name(color_space: u8) -> String;
        /// Evaluates the engine's easing curve number `kind` (see `sturdy::widgets::Easing`) at `t`.
        fn ease(kind: u8, t: f32) -> f32;
        /// Lays `extents.len()` items out in balanced tracks (a masonry / waterfall layout);
        /// `extents[i]` is item i's size along the flow axis, and its content is built by the
        /// callback.
        fn ui_masonry(
            engine: Pin<&mut EngineView>,
            desc: &UiElementDesc,
            config: &MasonryConfigDesc,
            extents: &[f32],
            content: Box<MasonryContent>,
        ) -> bool;
        /// Scroll behaviour of the whole UI context (wheel speed, smoothing, drag-to-scroll).
        fn ui_scroll_settings(engine: &EngineView) -> ScrollSettingsDesc;
        fn ui_set_scroll_settings(engine: Pin<&mut EngineView>, settings: &ScrollSettingsDesc);
        /// The cursor icon the UI wants this frame, as the numeric value of `ui::Cursor`.
        fn ui_desired_cursor(engine: &EngineView) -> u8;
        /// Overrides the cursor icon (numeric `ui::Cursor`) regardless of hover.
        fn ui_force_cursor(engine: Pin<&mut EngineView>, icon: u8);
        fn ui_cursor_management_enabled(engine: &EngineView) -> bool;
        /// When disabled the UI stops choosing a cursor icon (the application manages it).
        fn ui_set_cursor_management_enabled(engine: Pin<&mut EngineView>, enabled: bool);
        fn ui_color_picker(
            engine: &EngineView,
            id: &str,
            desc: &UiElementDesc,
            config: &ColorPickerConfigDesc,
            style: &ColorPickerStyleDesc,
            state: Pin<&mut ColorPickerStateBox>,
            color: [f32; 4],
            keys: &[u8],
            requested_color_space: i32,
            enabled: bool,
        ) -> ColorPickerOutcome;

        fn slider_state_dragging(state: &SliderStateBox) -> bool;
        fn dropdown_state_is_open(state: &DropdownStateBox) -> bool;
        fn dropdown_state_set_open(state: Pin<&mut DropdownStateBox>, open: bool);

        // -- Text edit state accessors (scalar indices are Unicode scalars, not bytes) ----------
        fn text_edit_text(state: &TextEditStateBox) -> String;
        fn text_edit_set_text(state: Pin<&mut TextEditStateBox>, text: &str);
        fn text_edit_caret(state: &TextEditStateBox) -> usize;
        fn text_edit_has_selection(state: &TextEditStateBox) -> bool;
        fn text_edit_selection(state: &TextEditStateBox) -> [usize; 2];
        fn text_edit_selected_text(state: &TextEditStateBox) -> String;
        fn text_edit_focused(state: &TextEditStateBox) -> bool;
        fn text_edit_set_focused(state: Pin<&mut TextEditStateBox>, focused: bool);
        fn text_edit_insert(state: Pin<&mut TextEditStateBox>, text: &str);
        fn text_edit_delete_selection(state: Pin<&mut TextEditStateBox>);
        fn text_edit_select_all(state: Pin<&mut TextEditStateBox>);
        fn text_edit_select_range(state: Pin<&mut TextEditStateBox>, start: usize, end: usize);
        fn text_edit_set_caret(state: Pin<&mut TextEditStateBox>, scalar_index: usize, extend: bool);

        // -- Widgets ------------------------------------------------------------------------------

        /// Opens a button element and **leaves it open** (its children go inside); close it with
        /// `ui_end_element`. Not opened (and `ok == false`) when there is no open UI frame.
        fn ui_button(
            engine: &EngineView,
            desc: &UiElementDesc,
            style: &ButtonStyleDesc,
            state: Pin<&mut ButtonStateBox>,
            enabled: bool,
        ) -> ToggleOutcome;
        fn ui_checkbox(
            engine: &EngineView,
            desc: &UiElementDesc,
            style: &ToggleStyleDesc,
            state: Pin<&mut ToggleStateBox>,
            checked: bool,
            font_id: u16,
            enabled: bool,
        ) -> ToggleOutcome;
        fn ui_radio_button(
            engine: &EngineView,
            desc: &UiElementDesc,
            style: &ToggleStyleDesc,
            state: Pin<&mut ToggleStateBox>,
            selected: bool,
            enabled: bool,
        ) -> ToggleOutcome;
        fn ui_switch_toggle(
            engine: &EngineView,
            desc: &UiElementDesc,
            style: &ToggleStyleDesc,
            state: Pin<&mut ToggleStateBox>,
            on: bool,
            enabled: bool,
        ) -> ToggleOutcome;
        fn ui_slider(
            engine: &EngineView,
            desc: &UiElementDesc,
            config: &SliderConfigDesc,
            style: &SliderStyleDesc,
            state: Pin<&mut SliderStateBox>,
            value: f64,
            enabled: bool,
        ) -> SliderOutcome;
        /// Single-line text input; text/caret/selection live in `state`. Typed text and edit keys
        /// come from the engine's per-tick UI text-input state.
        fn ui_text_input(
            engine: Pin<&mut EngineView>,
            desc: &UiElementDesc,
            style: &TextEditStyleDesc,
            state: Pin<&mut TextEditStateBox>,
            placeholder: &str,
            enabled: bool,
        ) -> TextEditOutcome;
        /// Multi-line text area (a text input with vertical scrolling).
        fn ui_text_area(
            engine: Pin<&mut EngineView>,
            desc: &UiElementDesc,
            style: &TextEditStyleDesc,
            scrollbar: &ScrollbarStyleDesc,
            state: Pin<&mut TextEditStateBox>,
            scroll_state: Pin<&mut ScrollAreaStateBox>,
            placeholder: &str,
            enabled: bool,
        ) -> TextEditOutcome;
        fn ui_scroll_area(
            engine: Pin<&mut EngineView>,
            id: &str,
            desc: &UiElementDesc,
            style: &ScrollbarStyleDesc,
            state: Pin<&mut ScrollAreaStateBox>,
            enabled: bool,
            content: Box<ScrollContent>,
        ) -> ScrollAreaOutcome;
        /// Dropdown whose options are plain text labels.
        fn ui_dropdown(
            engine: &EngineView,
            id: &str,
            trigger: &UiElementDesc,
            style: &DropdownStyleDesc,
            state: Pin<&mut DropdownStateBox>,
            selected_index: usize,
            options: &Vec<String>,
            font_id: u16,
            font_size: u16,
        ) -> DropdownOutcome;
    }
}

use core::pin::Pin;

use crate::diagnostics::guard;
pub use ffi::EngineView;

/// The per-item builders of a masonry layout, consumed by `ui_masonry`.
pub struct MasonryContent(pub Vec<Box<dyn FnMut(Pin<&mut EngineView>)>>);

fn masonry_item_run(content: &mut MasonryContent, index: usize, engine: Pin<&mut EngineView>) {
    if let Some(build) = content.0.get_mut(index) {
        guard("ui masonry item", move || build(engine));
    }
}

/// Holds the closure that builds a scroll area's content, consumed by `ui_scroll_area`.
pub struct ScrollContent(pub Option<Box<dyn FnOnce(Pin<&mut EngineView>)>>);

fn scroll_content_run(content: &mut ScrollContent, engine: Pin<&mut EngineView>) {
    if let Some(build) = content.0.take() {
        guard("ui scroll content", move || build(engine));
    }
}

// SAFETY: every widget state object is plain owned data (strings, vectors, floats) with no thread
// affinity; each is only touched from the single thread that runs the UI draw hook, and the hook
// closure holding them must be `Send`.
unsafe impl Send for ffi::ButtonStateBox {}
unsafe impl Send for ffi::ToggleStateBox {}
unsafe impl Send for ffi::SliderStateBox {}
unsafe impl Send for ffi::TextEditStateBox {}
unsafe impl Send for ffi::ScrollAreaStateBox {}
unsafe impl Send for ffi::DropdownStateBox {}
unsafe impl Send for ffi::ColorPickerStateBox {}
