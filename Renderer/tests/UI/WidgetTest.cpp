#include <Renderer/UI/UI.hpp>
#include <Renderer/UI/TextBridge.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace {

    using SFT::UI::Color;
    using SFT::UI::ColorPickerColorSpace;
    using SFT::UI::ColorPickerConfig;
    using SFT::UI::ColorPickerInput;
    using SFT::UI::ColorPickerKey;
    using SFT::UI::ColorPickerPart;
    using SFT::UI::ColorPickerResult;
    using SFT::UI::ColorPickerState;
    using SFT::UI::ColorPickerStyle;
    using SFT::UI::Context;
    using SFT::UI::CursorIcon;
    using SFT::UI::DocumentTextAreaResult;
    using SFT::UI::DocumentTextAreaState;
    using SFT::UI::EditKey;
    using SFT::UI::ElementBounds;
    using SFT::UI::ElementDecl;
    using SFT::UI::FloatingAttachPoint;
    using SFT::UI::FloatingAttachTo;
    using SFT::UI::FloatingClipTo;
    using SFT::UI::FloatingConfig;
    using SFT::UI::FontId;
    using SFT::UI::FrameSnapshot;
    using SFT::UI::LayoutDirection;
    using SFT::UI::OutlineCacheKey;
    using SFT::UI::OutlineCacheKeyHash;
    using SFT::UI::Padding;
    using SFT::UI::PointerState;
    using SFT::UI::QuadDraw;
    using SFT::UI::ScrollAreaState;
    using SFT::UI::ScrollAreaVisualPart;
    using SFT::UI::ScrollbarStyle;
    using SFT::UI::SizingAxis;
    using SFT::UI::SliderConfig;
    using SFT::UI::SliderInput;
    using SFT::UI::SliderKey;
    using SFT::UI::SliderResult;
    using SFT::UI::SliderState;
    using SFT::UI::SliderStyle;
    using SFT::UI::TextBridge;
    using SFT::UI::TextEditBindings;
    using SFT::UI::TextEditFeatures;
    using SFT::UI::TextEditInput;
    using SFT::UI::TextEditKeyBinding;
    using SFT::UI::TextEditState;
    using SFT::UI::TextEditStyle;
    using SFT::UI::color_picker;
    using SFT::UI::scroll_area;
    using SFT::UI::scroll_area_part_id;
    using SFT::UI::slider;
    using SFT::UI::text_area;
    using SFT::UI::text_input;
    using SFT::Text::Font;
    using SFT::Text::FontStack;

    /// Performs the near operation using the supplied arguments.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    /// @param tolerance `tolerance` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool near(f64 lhs, f64 rhs, f64 tolerance = 1.0e-6) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    /// Creates a context value from the supplied arguments.
    ///
    /// @return Returns the value produced by the operation.
    /// @pre `created.has_value()`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    SFT::UI::Context make_context() {
        auto created = SFT::UI::Context::create(SFT::UI::Context::Config{});
        assert(created.has_value());
        return std::move(*created);
    }

    /// Performs the slider drag and keyboard operation using the supplied arguments.
    ///
    /// @pre `near(result.value, 0.0)`; debug builds assert if this precondition is violated.
    /// @pre `result.changed`; debug builds assert if this precondition is violated.
    /// @pre `result.dragging`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void slider_drag_and_keyboard() {
        Context context = make_context();
        SliderState state;
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(20.0f)},
            .id = UString{"test-slider"},
        };
        const SliderConfig config{.min = 0.0, .max = 100.0, .step = 1.0};
        const SliderStyle style{};

        context.begin_layout({200.0f, 100.0f});
        SliderResult result = slider(context, decl, config, style, state, 0.0);
        assert(near(result.value, 0.0));
        (void)context.finish_frame();

        context.begin_layout({200.0f, 100.0f}, PointerState{
                                                   .position = {50.0f, 10.0f},
                                                   .down = true,
                                                   .pressed = true,
                                               });
        result = slider(context, decl, config, style, state, result.value);
        assert(result.changed);
        assert(result.dragging);
        assert(near(result.value, 50.0));
        (void)context.finish_frame();

        context.begin_layout({200.0f, 100.0f}, PointerState{
                                                   .position = {150.0f, 10.0f},
                                                   .released = true,
                                               });
        result = slider(context, decl, config, style, state, result.value);
        assert(result.changed);
        assert(result.committed);
        assert(!result.dragging);
        assert(near(result.value, 100.0));
        (void)context.finish_frame();

        const std::array keys{SliderKey::Minimum, SliderKey::PageIncrement};
        context.begin_layout({200.0f, 100.0f});
        result = slider(context, decl, config, style, state, result.value, SliderInput{.keys = keys, .request_focus = true});
        assert(result.changed);
        assert(result.committed);
        assert(near(result.value, 10.0));
        (void)context.finish_frame();

        const SliderConfig uneven_step{.min = 0.0, .max = 10.0, .step = 6.0};
        const std::array maximum_key{SliderKey::Maximum};
        context.begin_layout({200.0f, 100.0f});
        result = slider(context, decl, uneven_step, style, state, 0.0, SliderInput{.keys = maximum_key, .request_focus = true});
        assert(result.changed);
        assert(near(result.value, 6.0));
        (void)context.finish_frame();
    }

    /// Performs the color picker preserves achromatic hue operation using the supplied arguments.
    ///
    /// @pre `result.color.b > result.color.r`; debug builds assert if this precondition is violated.
    /// @pre `result.changed`; debug builds assert if this precondition is violated.
    /// @pre `result.committed`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void color_picker_preserves_achromatic_hue() {
        Context context = make_context();
        ColorPickerState state;
        ColorPickerStyle style{};
        style.plane_size = {100.0f, 100.0f};
        const ColorPickerConfig config{.show_color_space_dropdown = false, .show_alpha = true, .show_preview = false};
        const ElementDecl decl{};
        const UString id{"test-picker"};

        context.begin_layout({200.0f, 200.0f});
        ColorPickerResult result = color_picker(context, id, decl, config, style, state, Color{0.0, 0.0, 1.0, 1.0});
        assert(result.color.b > result.color.r);
        (void)context.finish_frame();

        const std::array hue_keys{ColorPickerKey::Right};
        context.begin_layout({200.0f, 200.0f});
        result = color_picker(context, id, decl, config, style, state, Color{0.5, 0.5, 0.5, 1.0}, ColorPickerInput{
                                                                                                      .keys = hue_keys,
                                                                                                      .request_focus = ColorPickerPart::Hue,
                                                                                                  });
        assert(result.changed);
        assert(result.committed);
        assert(near(result.color.r, result.color.g));
        assert(near(result.color.g, result.color.b));
        (void)context.finish_frame();

        const std::array saturation_keys{ColorPickerKey::Right};
        context.begin_layout({200.0f, 200.0f});
        result = color_picker(context, id, decl, config, style, state, result.color, ColorPickerInput{
                                                                                         .keys = saturation_keys,
                                                                                         .request_focus = ColorPickerPart::SaturationValue,
                                                                                     });
        assert(result.changed);
        assert(result.committed);
        assert(result.color.b > result.color.r);
        (void)context.finish_frame();
    }

    /// Performs the color picker selects foundation color space operation using the supplied arguments.
    ///
    /// @pre `result.color_space == ColorPickerColorSpace::Srgb`; debug builds assert if this precondition is violated.
    /// @pre `std::holds_alternative<SFT::Foundation::Color::Srgb>(result.value)`; debug builds assert if this precondition is violated.
    /// @pre `trigger_bounds.has_value()`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void color_picker_selects_foundation_color_space() {
        Context context = make_context();
        ColorPickerState state;
        ColorPickerStyle style{};
        style.plane_size = {100.0f, 80.0f};
        const ColorPickerConfig config{
            .show_color_space_dropdown = true,
            .show_alpha = false,
            .show_preview = false,
        };
        const UString id{"test-picker-space"};

        context.begin_layout({300.0f, 600.0f});
        ColorPickerResult result = color_picker(context, id, ElementDecl{}, config, style, state,
                                                Color{0.2, 0.4, 0.8, 1.0});
        assert(result.color_space == ColorPickerColorSpace::Srgb);
        assert(std::holds_alternative<SFT::Foundation::Color::Srgb>(result.value));
        (void)context.finish_frame();

        const UString dropdown_id{"test-picker-space#color-space"};
        const std::optional<ElementBounds> trigger_bounds = context.element_bounds(dropdown_id);
        assert(trigger_bounds.has_value());
        const glm::vec2 trigger_center = trigger_bounds->position + trigger_bounds->size * 0.5f;
        context.begin_layout({300.0f, 600.0f}, PointerState{
                                                    .position = trigger_center,
                                                    .pressed = true,
                                                    .press_position = trigger_center,
                                                    .released = true,
                                                });
        result = color_picker(context, id, ElementDecl{}, config, style, state, result.color);
        assert(!result.color_space_changed);
        (void)context.finish_frame();

        const UString oklch_option_id{"test-picker-space#color-space#13"};
        const std::optional<ElementBounds> option_bounds = context.element_bounds(oklch_option_id);
        assert(option_bounds.has_value());
        const glm::vec2 option_center = option_bounds->position + option_bounds->size * 0.5f;
        context.begin_layout({300.0f, 600.0f}, PointerState{
                                                    .position = option_center,
                                                    .pressed = true,
                                                    .press_position = option_center,
                                                    .released = true,
                                                });
        result = color_picker(context, id, ElementDecl{}, config, style, state, result.color);
        assert(result.changed);
        assert(result.committed);
        assert(result.color_space_changed);
        assert(result.color_space == ColorPickerColorSpace::Oklch);
        assert(std::holds_alternative<SFT::Foundation::Color::Oklch>(result.value));
        const SFT::Foundation::Color::Oklch expected =
            SFT::Foundation::Color::convert_to<SFT::Foundation::Color::Oklch>(result.color);
        const SFT::Foundation::Color::Oklch selected = std::get<SFT::Foundation::Color::Oklch>(result.value);
        assert(near(selected.l, expected.l));
        assert(near(selected.c, expected.c));
        assert(near(selected.h, expected.h));
        (void)context.finish_frame();

        context.begin_layout({300.0f, 600.0f});
        result = color_picker(context, id, ElementDecl{}, config, style, state, result.color,
                              ColorPickerInput{.requested_color_space = ColorPickerColorSpace::DisplayP3});
        assert(!result.color_space_changed);
        assert(result.color_space == ColorPickerColorSpace::DisplayP3);
        assert(std::holds_alternative<SFT::Foundation::Color::DisplayP3>(result.value));
        (void)context.finish_frame();
    }


    /// Performs the color picker component sliders follow selected space operation using the supplied arguments.
    ///
    /// @pre `!context.element_bounds(UString{"test-picker-components#hue"}).has_value()`; debug builds assert if this precondition is violated.
    /// @pre `r_bounds.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `context.element_bounds(UString{"test-picker-components#component:2"}).has_value()`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void color_picker_component_sliders_follow_selected_space() {
        Context context = make_context();
        ColorPickerState state;
        ColorPickerStyle style{};
        style.plane_size = {100.0f, 80.0f};
        const ColorPickerConfig config{.show_color_space_dropdown = true, .show_alpha = false, .show_preview = false};
        const UString id{"test-picker-components"};

        context.begin_layout({300.0f, 600.0f});
        ColorPickerResult result = color_picker(context, id, ElementDecl{}, config, style, state,
                                                Color{0.2, 0.4, 0.8, 1.0});
        (void)context.finish_frame();

        assert(!context.element_bounds(UString{"test-picker-components#hue"}).has_value());
        const std::optional<ElementBounds> r_bounds =
            context.element_bounds(UString{"test-picker-components#component:0"});
        assert(r_bounds.has_value());
        assert(context.element_bounds(UString{"test-picker-components#component:2"}).has_value());
        assert(!context.element_bounds(UString{"test-picker-components#component:3"}).has_value());

        const glm::vec2 press{r_bounds->position.x + r_bounds->size.x - 1.0f,
                              r_bounds->position.y + r_bounds->size.y * 0.5f};
        context.begin_layout({300.0f, 600.0f}, PointerState{
                                                   .position = press,
                                                   .down = true,
                                                   .pressed = true,
                                                   .press_position = press,
                                               });
        result = color_picker(context, id, ElementDecl{}, config, style, state, result.color);
        assert(result.changed);
        assert(result.color.r > 0.95);

        assert(near(result.color.g, 0.4, 1.0e-6));
        assert(near(result.color.b, 0.8, 1.0e-6));
        (void)context.finish_frame();
    }


    /// Reports whether text caret has no layout footprint.
    ///
    /// @pre `state.focused()`; debug builds assert if this precondition is violated.
    /// @pre `caret_anchor.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `caret_anchor->size.x == 0.0f`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_caret_has_no_layout_footprint() {
        Context context = make_context();
        TextEditState state;
        const TextEditStyle style{};
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(200.0f), SizingAxis::fixed(30.0f)},
            .id = UString{"caret-input"},
        };

        context.begin_layout({300.0f, 100.0f});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();

        const glm::vec2 press{100.0f, 15.0f};
        context.begin_layout({300.0f, 100.0f}, PointerState{
                                                   .position = press,
                                                   .down = true,
                                                   .pressed = true,
                                                   .press_position = press,
                                               });
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();
        assert(state.focused());

        context.begin_layout({300.0f, 100.0f}, PointerState{.position = press});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();

        const std::optional<ElementBounds> caret_anchor = context.element_bounds(UString{"caret-input#caret"});
        assert(caret_anchor.has_value());
        assert(caret_anchor->size.x == 0.0f);
    }


    /// Performs the text area line heights ignore caret operation using the supplied arguments.
    ///
    /// @pre `unfocused_height > 0.0f`; debug builds assert if this precondition is violated.
    /// @pre `state.focused()`; debug builds assert if this precondition is violated.
    /// @pre `near(on_last, unfocused_height, 0.01)`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_area_line_heights_ignore_caret() {
        Context context = make_context();
        TextEditState state;
        state.set_text(UString{"ab\n\ncd"});
        const TextEditStyle style{};
        const ScrollbarStyle scrollbar_style{};
        ScrollAreaState scroll_state;
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(200.0f), SizingAxis::fixed(120.0f)},
            .id = UString{"area-strut"},
        };

        const auto build = [&](const PointerState &pointer, std::span<const EditKey> pressed = {}) {
            TextEditInput input{};
            input.keys.assign(pressed.begin(), pressed.end());
            context.begin_layout({300.0f, 200.0f}, pointer, 0.016f);
            (void)text_area(context, decl, style, state, input, 0.016f, scrollbar_style, scroll_state);
            (void)context.finish_frame();
            return context.scroll_metrics(decl.id).content_size.y;
        };

        const f32 unfocused_height = build(PointerState{});
        assert(unfocused_height > 0.0f);

        const glm::vec2 press{100.0f, 60.0f};
        (void)build(PointerState{.position = press, .down = true, .pressed = true, .press_position = press});
        assert(state.focused());


        const std::array up{EditKey::Up};
        const f32 on_last = build(PointerState{});
        const f32 on_empty = build(PointerState{}, up);
        const f32 on_first = build(PointerState{}, up);
        assert(near(on_last, unfocused_height, 0.01));
        assert(near(on_empty, unfocused_height, 0.01));
        assert(near(on_first, unfocused_height, 0.01));
    }


    /// Performs the text edit state click streak and word select operation using the supplied arguments.
    ///
    /// @pre `state.register_click(pos, true) == 1`; debug builds assert if this precondition is violated.
    /// @pre `state.register_click(pos, true) == 2`; debug builds assert if this precondition is violated.
    /// @pre `state.register_click(pos, true) == 3`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_edit_state_click_streak_and_word_select() {
        TextEditState state;
        state.set_text(UString{"hello world foo"});

        const glm::vec2 pos{10.0f, 10.0f};
        assert(state.register_click(pos,                       true) == 1);
        assert(state.register_click(pos,                       true) == 2);
        assert(state.register_click(pos,                       true) == 3);
        assert(state.register_click(pos,                       true) == 3);


        const glm::vec2 far{200.0f, 10.0f};
        assert(state.register_click(far,                       true) == 1);
        assert(state.register_click(far,                       true) == 2);


        assert(state.register_click(far,                       false) == 1);

        state.select_word_at(2);
        assert(state.selected_text().cpp_string() == "hello");

        state.select_word_at(8);
        assert(state.selected_text().cpp_string() == "world");

        state.select_range(6, 11);
        assert(state.selected_text().cpp_string() == "world");
    }

    /// Performs the document text area virtualizes large documents operation using the supplied arguments.
    ///
    /// @pre `result.first_rendered_line == 0`; debug builds assert if this precondition is violated.
    /// @pre `result.rendered_line_count < 32`; debug builds assert if this precondition is violated.
    /// @pre `state.document().snapshot().line_count() == 10'001`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void document_text_area_virtualizes_large_documents() {
        Context context = make_context();
        DocumentTextAreaState state;
        std::string source;
        for (usize line = 0; line < 10'000; ++line) {
            source += "line ";
            source += std::to_string(line);
            source += '\n';
        }
        state.set_text(source);
        const TextEditStyle style{};
        const ScrollbarStyle scrollbar_style{};
        ScrollAreaState scroll_state{};
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(300.0f), SizingAxis::fixed(120.0f)},
            .padding = Padding::all(4),
            .id = UString{"virtual-document-area"},
        };
        context.begin_layout({400.0f, 200.0f});
        const DocumentTextAreaResult result =
            text_area(context, decl, style, state, TextEditInput{}, 0.016f, scrollbar_style, scroll_state, 20.0f);
        (void)context.finish_frame();
        assert(result.first_rendered_line == 0);
        assert(result.rendered_line_count < 32);
        assert(state.document().snapshot().line_count() == 10'001);
    }

    /// Performs the text edit features and rebinding operation using the supplied arguments.
    ///
    /// @pre `!typing_result.changed`; debug builds assert if this precondition is violated.
    /// @pre `state.text().cpp_string() == "alpha"`; debug builds assert if this precondition is violated.
    /// @pre `!delete_result.changed`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_edit_features_and_rebinding() {
        TextEditState state;
        state.set_text(UString{"alpha"});
        state.set_focused(true);

        TextEditFeatures no_typing{};
        no_typing.typing = false;
        const TextEditState::ApplyResult typing_result = state.apply_input(TextEditInput{.typed_text = "!"}, false, no_typing);
        assert(!typing_result.changed);
        assert(state.text().cpp_string() == "alpha");

        TextEditFeatures no_deletion{};
        no_deletion.deletion = false;
        state.set_caret_to(state.text().size(), false);
        const TextEditState::ApplyResult delete_result =
            state.apply_input(TextEditInput{.keys = {EditKey::Backspace}}, false, no_deletion);
        assert(!delete_result.changed);
        assert(state.text().cpp_string() == "alpha");

        TextEditBindings bindings{};
        bindings.keys.push_back(TextEditKeyBinding{.trigger = EditKey::Left, .command = EditKey::End});
        state.set_caret_to(0, false);
        (void)state.apply_input(TextEditInput{.keys = {EditKey::Left}}, false, TextEditFeatures{}, bindings);
        assert(state.caret() == state.text().size());


        bindings.keys[0].enabled = false;
        state.set_caret_to(0, false);
        (void)state.apply_input(TextEditInput{.keys = {EditKey::Left}}, false, TextEditFeatures{}, bindings);
        assert(state.caret() == 0);
    }


    /// Performs the outline cache key distinguishes font faces with shared low bits operation using the supplied arguments.
    ///
    /// @pre `!(primary_key == fallback_key)`; debug builds assert if this precondition is violated.
    /// @pre `map.size() == 2`; debug builds assert if this precondition is violated.
    /// @pre `map[primary_key] == 100`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void outline_cache_key_distinguishes_font_faces_with_shared_low_bits() {
        constexpr u64 slot = 1;
        const OutlineCacheKey primary_key{.font_id = slot, .glyph_id = 5};
        const OutlineCacheKey fallback_key{.font_id = slot | (u64{2} << 32), .glyph_id = 5};
        assert(!(primary_key == fallback_key));

        std::unordered_map<OutlineCacheKey, int, OutlineCacheKeyHash> map;
        map[primary_key] = 100;
        map[fallback_key] = 200;
        assert(map.size() == 2);
        assert(map[primary_key] == 100);
        assert(map[fallback_key] == 200);
    }


    /// Registers font stores fallback fonts with stable addresses using the supplied arguments and current state.
    ///
    /// @pre `stack != nullptr`; debug builds assert if this precondition is violated.
    /// @pre `stack->primary == &primary`; debug builds assert if this precondition is violated.
    /// @pre `stack->fallbacks.size() == 2`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void register_font_stores_fallback_fonts_with_stable_addresses() {
        Font primary;
        Font fallback_a;
        Font fallback_b;
        TextBridge bridge;

        constexpr FontId font_id = 7;
        const std::array<const Font *, 2> fallbacks{&fallback_a, &fallback_b};
        bridge.register_font(font_id, primary,                    nullptr, fallbacks);

        const FontStack *stack = bridge.font_stack(font_id);
        assert(stack != nullptr);
        assert(stack->primary == &primary);
        assert(stack->fallbacks.size() == 2);
        assert(stack->fallbacks[0].font == &fallback_a);
        assert(stack->fallbacks[1].font == &fallback_b);


        assert(stack->fallbacks[0].font_id != stack->fallbacks[1].font_id);
        assert(stack->fallbacks[0].font_id != stack->primary_font_id);
        assert(!stack->fallbacks[0].is_color && !stack->fallbacks[1].is_color);


        for (FontId other = 100; other < 164; ++other) {
            bridge.register_font(other, primary);
        }
        const FontStack *after_growth = bridge.font_stack(font_id);
        assert(after_growth != nullptr);
        assert(after_growth->fallbacks.size() == 2);
        assert(after_growth->fallbacks[0].font == &fallback_a);
        assert(after_growth->fallbacks[1].font == &fallback_b);


        const std::array<const Font *, 1> replacement_fallbacks{&fallback_b};
        bridge.register_font(font_id, primary, nullptr, replacement_fallbacks);
        const FontStack *after_replace = bridge.font_stack(font_id);
        assert(after_replace != nullptr);
        assert(after_replace->fallbacks.size() == 1);
        assert(after_replace->fallbacks[0].font == &fallback_b);
    }


    /// Performs the ime composition swallows enter and escape operation using the supplied arguments.
    ///
    /// @pre `!swallowed_enter.submitted`; debug builds assert if this precondition is violated.
    /// @pre `single_line.focused()`; debug builds assert if this precondition is violated.
    /// @pre `!swallowed_escape.submitted`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void ime_composition_swallows_enter_and_escape() {
        TextEditState single_line;
        single_line.set_text(UString{"alpha"});
        single_line.set_focused(true);

        const TextEditState::ApplyResult swallowed_enter =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = true},               false);
        assert(!swallowed_enter.submitted);
        assert(single_line.focused());

        const TextEditState::ApplyResult swallowed_escape =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Escape}, .composing = true},               false);
        assert(!swallowed_escape.submitted);
        assert(single_line.focused());


        const TextEditState::ApplyResult normal_enter =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = false},               false);
        assert(normal_enter.submitted);
        assert(single_line.focused());

        const TextEditState::ApplyResult normal_escape =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Escape}, .composing = false},               false);
        assert(!normal_escape.submitted);
        assert(!single_line.focused());


        TextEditState multiline;
        multiline.set_text(UString{"line"});
        multiline.set_focused(true);
        multiline.set_caret_to(multiline.text().size(), false);
        const TextEditState::ApplyResult swallowed_newline =
            multiline.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = true},               true);
        assert(!swallowed_newline.changed);
        assert(multiline.text().cpp_string() == "line");
        const TextEditState::ApplyResult normal_newline =
            multiline.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = false},               true);
        assert(normal_newline.changed);
        assert(multiline.text().cpp_string() == "line\n");
    }


    /// Reports whether ime composition text is visible but not inserted.
    ///
    /// @pre `state.composition_text() == "konnichiwa"`; debug builds assert if this precondition is violated.
    /// @pre `state.text().cpp_string() == "ab"`; debug builds assert if this precondition is violated.
    /// @pre `committed.changed`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void ime_composition_text_is_visible_but_not_inserted() {
        TextEditState state;
        state.set_text(UString{"ab"});
        state.set_focused(true);
        state.set_caret_to(state.text().size(), false);

        (void)state.apply_input(TextEditInput{.composition_text = "konnichiwa", .composing = true},               false);
        assert(state.composition_text() == "konnichiwa");
        assert(state.text().cpp_string() == "ab");


        const TextEditState::ApplyResult committed = state.apply_input(
            TextEditInput{.typed_text = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf", .composition_text = "", .composing = false},
                          false);
        assert(committed.changed);
        assert(state.composition_text().empty());
        assert(state.text().cpp_string() == "ab\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf");


        (void)state.apply_input(TextEditInput{.composition_text = "leftover"},               false);
        assert(state.composition_text() == "leftover");
        state.set_focused(false);
        (void)state.apply_input(TextEditInput{.composition_text = "leftover"},               false);
        assert(state.composition_text().empty());
    }


    /// Performs the text input shift click extends selection operation using the supplied arguments.
    ///
    /// @pre `!state.has_selection()`; debug builds assert if this precondition is violated.
    /// @pre `state.has_selection()`; debug builds assert if this precondition is violated.
    /// @pre `state.selection_max() == 5`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_input_shift_click_extends_selection() {
        Context context = make_context();
        TextEditState state;
        state.set_text(UString{"hello world"});
        state.set_caret_to(5,            false);
        assert(!state.has_selection());
        const TextEditStyle style{};
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(200.0f), SizingAxis::fixed(30.0f)},
            .id = UString{"shift-click-input"},
        };

        context.begin_layout({300.0f, 100.0f});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();

        const glm::vec2 press{20.0f, 15.0f};
        TextEditInput shift_input{};
        shift_input.shift_held = true;
        context.begin_layout({300.0f, 100.0f}, PointerState{
                                                    .position = press,
                                                    .down = true,
                                                    .pressed = true,
                                                    .press_position = press,
                                                });
        (void)text_input(context, decl, style, state, shift_input, 0.016f);
        (void)context.finish_frame();

        assert(state.has_selection());
        assert(state.selection_max() == 5);
        assert(state.selected_text().cpp_string() == "hello");
    }


    /// Performs the text input drag acquires and releases pointer capture operation using the supplied arguments.
    ///
    /// @pre `context.has_pointer_capture(decl.id)`; debug builds assert if this precondition is violated.
    /// @pre `!context.has_pointer_capture(decl.id)`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_input_drag_acquires_and_releases_pointer_capture() {
        Context context = make_context();
        TextEditState state;
        state.set_text(UString{"hello world"});
        const TextEditStyle style{};
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(200.0f), SizingAxis::fixed(30.0f)},
            .id = UString{"drag-input"},
        };

        context.begin_layout({300.0f, 100.0f});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();

        const glm::vec2 press{20.0f, 15.0f};
        context.begin_layout({300.0f, 100.0f}, PointerState{
                                                    .position = press,
                                                    .down = true,
                                                    .pressed = true,
                                                    .press_position = press,
                                                });
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();
        assert(context.has_pointer_capture(decl.id));

        const glm::vec2 dragged{150.0f, 15.0f};
        context.begin_layout({300.0f, 100.0f}, PointerState{.position = dragged, .down = true});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();
        assert(context.has_pointer_capture(decl.id));

        context.begin_layout({300.0f, 100.0f}, PointerState{.position = dragged, .released = true});
        (void)text_input(context, decl, style, state, TextEditInput{}, 0.016f);
        (void)context.finish_frame();
        assert(!context.has_pointer_capture(decl.id));
    }

    /// Computes the scroll container moves child offset required by the supplied values.
    ///
    /// @pre `before.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `after.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `after->position.y < before->position.y - 1.0f`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void scroll_container_moves_child_offset() {
        Context context = make_context();
        const UString box_id{"scroll-box"};
        const UString child_b_id{"child-b"};

        const auto build = [&](const PointerState &pointer) {
            context.begin_layout({200.0f, 200.0f}, pointer, 0.016f);
            {
                auto box = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .direction = LayoutDirection::TopToBottom,
                    .clip = {.vertical = true},
                    .id = box_id,
                });
                (void)box;
                {
                    auto a = context.element(ElementDecl{
                        .sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)},
                        .id = UString{"child-a"},
                    });
                    (void)a;
                }
                {
                    auto b = context.element(ElementDecl{
                        .sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)},
                        .id = child_b_id,
                    });
                    (void)b;
                }
            }
            (void)context.finish_frame();
        };

        build(PointerState{});
        const std::optional<ElementBounds> before = context.element_bounds(child_b_id);
        assert(before.has_value());


        const PointerState hovering_and_scrolling{.position = {50.0f, 50.0f}, .scroll_delta = {0.0f, -60.0f}};
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        const std::optional<ElementBounds> after = context.element_bounds(child_b_id);
        assert(after.has_value());


        assert(after->position.y < before->position.y - 1.0f);
    }


    /// Performs the floating attached parent clips to ancestor operation using the supplied arguments.
    ///
    /// @pre `floater_quad != nullptr`; debug builds assert if this precondition is violated.
    /// @pre `floater_quad->scissor.x == 0`; debug builds assert if this precondition is violated.
    /// @pre `floater_quad->scissor.y == 0`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void floating_attached_parent_clips_to_ancestor() {
        Context context = make_context();
        const UString clip_box_id{"clip-box"};
        const UString floater_id{"floater"};

        context.begin_layout({200.0f, 200.0f});
        {


            auto box = context.element(ElementDecl{
                .sizing = {SizingAxis::fixed(50.0f), SizingAxis::fixed(50.0f)},
                .clip = {.vertical = true},
                .id = clip_box_id,
            });
            (void)box;


            auto floater = context.element(ElementDecl{
                .sizing = {SizingAxis::fixed(20.0f), SizingAxis::fixed(20.0f)},
                .background_color = Color{1.0, 1.0, 1.0, 1.0},
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::Parent,
                    .element_attach_point = FloatingAttachPoint::LeftTop,
                    .parent_attach_point = FloatingAttachPoint::LeftTop,
                    .offset = {0.0f, 300.0f},
                    .clip_to = FloatingClipTo::AttachedParent,
                },
                .id = floater_id,
            });
            (void)floater;
        }
        FrameSnapshot snapshot = context.finish_frame();

        const QuadDraw *floater_quad = nullptr;
        for (const QuadDraw &quad : snapshot.quads()) {


            if (near(quad.instance.size.x, 20.0) && near(quad.instance.size.y, 20.0)) {
                floater_quad = &quad;
            }
        }
        assert(floater_quad != nullptr);


        assert(floater_quad->scissor.x == 0);
        assert(floater_quad->scissor.y == 0);
        assert(floater_quad->scissor.width == 50);
        assert(floater_quad->scissor.height == 50);
    }


    /// Performs the clicked respects ancestor clip operation using the supplied arguments.
    ///
    /// @pre `context.clicked(visible_floater_id)`; debug builds assert if this precondition is violated.
    /// @pre `!context.clicked(hidden_floater_id)`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void clicked_respects_ancestor_clip() {
        Context context = make_context();
        const UString clip_box_id{"click-clip-box"};
        const UString visible_floater_id{"click-floater-visible"};
        const UString hidden_floater_id{"click-floater-hidden"};

        const auto build = [&](const PointerState &pointer) {
            context.begin_layout({200.0f, 200.0f}, pointer);
            {


                auto box = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(50.0f), SizingAxis::fixed(50.0f)},
                    .clip = {.vertical = true},
                    .id = clip_box_id,
                });
                (void)box;

                auto visible = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(20.0f), SizingAxis::fixed(20.0f)},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::LeftTop,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {10.0f, 10.0f},
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                    .id = visible_floater_id,
                });
                (void)visible;


                auto hidden = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(20.0f), SizingAxis::fixed(20.0f)},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::LeftTop,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {0.0f, 100.0f},
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                    .id = hidden_floater_id,
                });
                (void)hidden;
            }
            (void)context.finish_frame();
        };

        build(PointerState{});

        const glm::vec2 visible_press{20.0f, 20.0f};
        build(PointerState{.position = visible_press, .pressed = true, .press_position = visible_press});
        assert(context.clicked(visible_floater_id));

        build(PointerState{});

        const glm::vec2 hidden_press{10.0f, 110.0f};
        build(PointerState{.position = hidden_press, .pressed = true, .press_position = hidden_press});
        assert(!context.clicked(hidden_floater_id));
    }


    /// Scrolls metrics and set scroll offset work using the supplied arguments and current state.
    ///
    /// @pre `before.found`; debug builds assert if this precondition is violated.
    /// @pre `before.vertical`; debug builds assert if this precondition is violated.
    /// @pre `!before.horizontal`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void scroll_metrics_and_set_scroll_offset_work() {
        Context context = make_context();
        const UString box_id{"metrics-box"};
        const UString child_b_id{"metrics-child-b"};

        const auto build = [&]() {
            context.begin_layout({200.0f, 200.0f});
            {
                auto box = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .direction = LayoutDirection::TopToBottom,
                    .clip = {.vertical = true},
                    .id = box_id,
                });
                (void)box;
                { auto a = context.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"metrics-child-a"}}); (void)a; }
                { auto b = context.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = child_b_id}); (void)b; }
            }
            (void)context.finish_frame();
        };

        build();
        const Context::ScrollMetrics before = context.scroll_metrics(box_id);
        assert(before.found);
        assert(before.vertical);
        assert(!before.horizontal);
        assert(near(before.content_size.y, 160.0));
        assert(near(before.container_size.y, 100.0));
        assert(near(before.offset.y, 0.0));


        assert(context.set_scroll_offset(box_id, glm::vec2{0.0f, -1000.0f}));
        const Context::ScrollMetrics clamped = context.scroll_metrics(box_id);
        assert(near(clamped.offset.y, -60.0));


        assert(!context.set_scroll_offset(UString{"does-not-exist"}, glm::vec2{0.0f, -10.0f}));

        build();
        const std::optional<ElementBounds> child_b_bounds = context.element_bounds(child_b_id);
        assert(child_b_bounds.has_value());
        assert(near(child_b_bounds->position.y, 20.0));
    }


    /// Scrolls area shows thumb only when hovering overflowing content using the supplied arguments and current state.
    ///
    /// @pre `!context.element_bounds(overflow_thumb_id).has_value()`; debug builds assert if this precondition is violated.
    /// @pre `!context.element_bounds(fit_thumb_id).has_value()`; debug builds assert if this precondition is violated.
    /// @pre `context.element_bounds(overflow_thumb_id).has_value()`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void scroll_area_shows_thumb_only_when_hovering_overflowing_content() {
        Context context = make_context();
        ScrollAreaState overflowing_state;
        ScrollAreaState fitting_state;
        const UString overflowing_id{"scroll-area-overflow"};
        const UString fitting_id{"scroll-area-fit"};
        const UString overflow_thumb_id = scroll_area_part_id(overflowing_id, ScrollAreaVisualPart::VerticalThumb);
        const UString fit_thumb_id = scroll_area_part_id(fitting_id, ScrollAreaVisualPart::VerticalThumb);

        const auto build = [&](const PointerState &pointer, f32 dt) {
            context.begin_layout({200.0f, 200.0f}, pointer, dt);
            (void)scroll_area(
                context, overflowing_id,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .direction = LayoutDirection::TopToBottom,
                    .clip = {.vertical = true},
                },
                ScrollbarStyle{}, overflowing_state, dt, [](Context &ctx) {


                    { auto a = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"sa-overflow-a"}}); (void)a; }
                    { auto b = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"sa-overflow-b"}}); (void)b; }
                });


            (void)scroll_area(
                context, fitting_id,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .direction = LayoutDirection::TopToBottom,
                    .clip = {.vertical = true},
                    .floating = FloatingConfig{.attach_to = FloatingAttachTo::Root, .offset = {150.0f, 0.0f}},
                },
                ScrollbarStyle{}, fitting_state, dt, [](Context &ctx) {
                    auto a = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(40.0f), SizingAxis::fixed(40.0f)}, .id = UString{"sa-fit-a"}});
                    (void)a;
                });
            (void)context.finish_frame();
        };


        build(PointerState{.position = {500.0f, 500.0f}}, 0.016f);
        assert(!context.element_bounds(overflow_thumb_id).has_value());
        assert(!context.element_bounds(fit_thumb_id).has_value());


        const PointerState hovering_overflow{.position = {50.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering_overflow, 0.05f);
        }
        assert(context.element_bounds(overflow_thumb_id).has_value());


        const PointerState hovering_fit{.position = {200.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering_fit, 0.05f);
        }
        assert(!context.element_bounds(fit_thumb_id).has_value());


        for (int i = 0; i < 20; ++i) {
            build(PointerState{.position = {500.0f, 500.0f}}, 0.05f);
        }
        assert(!context.element_bounds(overflow_thumb_id).has_value());
    }


    /// Scrolls area thumb drag scrolls content using the supplied arguments and current state.
    ///
    /// @pre `thumb_before.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `child_b_bounds.has_value()`; debug builds assert if this precondition is violated.
    /// @pre `near(child_b_bounds->position.y, 20.0)`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void scroll_area_thumb_drag_scrolls_content() {
        Context context = make_context();
        ScrollAreaState state;
        const UString area_id{"scroll-area-drag"};
        const UString thumb_id = scroll_area_part_id(area_id, ScrollAreaVisualPart::VerticalThumb);
        const UString child_b_id{"sa-drag-b"};

        const auto build = [&](const PointerState &pointer, f32 dt) {
            context.begin_layout({200.0f, 200.0f}, pointer, dt);
            (void)scroll_area(
                context, area_id,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .direction = LayoutDirection::TopToBottom,
                    .clip = {.vertical = true},
                },
                ScrollbarStyle{}, state, dt, [&](Context &ctx) {
                    { auto a = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"sa-drag-a"}}); (void)a; }
                    { auto b = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = child_b_id}); (void)b; }
                });
            (void)context.finish_frame();
        };


        const PointerState hovering{.position = {50.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering, 0.05f);
        }
        const std::optional<ElementBounds> thumb_before = context.element_bounds(thumb_id);
        assert(thumb_before.has_value());
        const glm::vec2 thumb_center = thumb_before->position + thumb_before->size * 0.5f;


        build(PointerState{.position = thumb_center, .down = true, .pressed = true, .press_position = thumb_center}, 0.05f);
        const glm::vec2 drag_to{thumb_center.x, 95.0f};
        for (int i = 0; i < 5; ++i) {
            build(PointerState{.position = drag_to, .down = true}, 0.05f);
        }
        const std::optional<ElementBounds> child_b_bounds = context.element_bounds(child_b_id);
        assert(child_b_bounds.has_value());
        assert(near(child_b_bounds->position.y, 20.0));
    }


    /// Performs the text input cursor defaults and overrides operation using the supplied arguments.
    ///
    /// @pre `context.desired_cursor() == CursorIcon::Text`; debug builds assert if this precondition is violated.
    /// @pre `context.desired_cursor() == CursorIcon::NotAllowed`; debug builds assert if this precondition is violated.
    /// @pre `context.desired_cursor() == CursorIcon::Default`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void text_input_cursor_defaults_and_overrides() {
        Context context = make_context();
        TextEditState state;
        TextEditStyle style;
        const UString id{"cursor-text-input"};

        const auto build = [&](CursorIcon cursor, bool enabled) {
            context.begin_layout({200.0f, 80.0f}, PointerState{.position = {10.0f, 10.0f}});
            (void)text_input(
                context,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(160.0f), SizingAxis::fixed(32.0f)},
                    .cursor = cursor,
                    .id = id,
                },
                style, state, TextEditInput{}, 0.016f, UString{}, enabled);
            (void)context.finish_frame();
        };

        build(CursorIcon::Auto, true);
        build(CursorIcon::Auto, true);
        assert(context.desired_cursor() == CursorIcon::Text);

        build(CursorIcon::Auto, false);
        assert(context.desired_cursor() == CursorIcon::NotAllowed);


        build(CursorIcon::Default, true);
        assert(context.desired_cursor() == CursorIcon::Default);
        build(CursorIcon::ResizeHorizontal, true);
        assert(context.desired_cursor() == CursorIcon::ResizeHorizontal);
    }

    /// Performs the desired cursor resolves hover and specificity operation using the supplied arguments.
    ///
    /// @pre `context.desired_cursor() == CursorIcon::Default`; debug builds assert if this precondition is violated.
    /// @pre `context.desired_cursor() == CursorIcon::ResizeHorizontal`; debug builds assert if this precondition is violated.
    /// @pre `context.desired_cursor() == CursorIcon::Pointer`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void desired_cursor_resolves_hover_and_specificity() {
        Context context = make_context();
        const UString parent_id{"cursor-parent"};
        const UString child_id{"cursor-child"};
        const UString plain_id{"cursor-plain"};

        const auto build = [&](const PointerState &pointer) {
            context.begin_layout({200.0f, 200.0f}, pointer);
            {


                auto parent = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(100.0f)},
                    .cursor = CursorIcon::ResizeHorizontal,
                    .id = parent_id,
                });
                (void)parent;
                {
                    auto child = context.element(ElementDecl{
                        .sizing = {SizingAxis::fixed(30.0f), SizingAxis::fixed(30.0f)},
                        .cursor = CursorIcon::Pointer,
                        .id = child_id,
                    });
                    (void)child;
                }
            }
            {
                auto plain = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(20.0f), SizingAxis::fixed(20.0f)},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Root,
                        .element_attach_point = FloatingAttachPoint::LeftTop,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {150.0f, 0.0f},
                    },
                    .cursor = CursorIcon::Text,
                    .id = plain_id,
                });
                (void)plain;
            }
            (void)context.finish_frame();
        };

        build(PointerState{});


        assert(context.desired_cursor() == CursorIcon::Default);


        build(PointerState{.position = {70.0f, 70.0f}});
        assert(context.desired_cursor() == CursorIcon::ResizeHorizontal);


        build(PointerState{.position = {10.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Pointer);


        build(PointerState{.position = {160.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Text);


        context.set_cursor_management_enabled(false);
        assert(!context.cursor_management_enabled());
        build(PointerState{.position = {10.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Default);
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    slider_drag_and_keyboard();
    color_picker_preserves_achromatic_hue();
    color_picker_selects_foundation_color_space();
    color_picker_component_sliders_follow_selected_space();
    text_caret_has_no_layout_footprint();
    text_area_line_heights_ignore_caret();
    text_edit_state_click_streak_and_word_select();
    document_text_area_virtualizes_large_documents();
    text_edit_features_and_rebinding();
    register_font_stores_fallback_fonts_with_stable_addresses();
    outline_cache_key_distinguishes_font_faces_with_shared_low_bits();
    ime_composition_swallows_enter_and_escape();
    ime_composition_text_is_visible_but_not_inserted();
    text_input_shift_click_extends_selection();
    text_input_drag_acquires_and_releases_pointer_capture();
    scroll_container_moves_child_offset();
    floating_attached_parent_clips_to_ancestor();
    clicked_respects_ancestor_clip();
    scroll_metrics_and_set_scroll_offset_work();
    scroll_area_shows_thumb_only_when_hovering_overflowing_content();
    scroll_area_thumb_drag_scrolls_content();
    text_input_cursor_defaults_and_overrides();
    desired_cursor_resolves_hover_and_specificity();


    std::printf("UIWidgetTest: all checks passed.\n");
    return 0;
}
