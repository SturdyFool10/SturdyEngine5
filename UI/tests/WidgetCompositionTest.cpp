#include <UI/ColorPicker.hpp>
#include <UI/Dropdown.hpp>
#include <UI/Slider.hpp>
#include <UI/WidgetComposition.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    SFT::UI::Context make_context() {
        auto created = SFT::UI::Context::create(SFT::UI::Context::Config{});
        assert(created.has_value());
        return std::move(*created);
    }

    bool same_color(const SFT::UI::Color &lhs, const SFT::UI::Color &rhs) {
        return lhs == rhs;
    }

    void visual_patches_compose_in_documented_order() {
        using namespace SFT::UI;
        const Color generated{0.1, 0.2, 0.3, 1.0};
        const Color selected{0.2, 0.3, 0.4, 1.0};
        const Color hovered{0.3, 0.4, 0.5, 1.0};
        const Color disabled{0.4, 0.5, 0.6, 0.5};
        const BorderStyle focused_border{.color = Color{0.8, 0.7, 0.6, 1.0}, .width = BorderWidth::all(2)};

        ElementDecl decl{.background_color = generated, .corner_radius = CornerRadius::all(3.0f)};
        PartVisualStyle style;
        style.selected.background_color = selected;
        style.hovered.background_color = hovered;
        style.focused.border = focused_border;
        style.disabled.background_color = disabled;
        apply_part_visual(decl, style, PartVisualState{
                                           .enabled = false,
                                           .hovered = true,
                                           .focused = true,
                                           .selected = true,
                                       });
        assert(same_color(decl.background_color, disabled));
        assert(decl.border.width.left == 2);
        assert(decl.corner_radius.top_left == 3.0f);

        ElementDecl transparent{.background_color = generated};
        PartVisualStyle transparent_style;
        transparent_style.idle.background_color = Color{0.0, 0.0, 0.0, 0.0};
        apply_part_visual(transparent, transparent_style, PartVisualState{});
        assert(transparent.background_color.a == 0.0);
    }

    void legacy_overloads_remain_callable() {
        using namespace SFT::UI;
        Context context = make_context();
        SliderState slider_state;
        DropdownState dropdown_state;
        ColorPickerState picker_state;
        const std::array options{DropdownOption{.build = [](Context &) {}}};

        context.begin_layout({400.0f, 400.0f});
        const SliderResult slider_result = slider(
            context,
            ElementDecl{
                .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(20.0f)},
                .id = UString{"legacy-slider"},
            },
            SliderConfig{},
            SliderStyle{},
            slider_state,
            25.0);
        const DropdownResult dropdown_result = dropdown(
            context,
            UString{"legacy-dropdown"},
            ElementDecl{.sizing = {SizingAxis::fixed(120.0f), SizingAxis::fixed(24.0f)}},
            DropdownStyle{.show_arrow_indicator = false},
            dropdown_state,
            0.0f,
            0,
            options);
        const ColorPickerResult picker_result = color_picker(
            context,
            UString{"legacy-picker"},
            ElementDecl{},
            ColorPickerConfig{.show_color_space_dropdown = false, .show_alpha = false, .show_preview = false},
            ColorPickerStyle{.plane_size = {60.0f, 40.0f}},
            picker_state,
            Color{0.2, 0.4, 0.6, 1.0});
        assert(slider_result.value == 25.0);
        assert(dropdown_result.selected_index == 0);
        assert(picker_result.color.a == 1.0);
        (void)context.finish_frame();
    }

    void slider_parts_have_stable_ids_hooks_and_builders() {
        using namespace SFT::UI;
        Context context = make_context();
        SliderState state;
        const std::array ticks{SliderTick{.value = 25.0}};
        const SliderConfig config{
            .min = 0.0,
            .max = 100.0,
            .step = 50.0,
            .ticks = ticks,
            .show_step_ticks = true,
        };
        SliderComposition composition;
        usize track_builds = 0;
        usize marker_builds = 0;
        usize generated_markers = 0;
        usize marker_labels = 0;
        usize labels = 0;
        usize tooltips = 0;
        composition.track.build = [&](Context &, const SliderPartContext &part) {
            ++track_builds;
            assert(part.id == slider_part_id(UString{"composed-slider"}, SliderVisualPart::Track));
        };
        composition.thumb.alter_decl = [](ElementDecl &decl, const SliderPartContext &) {
            decl.sizing = {SizingAxis::fixed(24.0f), SizingAxis::fixed(24.0f)};
        };
        composition.marker.build = [&](Context &, const SliderPartContext &part) {
            ++marker_builds;
            generated_markers += part.generated_marker ? 1u : 0u;
            assert(part.marker_value.has_value());
        };
        composition.marker_label.visible = true;
        composition.marker_label.build = [&](Context &, const SliderPartContext &part) {
            ++marker_labels;
            assert(part.part == SliderVisualPart::MarkerLabel);
        };
        composition.label.visible = true;
        composition.label.build = [&](Context &, const SliderPartContext &part) {
            ++labels;
            assert(part.part == SliderVisualPart::Label);
        };
        composition.tooltip.visible = true;
        composition.tooltip.build = [&](Context &, const SliderPartContext &) { ++tooltips; };

        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(30.0f)},
            .id = UString{"composed-slider"},
        };
        context.begin_layout({240.0f, 120.0f});
        SliderResult result = slider(context, decl, config, SliderStyle{}, state, 50.0, SliderInput{}, true, composition);
        assert(result.value == 50.0);
        (void)context.finish_frame();
        assert(track_builds == 1);
        assert(marker_builds == 4);
        assert(generated_markers == 3);
        assert(marker_labels == marker_builds);
        assert(labels == 1);
        assert(tooltips == 0);

        const auto thumb_bounds = context.element_bounds(slider_part_id(decl.id, SliderVisualPart::Thumb));
        assert(thumb_bounds.has_value());
        assert(thumb_bounds->size.x == 24.0f);
        assert(context.element_bounds(slider_part_id(decl.id, SliderVisualPart::Marker, 0)).has_value());

        context.begin_layout({240.0f, 120.0f}, PointerState{.position = thumb_bounds->position + thumb_bounds->size * 0.5f});
        result = slider(context, decl, config, SliderStyle{}, state, result.value, SliderInput{}, true, composition);
        (void)context.finish_frame();
        assert(result.hovered);
        assert(tooltips == 1);
    }

    void slider_track_can_be_disabled_without_hiding_it() {
        using namespace SFT::UI;
        Context context = make_context();
        SliderState state;
        SliderComposition composition;
        composition.track.enabled = false;
        const ElementDecl decl{
            .sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(20.0f)},
            .id = UString{"disabled-track-slider"},
        };

        context.begin_layout({200.0f, 80.0f});
        SliderResult result = slider(context, decl, SliderConfig{}, SliderStyle{}, state, 0.0, SliderInput{}, true, composition);
        (void)context.finish_frame();
        const auto bounds = context.element_bounds(decl.id);
        assert(bounds.has_value());
        assert(context.element_bounds(slider_part_id(decl.id, SliderVisualPart::Track)).has_value());

        const glm::vec2 center = bounds->position + bounds->size * 0.5f;
        context.begin_layout({200.0f, 80.0f}, PointerState{
                                                  .position = center,
                                                  .pressed = true,
                                                  .press_position = center,
                                                  .released = true,
                                              });
        result = slider(context, decl, SliderConfig{}, SliderStyle{}, state, result.value, SliderInput{}, true, composition);
        assert(!result.changed);
        assert(result.value == 0.0);
        (void)context.finish_frame();
    }

    void color_picker_parts_can_be_replaced_hidden_and_labeled() {
        using namespace SFT::UI;
        Context context = make_context();
        ColorPickerState state;
        ColorPickerComposition composition;
        composition.dropdown.visible = false;
        composition.hue.visible = false;
        composition.alpha.visible = false;
        composition.preview.visible = false;
        composition.saturation_value.render_default = false;
        composition.label.visible = true;

        usize plane_builds = 0;
        usize marker_builds = 0;
        usize label_builds = 0;
        composition.saturation_value.build = [&](Context &ctx, const ColorPickerPartContext &part) {
            ++plane_builds;
            assert(part.part == ColorPickerVisualPart::SaturationValue);
            auto custom = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(10.0f), SizingAxis::fixed(10.0f)},
                .id = UString{"custom-plane-child"},
            });
            (void)custom;
        };
        composition.saturation_value_marker.build = [&](Context &, const ColorPickerPartContext &part) {
            ++marker_builds;
            assert(part.normalized_position.x >= 0.0f && part.normalized_position.x <= 1.0f);
        };
        composition.label.build = [&](Context &, const ColorPickerPartContext &part) {
            ++label_builds;
            assert(part.target == ColorPickerVisualPart::SaturationValue);
        };

        ColorPickerStyle style;
        style.plane_size = {80.0f, 60.0f};
        const UString id{"composed-picker"};
        context.begin_layout({200.0f, 160.0f});
        const ColorPickerResult result = color_picker(
            context,
            id,
            ElementDecl{},
            ColorPickerConfig{.show_color_space_dropdown = true, .show_alpha = true, .show_preview = true},
            style,
            state,
            Color{0.2, 0.4, 0.8, 0.75},
            ColorPickerInput{},
            true,
            composition);
        assert(result.color.a == 0.75);
        (void)context.finish_frame();

        assert(plane_builds == 1);
        assert(marker_builds == 1);
        assert(label_builds == 1);
        assert(context.element_bounds(color_picker_part_id(id, ColorPickerVisualPart::SaturationValue)).has_value());
        assert(context.element_bounds(color_picker_part_id(id, ColorPickerVisualPart::SaturationValueMarker)).has_value());
        assert(context.element_bounds(UString{"custom-plane-child"}).has_value());
        assert(!context.element_bounds(color_picker_part_id(id, ColorPickerVisualPart::Hue)).has_value());
        assert(!context.element_bounds(color_picker_part_id(id, ColorPickerVisualPart::Alpha)).has_value());
        assert(!context.element_bounds(color_picker_part_id(id, ColorPickerVisualPart::Preview)).has_value());
    }

    void dropdown_composition_reports_states_and_blocks_disabled_options() {
        using namespace SFT::UI;
        Context context = make_context();
        DropdownState state;
        state.open = true;
        usize legacy_builds = 0;
        const auto build_option = [&](Context &ctx) {
            ++legacy_builds;
            auto content = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(40.0f), SizingAxis::fixed(10.0f)},
            });
            (void)content;
        };
        std::array options{
            DropdownOption{.build = build_option},
            DropdownOption{.build = build_option},
            DropdownOption{.build = build_option},
        };
        DropdownComposition composition;
        composition.option_enabled = [](usize index) { return index != 1; };
        composition.header.visible = true;
        composition.footer.visible = true;
        composition.tooltip.visible = true;
        usize header_builds = 0;
        usize footer_builds = 0;
        usize option_builds = 0;
        usize disabled_options = 0;
        usize selected_options = 0;
        usize indicator_builds = 0;
        usize tooltip_builds = 0;
        composition.header.build = [&](Context &, const DropdownPartContext &) { ++header_builds; };
        composition.footer.build = [&](Context &, const DropdownPartContext &) { ++footer_builds; };
        composition.option.build = [&](Context &, const DropdownPartContext &part) {
            ++option_builds;
            disabled_options += part.visual.enabled ? 0u : 1u;
            selected_options += part.visual.selected ? 1u : 0u;
        };
        composition.indicator.render_default = false;
        composition.indicator.build = [&](Context &, const DropdownPartContext &) { ++indicator_builds; };
        composition.tooltip.build = [&](Context &, const DropdownPartContext &part) {
            ++tooltip_builds;
            assert(part.option_index.has_value());
        };

        const UString id{"composed-dropdown"};
        const ElementDecl trigger_decl{
            .sizing = {SizingAxis::fixed(120.0f), SizingAxis::fixed(24.0f)},
            .id = id,
        };
        DropdownStyle style;
        style.show_arrow_indicator = false;

        context.begin_layout({300.0f, 240.0f});
        DropdownResult result = dropdown(context, id, trigger_decl, style, state, 0.0f, 0, options, true, composition);
        (void)context.finish_frame();
        assert(!result.changed);
        assert(header_builds == 1 && footer_builds == 1);
        assert(option_builds == 3 && disabled_options == 1 && selected_options == 1);
        assert(indicator_builds == 1);
        assert(legacy_builds == 4);

        const auto enabled_bounds = context.element_bounds(dropdown_part_id(id, DropdownVisualPart::Option, 2));
        const auto disabled_bounds = context.element_bounds(dropdown_part_id(id, DropdownVisualPart::Option, 1));
        assert(enabled_bounds.has_value() && disabled_bounds.has_value());

        const glm::vec2 enabled_center = enabled_bounds->position + enabled_bounds->size * 0.5f;
        context.begin_layout({300.0f, 240.0f}, PointerState{.position = enabled_center});
        result = dropdown(context, id, trigger_decl, style, state, 0.0f, 0, options, true, composition);
        (void)context.finish_frame();
        assert(!result.changed);
        assert(tooltip_builds == 1);

        const auto current_disabled_bounds = context.element_bounds(
            dropdown_part_id(id, DropdownVisualPart::Option, 1));
        assert(current_disabled_bounds.has_value());
        const glm::vec2 disabled_center = current_disabled_bounds->position + current_disabled_bounds->size * 0.5f;
        context.begin_layout({300.0f, 240.0f}, PointerState{
                                                   .position = disabled_center,
                                                   .pressed = true,
                                                   .press_position = disabled_center,
                                                   .released = true,
                                               });
        result = dropdown(context, id, trigger_decl, style, state, 0.0f, 0, options, true, composition);
        assert(!result.changed);
        assert(state.open);
        (void)context.finish_frame();

        context.begin_layout({300.0f, 240.0f}, PointerState{
                                                   .position = enabled_center,
                                                   .pressed = true,
                                                   .press_position = enabled_center,
                                                   .released = true,
                                               });
        result = dropdown(context, id, trigger_decl, style, state, 0.0f, 0, options, true, composition);
        assert(result.changed);
        assert(result.selected_index == 2);
        assert(!state.open);
        (void)context.finish_frame();
    }

    void dropdown_empty_slot_builds() {
        using namespace SFT::UI;
        Context context = make_context();
        DropdownState state;
        state.open = true;
        DropdownComposition composition;
        composition.empty.visible = true;
        usize empty_builds = 0;
        composition.empty.build = [&](Context &, const DropdownPartContext &) { ++empty_builds; };
        const std::span<const DropdownOption> no_options{};

        context.begin_layout({200.0f, 120.0f});
        (void)dropdown(context, UString{"empty-dropdown"}, ElementDecl{.sizing = {SizingAxis::fixed(100.0f), SizingAxis::fixed(20.0f)}}, DropdownStyle{.show_arrow_indicator = false}, state, 0.0f, 0, no_options, true, composition);
        (void)context.finish_frame();
        assert(empty_builds == 1);
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    visual_patches_compose_in_documented_order();
    legacy_overloads_remain_callable();
    slider_parts_have_stable_ids_hooks_and_builders();
    slider_track_can_be_disabled_without_hiding_it();
    color_picker_parts_can_be_replaced_hidden_and_labeled();
    dropdown_composition_reports_states_and_blocks_disabled_options();
    dropdown_empty_slot_builds();


    std::printf("UIWidgetCompositionTest: all checks passed.\n");
    return 0;
}
