// Rust <-> SturdyEngine 5 shim for the stateful UI widgets. See src/ui_widgets.rs.
#pragma once

#include <cstdint>
#include <memory>

#include <rust/cxx.h>

#include <Renderer/UI/Button.hpp>
#include <Renderer/UI/ColorPicker.hpp>
#include <Renderer/UI/Easing.hpp>
#include <Renderer/UI/Masonry.hpp>
#include <Renderer/UI/Dropdown.hpp>
#include <Renderer/UI/ScrollArea.hpp>
#include <Renderer/UI/Slider.hpp>
#include <Renderer/UI/TextEdit.hpp>
#include <Renderer/UI/Toggle.hpp>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::ui {
    struct UiElementDesc;
}

namespace sturdy_rs::ui_widgets {

    using EngineView = sturdy_rs::EngineView;
    using UiElementDesc = sturdy_rs::ui::UiElementDesc;

    struct ButtonStyleDesc;
    struct ToggleStyleDesc;
    struct ToggleOutcome;
    struct SliderConfigDesc;
    struct SliderStyleDesc;
    struct SliderOutcome;
    struct TextEditStyleDesc;
    struct TextEditOutcome;
    struct ScrollbarStyleDesc;
    struct ScrollAreaOutcome;
    struct DropdownStyleDesc;
    struct DropdownOutcome;
    struct ScrollContent;
    struct MasonryContent;
    struct MasonryConfigDesc;
    struct ScrollSettingsDesc;
    struct ColorPickerConfigDesc;
    struct ColorPickerStyleDesc;
    struct ColorPickerOutcome;

    using ButtonStateBox = SFT::UI::ButtonState;
    using ToggleStateBox = SFT::UI::ToggleState;
    using SliderStateBox = SFT::UI::SliderState;
    using TextEditStateBox = SFT::UI::TextEditState;
    using ScrollAreaStateBox = SFT::UI::ScrollAreaState;
    using DropdownStateBox = SFT::UI::DropdownState;
    using ColorPickerStateBox = SFT::UI::ColorPickerState;

    std::unique_ptr<ButtonStateBox> button_state_new();
    std::unique_ptr<ToggleStateBox> toggle_state_new();
    std::unique_ptr<SliderStateBox> slider_state_new();
    std::unique_ptr<TextEditStateBox> text_edit_state_new();
    std::unique_ptr<ScrollAreaStateBox> scroll_area_state_new();
    std::unique_ptr<DropdownStateBox> dropdown_state_new();
    std::unique_ptr<ColorPickerStateBox> color_picker_state_new();
    std::uint8_t color_picker_state_color_space(const ColorPickerStateBox &state) noexcept;
    void color_picker_state_set_color_space(ColorPickerStateBox &state, std::uint8_t color_space) noexcept;
    rust::String color_space_name(std::uint8_t color_space);
    float ease(std::uint8_t kind, float t) noexcept;
    bool ui_masonry(EngineView &engine, const UiElementDesc &desc, const MasonryConfigDesc &config,
                    rust::Slice<const float> extents, rust::Box<MasonryContent> content);
    ScrollSettingsDesc ui_scroll_settings(const EngineView &engine);
    void ui_set_scroll_settings(EngineView &engine, const ScrollSettingsDesc &settings);
    std::uint8_t ui_desired_cursor(const EngineView &engine);
    void ui_force_cursor(EngineView &engine, std::uint8_t icon);
    bool ui_cursor_management_enabled(const EngineView &engine);
    void ui_set_cursor_management_enabled(EngineView &engine, bool enabled);
    ColorPickerOutcome ui_color_picker(const EngineView &engine, rust::Str id, const UiElementDesc &desc,
                                       const ColorPickerConfigDesc &config, const ColorPickerStyleDesc &style,
                                       ColorPickerStateBox &state, std::array<float, 4> color,
                                       rust::Slice<const std::uint8_t> keys, std::int32_t requested_color_space,
                                       bool enabled);

    bool slider_state_dragging(const SliderStateBox &state) noexcept;
    bool dropdown_state_is_open(const DropdownStateBox &state) noexcept;
    void dropdown_state_set_open(DropdownStateBox &state, bool open) noexcept;

    rust::String text_edit_text(const TextEditStateBox &state);
    void text_edit_set_text(TextEditStateBox &state, rust::Str text);
    std::size_t text_edit_caret(const TextEditStateBox &state) noexcept;
    bool text_edit_has_selection(const TextEditStateBox &state) noexcept;
    std::array<std::size_t, 2> text_edit_selection(const TextEditStateBox &state) noexcept;
    rust::String text_edit_selected_text(const TextEditStateBox &state);
    bool text_edit_focused(const TextEditStateBox &state) noexcept;
    void text_edit_set_focused(TextEditStateBox &state, bool focused) noexcept;
    void text_edit_insert(TextEditStateBox &state, rust::Str text);
    void text_edit_delete_selection(TextEditStateBox &state);
    void text_edit_select_all(TextEditStateBox &state) noexcept;
    void text_edit_select_range(TextEditStateBox &state, std::size_t start, std::size_t end) noexcept;
    void text_edit_set_caret(TextEditStateBox &state, std::size_t scalar_index, bool extend) noexcept;

    ToggleOutcome ui_button(const EngineView &engine, const UiElementDesc &desc, const ButtonStyleDesc &style,
                            ButtonStateBox &state, bool enabled);
    ToggleOutcome ui_checkbox(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                              ToggleStateBox &state, bool checked, std::uint16_t font_id, bool enabled);
    ToggleOutcome ui_radio_button(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                                  ToggleStateBox &state, bool selected, bool enabled);
    ToggleOutcome ui_switch_toggle(const EngineView &engine, const UiElementDesc &desc, const ToggleStyleDesc &style,
                                   ToggleStateBox &state, bool on, bool enabled);
    SliderOutcome ui_slider(const EngineView &engine, const UiElementDesc &desc, const SliderConfigDesc &config,
                            const SliderStyleDesc &style, SliderStateBox &state, double value, bool enabled);
    TextEditOutcome ui_text_input(EngineView &engine, const UiElementDesc &desc, const TextEditStyleDesc &style,
                                  TextEditStateBox &state, rust::Str placeholder, bool enabled);
    TextEditOutcome ui_text_area(EngineView &engine, const UiElementDesc &desc, const TextEditStyleDesc &style,
                                 const ScrollbarStyleDesc &scrollbar, TextEditStateBox &state,
                                 ScrollAreaStateBox &scroll_state, rust::Str placeholder, bool enabled);
    struct RichSpanDesc;
    struct RichTextOutcome;
    RichTextOutcome ui_rich_text(EngineView &engine, const UiElementDesc &desc, const TextEditStyleDesc &style,
                                 const ScrollbarStyleDesc &scrollbar, TextEditStateBox &state,
                                 ScrollAreaStateBox &scroll_state, const rust::Vec<RichSpanDesc> &spans);
    ScrollAreaOutcome ui_scroll_area(EngineView &engine, rust::Str id, const UiElementDesc &desc,
                                     const ScrollbarStyleDesc &style, ScrollAreaStateBox &state, bool enabled,
                                     rust::Box<ScrollContent> content);
    DropdownOutcome ui_dropdown(const EngineView &engine, rust::Str id, const UiElementDesc &trigger,
                                const DropdownStyleDesc &style, DropdownStateBox &state, std::size_t selected_index,
                                const rust::Vec<rust::String> &options, std::uint16_t font_id, std::uint16_t font_size);

} // namespace sturdy_rs::ui_widgets
