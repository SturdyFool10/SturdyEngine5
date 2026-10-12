#include "sturdy_rs/ui_widgets.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <Engine/EcsUi.hpp>
#include <Engine/Engine.hpp>
#include <Renderer/UI/TextArea.hpp>
#include <Renderer/UI/TextInput.hpp>

#include "sturdy_rs/ui.hpp"

// Generated from src/ui.rs (UiElementDesc) and src/ui_widgets.rs.
#include "sturdy_rs/ui_bridge.h"
#include "sturdy_rs/ui_widgets_bridge.h"

namespace sturdy_rs::ui_widgets {
    namespace {

        namespace U = SFT::UI;

        U::Color color_of(const std::array<float, 4> &c) noexcept {
            return U::Color{static_cast<SFT::f64>(c[0]), static_cast<SFT::f64>(c[1]), static_cast<SFT::f64>(c[2]),
                            static_cast<SFT::f64>(c[3])};
        }

        SFT::UString ustring_of(rust::Str text) {
            return SFT::UString{std::string_view{text.data(), text.size()}};
        }

        rust::String rust_of(const SFT::UString &text) {
            const auto view = text.cpp_string_view();
            return rust::String::lossy(view.data(), view.size());
        }

        double delta_seconds(const EngineView &engine) noexcept {
            return engine.frame_time().delta_seconds();
        }

        U::ButtonStyle to_engine(const ButtonStyleDesc &s) {
            U::ButtonStyle out{};
            out.idle = color_of(s.idle);
            out.hovered = color_of(s.hovered);
            out.pressed = color_of(s.pressed);
            out.disabled = color_of(s.disabled);
            out.corner_radius = U::CornerRadius::all(s.corner_radius);
            out.transition_seconds = s.transition_seconds;
            return out;
        }

        U::ToggleStyle to_engine(const ToggleStyleDesc &s) {
            U::ToggleStyle out{};
            out.idle = color_of(s.idle);
            out.hovered = color_of(s.hovered);
            out.checked = color_of(s.checked);
            out.disabled = color_of(s.disabled);
            out.mark_color = color_of(s.mark_color);
            out.transition_seconds = s.transition_seconds;
            return out;
        }

        U::TextEditStyle to_engine(const TextEditStyleDesc &s) {
            U::TextEditStyle out{};
            out.idle = color_of(s.idle);
            out.hovered = color_of(s.hovered);
            out.focused = color_of(s.focused);
            out.disabled = color_of(s.disabled);
            out.border_idle.color = color_of(s.border_idle);
            out.border_focused.color = color_of(s.border_focused);
            out.text_color = color_of(s.text_color);
            out.placeholder_color = color_of(s.placeholder_color);
            out.selection_color = color_of(s.selection_color);
            out.caret_color = color_of(s.caret_color);
            out.corner_radius = U::CornerRadius::all(s.corner_radius);
            out.font_id = static_cast<U::FontId>(s.font_id);
            out.font_size = s.font_size;
            out.caret_blink_seconds = s.caret_blink_seconds;
            out.mask_characters = s.mask_characters;
            out.wrap_lines = s.wrap_lines;
            return out;
        }

        U::ScrollbarStyle to_engine(const ScrollbarStyleDesc &s) {
            U::ScrollbarStyle out{};
            out.idle_thickness = s.idle_thickness;
            out.hovered_thickness = s.hovered_thickness;
            out.track_color = color_of(s.track_color);
            out.thumb_color = color_of(s.thumb_color);
            out.thumb_hovered_color = color_of(s.thumb_hovered_color);
            out.thumb_dragging_color = color_of(s.thumb_dragging_color);
            out.corner_radius = s.corner_radius;
            out.margin = s.margin;
            out.min_thumb_length = s.min_thumb_length;
            out.visibility = s.visibility == 1   ? U::ScrollbarVisibility::AlwaysVisible
                             : s.visibility == 2 ? U::ScrollbarVisibility::AlwaysHidden
                                                 : U::ScrollbarVisibility::WhenNeeded;
            return out;
        }

