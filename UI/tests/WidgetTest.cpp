#include <UI/UI.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
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
        const PointerState hovering_and_scrolling{.position = {50.0f, 50.0f}, .scroll_delta = {0.0f, 60.0f}};
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        build(hovering_and_scrolling);
        const std::optional<ElementBounds> after = context.element_bounds(child_b_id);
        assert(after.has_value());

        std::fprintf(stderr, "DEBUG before.y=%f after.y=%f\n", before->position.y, after->position.y);

        // Scrolling down (positive Y delta, Clay's own convention) must move overflowing content
        // up relative to the clipped box — child-b's committed Y position must have decreased.
        assert(after->position.y < before->position.y - 1.0f);
    }

} // namespace

int main() {
    slider_drag_and_keyboard();
    color_picker_preserves_achromatic_hue();
    color_picker_selects_foundation_color_space();
    scroll_container_moves_child_offset();
    return 0;
}
