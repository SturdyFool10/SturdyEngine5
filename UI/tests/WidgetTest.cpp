#include <UI/UI.hpp>
#include <UI/src/UI/TextBridge.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace {

    [[nodiscard]] bool near(f64 lhs, f64 rhs, f64 tolerance = 1.0e-6) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    SFT::UI::Context make_context() {
        auto created = SFT::UI::Context::create(SFT::UI::Context::Config{});
        assert(created.has_value());
        return std::move(*created);
    }

    void slider_drag_and_keyboard() {
        using namespace SFT::UI;
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

    void color_picker_preserves_achromatic_hue() {
        using namespace SFT::UI;
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

    void color_picker_selects_foundation_color_space() {
        using namespace SFT::UI;
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

    // With the color-space dropdown enabled, the fixed hue bar gives way to per-channel sliders of
    // the selected space (sRGB by default: R/G/B) — the dropdown must actually change the picker's
    // controls, not just a text readout. Confirms the hue bar is genuinely absent, exactly three
    // component sliders exist, and dragging one (R to its track's right edge) drives the picked
    // color itself, round-tripped through the picker's internal HSV state.
    void color_picker_component_sliders_follow_selected_space() {
        using namespace SFT::UI;
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
        // Editing one channel must not disturb the others — the HSV round-trip is lossless in-gamut.
        assert(near(result.color.g, 0.4, 1.0e-6));
        assert(near(result.color.b, 0.8, 1.0e-6));
        (void)context.finish_frame();
    }

    // The caret is the editor's own indicator, not a physical glyph — it must occupy zero
    // horizontal space in the committed layout so text and placeholder never shift or wrap around
    // it (the visible 2px bar is a floating overlay centered on this zero-width anchor — see
    // Detail::render_line's emit_caret comment).
    void text_caret_has_no_layout_footprint() {
        using namespace SFT::UI;
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

    // Vertical counterpart of text_caret_has_no_layout_footprint(): an empty paragraph must be a
    // real, visible line whether or not the caret sits on it. Before the row strut, a blank line
    // rendered zero-height until the caret's anchor arrived and gave it height — a "line that
    // didn't exist" popping in and pushing everything below it down.
    void text_area_line_heights_ignore_caret() {
        using namespace SFT::UI;
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
        // Caret lands at the buffer's end (click-to-focus contract); walk it up through the empty
        // middle paragraph and onto the first one — the content height must never move.
        const std::array up{EditKey::Up};
        const f32 on_last = build(PointerState{});
        const f32 on_empty = build(PointerState{}, up);
        const f32 on_first = build(PointerState{}, up);
        assert(near(on_last, unfocused_height, 0.01));
        assert(near(on_empty, unfocused_height, 0.01));
        assert(near(on_first, unfocused_height, 0.01));
    }

    // Pure TextEditState-level coverage for the new click-classification/word/line-select helpers
    // — no Context/rendering involved, so it's unaffected by the fact these tests never register a
    // font (hit_test_text_byte_offset() always resolves to 0 without one; the widget-level tests
    // below work around that by asserting on selection *shape*, not on an exact hit-tested scalar).
    void text_edit_state_click_streak_and_word_select() {
        using namespace SFT::UI;
        TextEditState state;
        state.set_text(UString{"hello world foo"});

        const glm::vec2 pos{10.0f, 10.0f};
        assert(state.register_click(pos, /*allow_multi_click=*/true) == 1);
        assert(state.register_click(pos, /*allow_multi_click=*/true) == 2);
        assert(state.register_click(pos, /*allow_multi_click=*/true) == 3);
        assert(state.register_click(pos, /*allow_multi_click=*/true) == 3); // capped at triple-or-more

        // A click far enough away starts a fresh streak.
        const glm::vec2 far{200.0f, 10.0f};
        assert(state.register_click(far, /*allow_multi_click=*/true) == 1);
        assert(state.register_click(far, /*allow_multi_click=*/true) == 2);

        // A shift+click (allow_multi_click=false) always reports a fresh streak of 1, regardless of
        // whatever streak was already in progress — see register_click()'s own doc comment.
        assert(state.register_click(far, /*allow_multi_click=*/false) == 1);

        state.select_word_at(2); // inside "hello"
        assert(state.selected_text().cpp_string() == "hello");

        state.select_word_at(8); // inside "world"
        assert(state.selected_text().cpp_string() == "world");

        state.select_range(6, 11);
        assert(state.selected_text().cpp_string() == "world");
    }

    void document_text_area_virtualizes_large_documents() {
        using namespace SFT::UI;
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

    void text_edit_features_and_rebinding() {
        using namespace SFT::UI;
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

        // A disabled binding consumes its trigger rather than falling through to identity behavior.
        bindings.keys[0].enabled = false;
        state.set_caret_to(0, false);
        (void)state.apply_input(TextEditInput{.keys = {EditKey::Left}}, false, TextEditFeatures{}, bindings);
        assert(state.caret() == 0);
    }

    // Regression test for a real bug found via a live screenshot: CJK fallback glyphs rendered as
    // the *wrong* (garbled, not blank) shapes because Context::outline_cache_'s old key —
    // (font_id << 32) | glyph_id — discarded font_id's own high 32 bits when shifting. font_id is
    // itself a composite TextBridge::register_font stamps (low 32 bits = registered slot, high 32
    // = which face: primary=0, emoji=1, fallback=2,3,...), so two faces of the *same* registration
    // collided in the cache whenever glyph_id matched — a fallback-font glyph would silently reuse
    // whatever outline a same-numbered primary-font glyph had already cached.
    void outline_cache_key_distinguishes_font_faces_with_shared_low_bits() {
        using namespace SFT::UI;
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

    // Regression test for the register_font()/FontStack::fallbacks wiring added for CJK/IME glyph
    // coverage: fallback fonts must be stored with stable addresses (FontStack::fallbacks is a
    // non-owning span into TextBridge's own storage) and keep working after re-registering the same
    // font_id or after fonts_ reallocates internally (growing past its initial capacity).
    void register_font_stores_fallback_fonts_with_stable_addresses() {
        using namespace SFT::UI;
        using namespace SFT::Text;
        Font primary; // default-constructed (invalid/empty) — register_font never dereferences it
        Font fallback_a;
        Font fallback_b;
        TextBridge bridge;

        constexpr FontId font_id = 7;
        const std::array<const Font *, 2> fallbacks{&fallback_a, &fallback_b};
        bridge.register_font(font_id, primary, /*emoji_fallback=*/nullptr, fallbacks);

        const FontStack *stack = bridge.font_stack(font_id);
        assert(stack != nullptr);
        assert(stack->primary == &primary);
        assert(stack->fallbacks.size() == 2);
        assert(stack->fallbacks[0].font == &fallback_a);
        assert(stack->fallbacks[1].font == &fallback_b);
        // Every registered face (primary/emoji-slot/each fallback) must get a distinct font_id so
        // the glyph atlas never aliases two different fonts' glyph indices onto the same cache key.
        assert(stack->fallbacks[0].font_id != stack->fallbacks[1].font_id);
        assert(stack->fallbacks[0].font_id != stack->primary_font_id);
        assert(!stack->fallbacks[0].is_color && !stack->fallbacks[1].is_color);

        // Force fonts_ to reallocate internally by registering many other font_ids — this moves
        // every already-registered FontEntry (including font_id's own), which must not invalidate
        // the fallbacks span (see FontEntry::owned_fallbacks' own doc comment, TextBridge.hpp).
        for (FontId other = 100; other < 164; ++other) {
            bridge.register_font(other, primary);
        }
        const FontStack *after_growth = bridge.font_stack(font_id);
        assert(after_growth != nullptr);
        assert(after_growth->fallbacks.size() == 2);
        assert(after_growth->fallbacks[0].font == &fallback_a);
        assert(after_growth->fallbacks[1].font == &fallback_b);

        // Re-registering the same font_id with a *different* fallback list must replace, not append.
        const std::array<const Font *, 1> replacement_fallbacks{&fallback_b};
        bridge.register_font(font_id, primary, nullptr, replacement_fallbacks);
        const FontStack *after_replace = bridge.font_stack(font_id);
        assert(after_replace != nullptr);
        assert(after_replace->fallbacks.size() == 1);
        assert(after_replace->fallbacks[0].font == &fallback_b);
    }

    // While an IME composition is open, Enter/Escape belong to the IME (confirming/cancelling the
    // conversion), not the widget's own submit/unfocus behavior — most platforms never even forward
    // those keypresses to the app during composition, but TextEditState defends against whichever
    // backend/compositor combination does. See TextEditInput::composing's own doc comment.
    void ime_composition_swallows_enter_and_escape() {
        using namespace SFT::UI;
        TextEditState single_line;
        single_line.set_text(UString{"alpha"});
        single_line.set_focused(true);

        const TextEditState::ApplyResult swallowed_enter =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = true}, /*multiline=*/false);
        assert(!swallowed_enter.submitted);
        assert(single_line.focused()); // still focused — Escape below is the unfocus case, not Enter

        const TextEditState::ApplyResult swallowed_escape =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Escape}, .composing = true}, /*multiline=*/false);
        assert(!swallowed_escape.submitted);
        assert(single_line.focused()); // Escape must not unfocus while composing

        // The exact same keys, not composing, behave normally — proves the swallow is conditional on
        // `composing`, not a regression that disabled Enter/Escape outright.
        const TextEditState::ApplyResult normal_enter =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = false}, /*multiline=*/false);
        assert(normal_enter.submitted);
        assert(single_line.focused());

        const TextEditState::ApplyResult normal_escape =
            single_line.apply_input(TextEditInput{.keys = {EditKey::Escape}, .composing = false}, /*multiline=*/false);
        assert(!normal_escape.submitted);
        assert(!single_line.focused()); // Escape unfocuses when not composing

        // Multiline: Enter normally inserts '\n'; still swallowed while composing.
        TextEditState multiline;
        multiline.set_text(UString{"line"});
        multiline.set_focused(true);
        multiline.set_caret_to(multiline.text().size(), false);
        const TextEditState::ApplyResult swallowed_newline =
            multiline.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = true}, /*multiline=*/true);
        assert(!swallowed_newline.changed);
        assert(multiline.text().cpp_string() == "line");
        const TextEditState::ApplyResult normal_newline =
            multiline.apply_input(TextEditInput{.keys = {EditKey::Enter}, .composing = false}, /*multiline=*/true);
        assert(normal_newline.changed);
        assert(multiline.text().cpp_string() == "line\n");
    }

    // The composition (preedit) string is visible (TextEditState::composition_text(), which
    // render_line() paints underlined at the caret) but must never itself become part of the
    // committed buffer — only typed_text, arriving once the IME actually commits, does that. Also
    // covers that a stale composition never survives losing focus.
    void ime_composition_text_is_visible_but_not_inserted() {
        using namespace SFT::UI;
        TextEditState state;
        state.set_text(UString{"ab"});
        state.set_focused(true);
        state.set_caret_to(state.text().size(), false);

        (void)state.apply_input(TextEditInput{.composition_text = "konnichiwa", .composing = true}, /*multiline=*/false);
        assert(state.composition_text() == "konnichiwa");
        assert(state.text().cpp_string() == "ab"); // never inserted into the buffer

        // The IME commits: typed_text carries the final characters, composition_text clears.
        const TextEditState::ApplyResult committed = state.apply_input(
            TextEditInput{.typed_text = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf", .composition_text = "", .composing = false},
            /*multiline=*/false);
        assert(committed.changed);
        assert(state.composition_text().empty());
        assert(state.text().cpp_string() == "ab\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf");

        // A stale composition must not survive the field losing focus.
        (void)state.apply_input(TextEditInput{.composition_text = "leftover"}, /*multiline=*/false);
        assert(state.composition_text() == "leftover");
        state.set_focused(false);
        (void)state.apply_input(TextEditInput{.composition_text = "leftover"}, /*multiline=*/false);
        assert(state.composition_text().empty());
    }

    // Widget-level: shift+click must extend the existing caret position into a selection rather
    // than collapsing it (the pre-fix behavior). No font is registered in this test harness, so the
    // click's hit-tested scalar is always 0 (see this function group's own header comment) — that's
    // fine here since the point under test is that the *anchor* (the caret position before the
    // click) is preserved, not the exact resolved position.
    void text_input_shift_click_extends_selection() {
        using namespace SFT::UI;
        Context context = make_context();
        TextEditState state;
        state.set_text(UString{"hello world"});
        state.set_caret_to(5, /*extend=*/false); // caret at 5, no selection yet
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
        assert(state.selection_max() == 5); // the pre-click caret position, preserved as the anchor
        assert(state.selected_text().cpp_string() == "hello");
    }

    // Widget-level: a press acquires pointer capture (so a drag can continue tracking the pointer
    // even if it leaves the box's own bounds), held down keeps it, and release drops it — the same
    // capture/release contract ScrollArea.hpp's thumb drag already relies on.
    void text_input_drag_acquires_and_releases_pointer_capture() {
        using namespace SFT::UI;
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

    void scroll_container_moves_child_offset() {
        using namespace SFT::UI;
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

        // Scroll while the pointer hovers the box's own bounds (Clay only advances whichever
        // scroll container the pointer currently sits over) — feed the same delta across a few
        // frames so a one-frame settle/momentum quirk, if any, can't hide a real regression.
        const PointerState hovering_and_scrolling{.position = {50.0f, 50.0f}, .scroll_delta = {0.0f, -60.0f}};
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        const std::optional<ElementBounds> after = context.element_bounds(child_b_id);
        assert(after.has_value());

        // Scrolling down (positive Y delta, Clay's own convention) must move overflowing content
        // up relative to the clipped box — child-b's committed Y position must have decreased.
        assert(after->position.y < before->position.y - 1.0f);
    }

    // Regression test for a real bug: FloatingConfig had no way to express Clay's own
    // Clay_FloatingClipToElement (clay.h) at all — ContextImpl.cpp hardcoded CLAY_CLIP_TO_NONE, so
    // every floating element (a dropdown's arrow, a slider's thumb, a color picker's cursor, ...)
    // silently ignored any ancestor scroll container's clip rect and painted over content that
    // should have hidden it. FloatingClipTo::AttachedParent (Style.hpp) now threads through to
    // Clay's own attached-parent clipping — this proves the plumbing actually reaches a real
    // scissor rect on the resulting draw command, not just that it compiles.
    void floating_attached_parent_clips_to_ancestor() {
        using namespace SFT::UI;
        Context context = make_context();
        const UString clip_box_id{"clip-box"};
        const UString floater_id{"floater"};

        context.begin_layout({200.0f, 200.0f});
        {
            // A 50x50 clip container at the layout origin (the first, only element declared this
            // frame) — its committed bounding box is therefore exactly {0, 0, 50, 50}.
            auto box = context.element(ElementDecl{
                .sizing = {SizingAxis::fixed(50.0f), SizingAxis::fixed(50.0f)},
                .clip = {.vertical = true},
                .id = clip_box_id,
            });
            (void)box;
            // Offset it far outside the 50x50 box (and even the 200x200 viewport) — with clipping
            // respected, none of that geometry should survive into the emitted scissor rect.
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
            // The clip box itself never emits a RECTANGLE command (fully transparent background,
            // per Clay's own "backgroundColor.a > 0" gate) — the floater's 20x20 white quad is the
            // only draw command this scene produces, so size alone identifies it unambiguously.
            if (near(quad.instance.size.x, 20.0) && near(quad.instance.size.y, 20.0)) {
                floater_quad = &quad;
            }
        }
        assert(floater_quad != nullptr);
        // The floater's own bounding box sits far outside {0, 0, 50, 50}, so a correctly clipped
        // scissor rect must equal the clip box's bounds exactly, not the full 200x200 viewport.
        assert(floater_quad->scissor.x == 0);
        assert(floater_quad->scissor.y == 0);
        assert(floater_quad->scissor.width == 50);
        assert(floater_quad->scissor.height == 50);
    }

    // Regression test for a real bug: Context::clicked() reimplemented its own raw AABB-vs-point
    // test against element_bounds() (Clay's *unclipped* layout box) instead of reusing hovered()'s
    // Clay_PointerOver(), so it ignored both scissor clipping and layer/z ordering entirely — a
    // press landing inside an element's *layout* box registered as a click even when that element
    // was actually clipped away by an ancestor scroll container at that exact point. Confirms two
    // things in one scene: a press inside a clip box's visible window still clicks a floater sitting
    // there (no false negative from the fix), and a press inside a *different* floater's raw bounds,
    // but outside the clip box entirely, does not (the bug this test guards against).
    void clicked_respects_ancestor_clip() {
        using namespace SFT::UI;
        Context context = make_context();
        const UString clip_box_id{"click-clip-box"};
        const UString visible_floater_id{"click-floater-visible"};
        const UString hidden_floater_id{"click-floater-hidden"};

        const auto build = [&](const PointerState &pointer) {
            context.begin_layout({200.0f, 200.0f}, pointer);
            {
                // A 50x50 clip container at the layout origin — its committed bounding box is
                // exactly {0, 0, 50, 50}.
                auto box = context.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(50.0f), SizingAxis::fixed(50.0f)},
                    .clip = {.vertical = true},
                    .id = clip_box_id,
                });
                (void)box;
                // Inside the clip box's visible window: {10, 10}-{30, 30}.
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
                // Far below the clip box's own bounds, but still a perfectly valid layout box in
                // its own right (and still inside the 200x200 viewport) — only the clip should be
                // what excludes it.
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

    // Context::scroll_metrics()/set_scroll_offset() are ScrollArea.hpp's foundation — they wrap
    // Clay_GetScrollContainerData's own documented "external functionality that modifies scroll
    // position, such as scroll bars" use case (clay.h). Verifies both the read side (reports the
    // real content/container sizes and current offset) and the write side (clamps to the
    // container's valid range, same as wheel-driven scrolling would, and the new value is actually
    // picked up by the next frame's layout).
    void scroll_metrics_and_set_scroll_offset_work() {
        using namespace SFT::UI;
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

        // Ask for far more than the container can scroll — must clamp to -(content - container),
        // exactly what Clay's own wheel-driven clamping would produce (Clay_UpdateScrollContainers,
        // clay.h), not silently accept an out-of-range value.
        assert(context.set_scroll_offset(box_id, glm::vec2{0.0f, -1000.0f}));
        const Context::ScrollMetrics clamped = context.scroll_metrics(box_id);
        assert(near(clamped.offset.y, -60.0));

        // An id nothing declared with clip enabled is a no-op, not a crash.
        assert(!context.set_scroll_offset(UString{"does-not-exist"}, glm::vec2{0.0f, -10.0f}));

        build();
        const std::optional<ElementBounds> child_b_bounds = context.element_bounds(child_b_id);
        assert(child_b_bounds.has_value());
        assert(near(child_b_bounds->position.y, 20.0));
    }

    // Regression/behavior test for ScrollArea.hpp's core promise: the scrollbar stays invisible
    // (not even declared, let alone drawn) until the pointer actually enters a scrollable area, and
    // fades in shortly after — never a permanent fixture. Also confirms it does not appear at all
    // when there is nothing to scroll.
    void scroll_area_shows_thumb_only_when_hovering_overflowing_content() {
        using namespace SFT::UI;
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
                    // Braced so each scope closes immediately — an unbraced `auto x =
                    // ctx.element(...); (void)x;` stays open until the end of this lambda, which
                    // would make the second child a child of the first instead of its sibling (the
                    // same ElementScope-lifetime pitfall documented on WorkbenchUi.cpp's own
                    // dropdown_option()).
                    { auto a = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"sa-overflow-a"}}); (void)a; }
                    { auto b = ctx.element(ElementDecl{.sizing = {SizingAxis::fixed(80.0f), SizingAxis::fixed(80.0f)}, .id = UString{"sa-overflow-b"}}); (void)b; }
                });
            // A second scroll area, positioned well away from the first, whose content fits
            // entirely within its container — must never show a thumb regardless of hover.
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

        // Pointer parked well outside both areas — neither thumb should exist in the committed tree.
        build(PointerState{.position = {500.0f, 500.0f}}, 0.016f);
        assert(!context.element_bounds(overflow_thumb_id).has_value());
        assert(!context.element_bounds(fit_thumb_id).has_value());

        // Hover the overflowing area for long enough that fade_in_seconds (default 0.1s) completes.
        const PointerState hovering_overflow{.position = {50.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering_overflow, 0.05f);
        }
        assert(context.element_bounds(overflow_thumb_id).has_value());

        // Hovering the *fitting* area (nothing to scroll) must still never produce a thumb, even
        // after the same dwell time.
        const PointerState hovering_fit{.position = {200.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering_fit, 0.05f);
        }
        assert(!context.element_bounds(fit_thumb_id).has_value());

        // Move away and give it long enough to fade back out (default fade_out_seconds=0.4s,
        // idle_delay_seconds=0.4s) — it must not linger forever once the pointer leaves.
        for (int i = 0; i < 20; ++i) {
            build(PointerState{.position = {500.0f, 500.0f}}, 0.05f);
        }
        assert(!context.element_bounds(overflow_thumb_id).has_value());
    }

    // Confirms the thumb is actually draggable end-to-end: pressing it and moving the pointer must
    // scroll the underlying content via Context::set_scroll_offset(), not just animate the thumb's
    // own decorative position.
    void scroll_area_thumb_drag_scrolls_content() {
        using namespace SFT::UI;
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

        // Warm the thumb into existence (hover long enough to fully fade in) before trying to press
        // it — it isn't declared at all while invisible, so a press before this would hit nothing.
        const PointerState hovering{.position = {50.0f, 50.0f}};
        for (int i = 0; i < 10; ++i) {
            build(hovering, 0.05f);
        }
        const std::optional<ElementBounds> thumb_before = context.element_bounds(thumb_id);
        assert(thumb_before.has_value());
        const glm::vec2 thumb_center = thumb_before->position + thumb_before->size * 0.5f;

        // Press on the thumb, then drag it to the bottom of its track — content must scroll to
        // fully reveal child-b, the same end state the wheel-scroll test (further up this file)
        // reaches by an entirely different input path. `.down` stays true across every frame from
        // the press onward so only the *first* of these frames is seen as a new press edge
        // (pointer_down_ transitioning false->true) — otherwise each subsequent frame would look
        // like a fresh click too and keep recapturing/re-grabbing the thumb instead of dragging it.
        build(PointerState{.position = thumb_center, .down = true, .pressed = true, .press_position = thumb_center}, 0.05f);
        const glm::vec2 drag_to{thumb_center.x, 95.0f};
        for (int i = 0; i < 5; ++i) {
            build(PointerState{.position = drag_to, .down = true}, 0.05f);
        }
        const std::optional<ElementBounds> child_b_bounds = context.element_bounds(child_b_id);
        assert(child_b_bounds.has_value());
        assert(near(child_b_bounds->position.y, 20.0));
    }

    // Context::desired_cursor() — the whole feature's actual output — resolves to CursorIcon::
    // Default with nothing hovered, to a hovered element's own ElementDecl::cursor when one is set,
    // to the more specific of two nested hovered elements' cursors (the child, declared after its
    // parent, wins — see update_desired_cursor()'s own doc comment in ContextImpl.cpp for why
    // declaration order alone is enough to get that right), and back to Default for everything the
    // instant cursor management is disabled, "similar to how the web works" per the request this
    // feature backs (an app can still declare `cursor` everywhere; the app just stops acting on it).
    void desired_cursor_resolves_hover_and_specificity() {
        using namespace SFT::UI;
        Context context = make_context();
        const UString parent_id{"cursor-parent"};
        const UString child_id{"cursor-child"};
        const UString plain_id{"cursor-plain"};

        const auto build = [&](const PointerState &pointer) {
            context.begin_layout({200.0f, 200.0f}, pointer);
            {
                // A resize-cursor region (e.g. a dock divider) containing a pointer-cursor control
                // (e.g. a button) — the button, being more specific, must win while it's hovered.
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
        // Nothing hovered yet (this very first frame's pointer position was never tested against
        // anything, one-frame-stale like every other hover query here) — Default.
        assert(context.desired_cursor() == CursorIcon::Default);

        // Hover the parent, outside the child's own 30x30 box — the parent's own cursor applies.
        build(PointerState{.position = {70.0f, 70.0f}});
        assert(context.desired_cursor() == CursorIcon::ResizeHorizontal);

        // Hover squarely inside the child — more specific, wins over its ancestor.
        build(PointerState{.position = {10.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Pointer);

        // Hover the unrelated plain element — its own cursor, uncontaminated by the other subtree.
        build(PointerState{.position = {160.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Text);

        // Disabling cursor management must force Default even while still hovering a
        // cursor-declaring element — the global kill switch actually switches everything off.
        context.set_cursor_management_enabled(false);
        assert(!context.cursor_management_enabled());
        build(PointerState{.position = {10.0f, 10.0f}});
        assert(context.desired_cursor() == CursorIcon::Default);
    }

} // namespace

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
    desired_cursor_resolves_hover_and_specificity();
    // Every check above is a bare assert() — a failure never reaches this line (libc's assert
    // aborts with its own file:line diagnostic first). Printing on the success path is the only
    // way a "Run" task (.zed/tasks.json) shows any visible sign it did something, rather than
    // exiting 0 with a blank terminal that looks indistinguishable from not having run at all.
    std::printf("UIWidgetTest: all checks passed.\n");
    return 0;
}