        ToggleOutcome to_outcome(const U::ToggleResult &r) noexcept {
            return ToggleOutcome{true, r.hovered, r.pressed, r.clicked};
        }

        /// The engine's text-input state for this tick, with clipboard access routed to the
        /// primary window.
        U::TextEditInput frame_input(EngineView &engine) {
            EngineView *view = &engine;
            return ui::screen(engine).input().text_input(
                [view]() -> SFT::UString {
                    if (auto *window = view->primary_window()) {
                        const std::string text = window->clipboard_text();
                        return SFT::UString{std::string_view{text}};
                    }
                    return SFT::UString{};
                },
                [view](const SFT::UString &text) {
                    if (auto *window = view->primary_window()) {
                        (void)window->set_clipboard_text(text.cpp_string_view());
                    }
                });
        }

        /// Tells the window whether a text field has focus, so the platform starts/stops text input
        /// (without this SDL never delivers typed characters or IME composition). Mirrors
        /// `Engine::forward_text_input_state`, which the engine's own C++ callers invoke for the
        /// focused field; `owner` remembers which widget switched it on so a different unfocused
        /// widget doesn't switch it off.
        void forward_text_focus(EngineView &engine, rust::Str id, const UiElementDesc &desc, bool focused,
                                const std::optional<U::ElementBounds> &caret) {
            static std::string owner;
            SFT::WindowManager::Window *window = engine.primary_window();
            if (window == nullptr) {
                return;
            }
            const std::string widget{id.data(), id.size()};
            if (focused) {
                const auto field = ui::ui_session_context() != nullptr
                                       ? ui::ui_session_context()->element_bounds(ustring_of(rust::Str{desc.id.data(), desc.id.size()}))
                                       : std::nullopt;
                SFT::Engine::TextInputFocusInfo info{};
                info.field_bounds = field.value_or(U::ElementBounds{});
                info.caret_bounds = caret.value_or(info.field_bounds);
                info.ime_enabled = true;
                SFT::Engine::forward_text_input_state(engine.window_requests(), window->id(), info);
                owner = widget;
            } else if (owner == widget) {
                SFT::Engine::forward_text_input_state(engine.window_requests(), window->id(), std::nullopt);
                owner.clear();
            }
        }

    } // namespace

    std::unique_ptr<ButtonStateBox> button_state_new() { return std::make_unique<ButtonStateBox>(); }
    std::unique_ptr<ToggleStateBox> toggle_state_new() { return std::make_unique<ToggleStateBox>(); }
    std::unique_ptr<SliderStateBox> slider_state_new() { return std::make_unique<SliderStateBox>(); }
    std::unique_ptr<TextEditStateBox> text_edit_state_new() { return std::make_unique<TextEditStateBox>(); }
    std::unique_ptr<ScrollAreaStateBox> scroll_area_state_new() { return std::make_unique<ScrollAreaStateBox>(); }
    std::unique_ptr<DropdownStateBox> dropdown_state_new() { return std::make_unique<DropdownStateBox>(); }

    std::unique_ptr<ColorPickerStateBox> color_picker_state_new() { return std::make_unique<ColorPickerStateBox>(); }
    std::uint8_t color_picker_state_color_space(const ColorPickerStateBox &s) noexcept {
        return static_cast<std::uint8_t>(s.color_space());
    }
    void color_picker_state_set_color_space(ColorPickerStateBox &s, std::uint8_t space) noexcept {
        s.set_color_space(static_cast<U::ColorPickerColorSpace>(space));
    }
    rust::String color_space_name(std::uint8_t space) {
        if (space >= static_cast<std::uint8_t>(U::ColorPickerColorSpace::Count)) {
            return rust::String();
        }
        return rust::String(U::color_picker_space_name(static_cast<U::ColorPickerColorSpace>(space)));
    }

