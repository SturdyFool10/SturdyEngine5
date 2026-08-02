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

} // namespace

int main() {
    slider_drag_and_keyboard();
    color_picker_preserves_achromatic_hue();
    color_picker_selects_foundation_color_space();
    scroll_container_moves_child_offset();
    floating_attached_parent_clips_to_ancestor();
    clicked_respects_ancestor_clip();
    // Every check above is a bare assert() — a failure never reaches this line (libc's assert
    // aborts with its own file:line diagnostic first). Printing on the success path is the only
    // way a "Run" task (.zed/tasks.json) shows any visible sign it did something, rather than
    // exiting 0 with a blank terminal that looks indistinguishable from not having run at all.
    std::printf("UIWidgetTest: all checks passed.\n");
    return 0;
}