    float ease(std::uint8_t kind, float t) noexcept {
        using F = float (*)(float);
        static constexpr F table[] = {
            U::Easing::linear,      U::Easing::quad_in,      U::Easing::quad_out,     U::Easing::quad_in_out,
            U::Easing::cubic_in,    U::Easing::cubic_out,    U::Easing::cubic_in_out, U::Easing::quart_in,
            U::Easing::quart_out,   U::Easing::quart_in_out, U::Easing::quint_in,     U::Easing::quint_out,
            U::Easing::quint_in_out, U::Easing::sine_in,     U::Easing::sine_out,     U::Easing::sine_in_out,
            U::Easing::expo_in,     U::Easing::expo_out,     U::Easing::expo_in_out,  U::Easing::circ_in,
            U::Easing::circ_out,    U::Easing::circ_in_out,  U::Easing::back_in,      U::Easing::back_out,
            U::Easing::back_in_out, U::Easing::elastic_in,   U::Easing::elastic_out,  U::Easing::elastic_in_out,
            U::Easing::bounce_in,   U::Easing::bounce_out,   U::Easing::bounce_in_out,
        };
        constexpr std::size_t count = sizeof(table) / sizeof(table[0]);
        return kind < count ? table[kind](t) : t;
    }

    bool ui_masonry(EngineView &engine, const UiElementDesc &desc, const MasonryConfigDesc &config,
                    rust::Slice<const float> extents, rust::Box<MasonryContent> content) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return false;
        }
        try {
            EngineView *view = &engine;
            MasonryContent *raw = &*content;
            std::vector<U::MasonryItem> items;
            for (std::size_t i = 0; i < extents.size(); ++i) {
                items.push_back(U::MasonryItem{extents[i], [view, raw, i](U::Context &) { masonry_item_run(*raw, i, *view); }});
            }
            U::MasonryConfig engine_config{};
            engine_config.direction = config.horizontal ? U::MasonryDirection::Horizontal : U::MasonryDirection::Vertical;
            engine_config.track_count = config.track_count;
            engine_config.track_gap = config.track_gap;
            engine_config.item_gap = config.item_gap;
            U::masonry_layout(*ctx, ui::ui_session_element_decl(desc), engine_config,
                              std::span<const U::MasonryItem>{items});
            return true;
        } catch (...) {
            return false;
        }
    }

    ScrollSettingsDesc ui_scroll_settings(const EngineView &engine) {
        const U::ScrollSettings &s = ui::screen(engine).context().scroll_settings();
        return ScrollSettingsDesc{s.click_and_drag_scroll, s.smooth_scrolling, s.smoothing_rate, s.wheel_multiplier};
    }
    void ui_set_scroll_settings(EngineView &engine, const ScrollSettingsDesc &settings) {
        U::ScrollSettings s{};
        s.click_and_drag_scroll = settings.click_and_drag_scroll;
        s.smooth_scrolling = settings.smooth_scrolling;
        s.smoothing_rate = settings.smoothing_rate;
        s.wheel_multiplier = settings.wheel_multiplier;
        ui::screen(engine).context().set_scroll_settings(s);
    }
    std::uint8_t ui_desired_cursor(const EngineView &engine) {
        return static_cast<std::uint8_t>(ui::screen(engine).context().desired_cursor());
    }
    void ui_force_cursor(EngineView &engine, std::uint8_t icon) {
        ui::screen(engine).context().force_cursor(static_cast<U::CursorIcon>(icon));
    }
    bool ui_cursor_management_enabled(const EngineView &engine) {
        return ui::screen(engine).context().cursor_management_enabled();
    }
    void ui_set_cursor_management_enabled(EngineView &engine, bool enabled) {
        ui::screen(engine).context().set_cursor_management_enabled(enabled);
    }

    ColorPickerOutcome ui_color_picker(const EngineView &engine, rust::Str id, const UiElementDesc &desc,
                                       const ColorPickerConfigDesc &config, const ColorPickerStyleDesc &style,
                                       ColorPickerStateBox &state, std::array<float, 4> color,
                                       rust::Slice<const std::uint8_t> keys, std::int32_t requested_color_space,
                                       bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ColorPickerOutcome{};
        }
        try {
            U::ColorPickerConfig engine_config{};
            engine_config.show_color_space_dropdown = config.show_color_space_dropdown;
            engine_config.show_alpha = config.show_alpha;
            engine_config.show_preview = config.show_preview;
            engine_config.keyboard_step = config.keyboard_step;
            engine_config.page_step = config.page_step;
            U::ColorPickerStyle engine_style{};
            engine_style.plane_size = {style.plane_size[0], style.plane_size[1]};
            engine_style.bar_height = style.bar_height;
            engine_style.preview_height = style.preview_height;
            engine_style.color_space_dropdown_height = style.color_space_dropdown_height;
            engine_style.gap = style.gap;

            std::vector<U::ColorPickerKey> engine_keys;
            for (std::uint8_t key : keys) {
                engine_keys.push_back(static_cast<U::ColorPickerKey>(key));
            }
            U::ColorPickerInput input{};
            input.keys = std::span<const U::ColorPickerKey>{engine_keys};
            if (requested_color_space >= 0) {
                input.requested_color_space = static_cast<U::ColorPickerColorSpace>(requested_color_space);
            }
            input.delta_seconds = static_cast<float>(delta_seconds(engine));

            const U::ColorPickerResult r =
                U::color_picker(*ctx, ustring_of(id), ui::ui_session_element_decl(desc), engine_config, engine_style,
                                state, color_of(color), input, enabled);
            ColorPickerOutcome out{};
            out.ok = true;
            out.color = {static_cast<float>(r.color.r), static_cast<float>(r.color.g),
                         static_cast<float>(r.color.b), static_cast<float>(r.color.a)};
            out.changed = r.changed;
            out.committed = r.committed;
            out.cancelled = r.cancelled;
            out.hovered = r.hovered;
            out.color_space_changed = r.color_space_changed;
            out.color_space = static_cast<std::uint8_t>(r.color_space);
            out.active_part = static_cast<std::uint8_t>(r.active_part);
            out.focused_part = static_cast<std::uint8_t>(r.focused_part);
            return out;
        } catch (...) {
            return ColorPickerOutcome{};
        }
    }

    bool slider_state_dragging(const SliderStateBox &state) noexcept { return state.dragging(); }
    bool dropdown_state_is_open(const DropdownStateBox &state) noexcept { return state.open; }
    void dropdown_state_set_open(DropdownStateBox &state, bool open) noexcept { state.open = open; }

    rust::String text_edit_text(const TextEditStateBox &s) { return rust_of(s.text()); }
    void text_edit_set_text(TextEditStateBox &s, rust::Str text) { s.set_text(ustring_of(text)); }
    std::size_t text_edit_caret(const TextEditStateBox &s) noexcept { return s.caret(); }
    bool text_edit_has_selection(const TextEditStateBox &s) noexcept { return s.has_selection(); }
    std::array<std::size_t, 2> text_edit_selection(const TextEditStateBox &s) noexcept {
        return {s.selection_min(), s.selection_max()};
    }
    rust::String text_edit_selected_text(const TextEditStateBox &s) { return rust_of(s.selected_text()); }
    bool text_edit_focused(const TextEditStateBox &s) noexcept { return s.focused(); }
    void text_edit_set_focused(TextEditStateBox &s, bool focused) noexcept { s.set_focused(focused); }
    void text_edit_insert(TextEditStateBox &s, rust::Str text) { s.insert(ustring_of(text)); }
    void text_edit_delete_selection(TextEditStateBox &s) { s.delete_selection(); }
    void text_edit_select_all(TextEditStateBox &s) noexcept { s.select_all(); }
    void text_edit_select_range(TextEditStateBox &s, std::size_t start, std::size_t end) noexcept {
        s.select_range(start, end);
    }
    void text_edit_set_caret(TextEditStateBox &s, std::size_t index, bool extend) noexcept {
        s.set_caret_to(index, extend);
    }

    ToggleOutcome ui_button(const EngineView &engine, const UiElementDesc &desc, const ButtonStyleDesc &style,
                            ButtonStateBox &state, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ToggleOutcome{};
        }
        try {
            U::ButtonResult result = U::button(*ctx, ui::ui_session_element_decl(desc), to_engine(style), state,
                                               static_cast<float>(delta_seconds(engine)), enabled);
            ToggleOutcome out{true, result.hovered, result.pressed, result.clicked};
            ui::ui_session_push_scope(std::move(result.scope));
            return out;
        } catch (...) {
            return ToggleOutcome{};
        }
    }

    ToggleOutcome ui_checkbox(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                              ToggleStateBox &state, bool checked, std::uint16_t font_id, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ToggleOutcome{};
        }
        try {
            return to_outcome(U::checkbox(*ctx, ui::ui_session_element_decl(desc), to_engine(style), state,
                                          static_cast<float>(delta_seconds(engine)), checked,
                                          static_cast<U::FontId>(font_id), enabled));
        } catch (...) {
            return ToggleOutcome{};
        }
    }

    ToggleOutcome ui_radio_button(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                                  ToggleStateBox &state, bool selected, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ToggleOutcome{};
        }
        try {
            return to_outcome(U::radio_button(*ctx, ui::ui_session_element_decl(desc), to_engine(style), state,
                                              static_cast<float>(delta_seconds(engine)), selected, enabled));
        } catch (...) {
            return ToggleOutcome{};
        }
    }

    ToggleOutcome ui_switch_toggle(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                                   ToggleStateBox &state, bool on, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ToggleOutcome{};
        }
        try {
            return to_outcome(U::switch_toggle(*ctx, ui::ui_session_element_decl(desc), to_engine(style), state,
                                               static_cast<float>(delta_seconds(engine)), on, enabled));
        } catch (...) {
            return ToggleOutcome{};
        }
    }

    SliderOutcome ui_slider(const EngineView &, const UiElementDesc &desc, const SliderConfigDesc &config,
                            const SliderStyleDesc &style, SliderStateBox &state, double value, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return SliderOutcome{};
        }
        try {
            std::vector<U::SliderTick> ticks;
            for (double tick : config.ticks) {
                ticks.push_back(U::SliderTick{tick});
            }
            std::vector<U::SliderKey> keys;
            for (SliderKeyKind key : config.keys) {
                keys.push_back(static_cast<U::SliderKey>(static_cast<std::uint8_t>(key)));
            }
            U::SliderConfig engine_config{};
            engine_config.min = config.min;
            engine_config.max = config.max;
            engine_config.step = config.has_step ? std::optional<double>{config.step} : std::nullopt;
            if (config.has_keyboard_step) engine_config.keyboard_step = config.keyboard_step;
            if (config.has_page_step) engine_config.page_step = config.page_step;
            engine_config.orientation = config.vertical ? U::SliderOrientation::Vertical : U::SliderOrientation::Horizontal;
            engine_config.reversed = config.reversed;
            engine_config.show_step_ticks = config.show_step_ticks;
            engine_config.ticks = std::span<const U::SliderTick>{ticks};

            U::SliderStyle engine_style{};
            engine_style.track = color_of(style.track);
            engine_style.fill = color_of(style.fill);
            engine_style.thumb = color_of(style.thumb);
            engine_style.thumb_hovered = color_of(style.thumb_hovered);
            engine_style.thumb_dragging = color_of(style.thumb_dragging);
            engine_style.tick = color_of(style.tick);
            engine_style.track_thickness = style.track_thickness;
            engine_style.thumb_size = style.thumb_size;

            U::SliderInput input{};
            input.keys = std::span<const U::SliderKey>{keys};
            input.request_focus = config.request_focus;
            input.request_blur = config.request_blur;

            const U::SliderResult r =
                U::slider(*ctx, ui::ui_session_element_decl(desc), engine_config, engine_style, state, value, input, enabled);
            return SliderOutcome{true, r.value, r.changed, r.committed, r.adjusted, r.cancelled,
                                 r.hovered, r.dragging, r.focused};
        } catch (...) {
            return SliderOutcome{};
        }
    }

    TextEditOutcome ui_text_input(EngineView &engine, const UiElementDesc &desc, const TextEditStyleDesc &style,
                                  TextEditStateBox &state, rust::Str placeholder, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return TextEditOutcome{};
        }
        try {
            const U::TextEditInput input = frame_input(engine);
            const U::TextInputResult r =
                U::text_input(*ctx, ui::ui_session_element_decl(desc), to_engine(style), state, input,
                              static_cast<float>(delta_seconds(engine)), ustring_of(placeholder), enabled);
            forward_text_focus(engine, rust::Str{desc.id.data(), desc.id.size()}, desc, r.focused, r.caret_bounds);
            return TextEditOutcome{true, r.changed, r.submitted, r.focused};
        } catch (...) {
            return TextEditOutcome{};
        }
    }

    TextEditOutcome ui_text_area(EngineView &engine, const UiElementDesc &desc, const TextEditStyleDesc &style,
                                 const ScrollbarStyleDesc &scrollbar, TextEditStateBox &state,
                                 ScrollAreaStateBox &scroll_state, rust::Str placeholder, bool enabled) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return TextEditOutcome{};
        }
        try {
            const U::TextEditInput input = frame_input(engine);
            const U::TextAreaResult r = U::text_area(
                *ctx, ui::ui_session_element_decl(desc), to_engine(style), state, input,
                static_cast<float>(delta_seconds(engine)), to_engine(scrollbar), scroll_state,
                ustring_of(placeholder), enabled);
            forward_text_focus(engine, rust::Str{desc.id.data(), desc.id.size()}, desc, r.focused, r.caret_bounds);
            return TextEditOutcome{true, r.changed, false, r.focused};
        } catch (...) {
            return TextEditOutcome{};
        }
    }

    ScrollAreaOutcome ui_scroll_area(EngineView &engine, rust::Str id, const UiElementDesc &desc,
                                     const ScrollbarStyleDesc &style, ScrollAreaStateBox &state, bool enabled,
                                     rust::Box<ScrollContent> content) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return ScrollAreaOutcome{};
        }
        try {
            EngineView *view = &engine;
            ScrollContent *raw = &*content;
            const U::ScrollAreaResult r = U::scroll_area(
                *ctx, ustring_of(id), ui::ui_session_element_decl(desc), to_engine(style), state,
                static_cast<float>(delta_seconds(engine)),
                [view, raw](U::Context &) { scroll_content_run(*raw, *view); }, enabled);
            return ScrollAreaOutcome{true, r.vertical_dragging, r.horizontal_dragging};
        } catch (...) {
            return ScrollAreaOutcome{};
        }
    }

    DropdownOutcome ui_dropdown(const EngineView &engine, rust::Str id, const UiElementDesc &trigger,
                                const DropdownStyleDesc &style, DropdownStateBox &state, std::size_t selected_index,
                                const rust::Vec<rust::String> &options, std::uint16_t font_id, std::uint16_t font_size) {
        U::Context *ctx = ui::ui_session_context();
        if (ctx == nullptr) {
            return DropdownOutcome{};
        }
        try {
            U::DropdownStyle engine_style{};
            engine_style.trigger = to_engine(style.trigger);
            engine_style.list_background = color_of(style.list_background);
            engine_style.option_hovered = color_of(style.option_hovered);
            engine_style.corner_radius = U::CornerRadius::all(style.corner_radius);
            engine_style.option_padding = style.option_padding;
            engine_style.show_arrow_indicator = style.show_arrow_indicator;
            engine_style.arrow_color = color_of(style.arrow_color);
            engine_style.arrow_font_id = static_cast<U::FontId>(style.arrow_font_id);

            std::vector<U::DropdownOption> engine_options;
            for (const rust::String &label : options) {
                std::string text(label);
                engine_options.push_back(U::DropdownOption{[text, font_id, font_size](U::Context &c) {
                    U::TextStyle text_style{};
                    text_style.font_id = static_cast<U::FontId>(font_id);
                    text_style.font_size = font_size;
                    c.text(SFT::ustr{std::string_view{text}}, text_style);
                }});
            }
            const U::DropdownResult r = U::dropdown(
                *ctx, ustring_of(id), ui::ui_session_element_decl(trigger), engine_style, state,
                static_cast<float>(delta_seconds(engine)), selected_index,
                std::span<const U::DropdownOption>{engine_options});
            return DropdownOutcome{true, r.changed, r.selected_index};
        } catch (...) {
            return DropdownOutcome{};
        }
    }

} // namespace sturdy_rs::ui_widgets
