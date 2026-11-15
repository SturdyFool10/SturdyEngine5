// Hit testing through a 2D transform (`Effects::transform`): hover and click follow where the element is drawn, not where it was laid out.

#include <Renderer/UI/UI.hpp>

#include <cstdio>
#include <array>
#include <optional>
#include <cstdlib>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT;

    /// Lays a 100x50 element "btn" out twice (hover reads the previous layout) with the pointer at `pointer`; returns hovered("btn").
    bool hovered_at(UI::Context &ui, const std::shared_ptr<const UI::UiEffect> &effect, glm::vec2 pointer) {
        bool result = false;
        for (int frame = 0; frame < 2; ++frame) {
            UI::PointerState state;
            state.position = pointer;
            ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
            {
                auto root = ui.element(UI::ElementDecl{});
                auto button = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                         .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                                         .id = UString{"btn"},
                                                         .effect = effect});
                auto child = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(10.0f), UI::SizingAxis::fixed(10.0f)},
                                                        .id = UString{"inner"}});
                result = ui.hovered(UString{"btn"});
            }
            (void)ui.finish_frame();
        }
        return result;
    }

    bool inner_hovered_at(UI::Context &ui, const std::shared_ptr<const UI::UiEffect> &effect, glm::vec2 pointer) {
        bool result = false;
        for (int frame = 0; frame < 2; ++frame) {
            UI::PointerState state;
            state.position = pointer;
            ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
            {
                auto root = ui.element(UI::ElementDecl{});
                auto button = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                         .id = UString{"btn"},
                                                         .effect = effect});
                auto child = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(10.0f), UI::SizingAxis::fixed(10.0f)},
                                                        .id = UString{"inner"}});
                result = ui.hovered(UString{"inner"});
            }
            (void)ui.finish_frame();
        }
        return result;
    }

} // namespace

int main() {
    UI::Context::Config config{};
    config.load_default_font = false;
    auto created = UI::Context::create(config);
    assert(created.has_value());
    UI::Context &ui = *created;

    // No transform: the ordinary test.
    assert(hovered_at(ui, nullptr, {50.0f, 25.0f}));
    assert(!hovered_at(ui, nullptr, {250.0f, 25.0f}));

    // Moved 200 px right: it reacts at its new place and no longer at its old one.
    const auto moved = UI::Effects::transform({200.0f, 0.0f});
    assert(hovered_at(ui, moved, {250.0f, 25.0f}));
    assert(!hovered_at(ui, moved, {50.0f, 25.0f}));

    // A quarter turn about the centre (50, 25): the box now covers x 25..75, y -25..75.
    const auto rotated = UI::Effects::transform({0.0f, 0.0f}, 1.57079632679f);
    assert(hovered_at(ui, rotated, {50.0f, -10.0f}));
    assert(hovered_at(ui, rotated, {50.0f, 70.0f}));
    assert(!hovered_at(ui, rotated, {5.0f, 25.0f}));
    assert(!hovered_at(ui, rotated, {95.0f, 25.0f}));

    // Scaled to double size about the centre: it covers x -50..150, y -25..75.
    const auto doubled = UI::Effects::transform({0.0f, 0.0f}, 0.0f, {2.0f, 2.0f});
    assert(hovered_at(ui, doubled, {-30.0f, 25.0f}));
    assert(!hovered_at(ui, doubled, {160.0f, 25.0f}));

    // A child follows its parent's transform: the 10x10 "inner" at (0, 0) of the moved button is at (200, 0) now.
    assert(inner_hovered_at(ui, moved, {205.0f, 5.0f}));
    assert(!inner_hovered_at(ui, moved, {5.0f, 5.0f}));

    // ---- Clip-path cuts the reactive area too (and its children's). ----
    const auto circle = UI::Effects::clip_circle(20.0f, {0.5f, 0.5f}); // centred at (50, 25), 20 px radius
    assert(hovered_at(ui, circle, {50.0f, 25.0f}));
    assert(!hovered_at(ui, circle, {5.0f, 5.0f}));    // inside the box, outside the circle
    assert(!hovered_at(ui, circle, {90.0f, 25.0f}));  // 40 px from the centre
    // The 10x10 child sits at the box's top-left corner, outside the circle: it is cut away with it.
    assert(!inner_hovered_at(ui, circle, {5.0f, 5.0f}));
    const auto triangle_points = std::array<glm::vec2, 3>{glm::vec2{0.5f, 0.0f}, glm::vec2{1.0f, 1.0f}, glm::vec2{0.0f, 1.0f}};
    const auto triangle = UI::Effects::clip_polygon(triangle_points);
    assert(hovered_at(ui, triangle, {50.0f, 40.0f}));  // low and central: inside
    assert(!hovered_at(ui, triangle, {5.0f, 5.0f}));   // the cut-away top-left
    // A clip-path under a transform: the shape moves with the element.
    {
        // Both effects cannot sit on one element, so nest: an outer moved wrapper holds a clipped box.
        bool inside = false;
        bool outside = false;
        for (int frame = 0; frame < 2; ++frame) {
            for (const bool probe_inside : {true, false}) {
                UI::PointerState state;
                state.position = probe_inside ? glm::vec2{250.0f, 25.0f} : glm::vec2{205.0f, 5.0f};
                ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
                bool answer = false;
                {
                    auto root = ui.element(UI::ElementDecl{});
                    auto wrapper = ui.element(UI::ElementDecl{.effect = UI::Effects::transform({200.0f, 0.0f})});
                    auto clipped = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                              .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                                              .id = UString{"clipped"},
                                                              .effect = circle});
                    answer = ui.hovered(UString{"clipped"});
                }
                (void)ui.finish_frame();
                (probe_inside ? inside : outside) = answer;
            }
        }
        assert(inside);    // the circle's centre, moved 200 px right
        assert(!outside);  // the old corner area, also moved: outside the circle
    }

    // ---- Exact ancestry: a sibling that merely overlaps a transformed element is not transformed with it. ----
    {
        bool sibling_hit = false;
        for (int frame = 0; frame < 2; ++frame) {
            UI::PointerState state;
            state.position = glm::vec2{5.0f, 5.0f};
            ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
            {
                auto root = ui.element(UI::ElementDecl{.direction = UI::LayoutDirection::TopToBottom});
                {
                    auto moved_box = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                                 .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                                                 .effect = moved});
                }
                {
                    auto sibling = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                              .background_color = UI::Color{1.0, 0.0, 0.0, 1.0},
                                                              .id = UString{"sibling"}});
                    sibling_hit = ui.hovered(UString{"sibling"});
                }
            }
            (void)ui.finish_frame();
        }
        // The sibling is below the moved box at y 50..100, so (5, 5) is not over it; and a probe at its own place would be.
        assert(!sibling_hit);
    }

    // ---- Occlusion: an element drawn above, holding the pointer, covers one below it; a see-through one does not. ----
    {
        const auto covered_by = [&](bool cover_has_background) {
            bool result = false;
            for (int frame = 0; frame < 2; ++frame) {
                UI::PointerState state;
                state.position = glm::vec2{250.0f, 25.0f};
                ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
                {
                    auto root = ui.element(UI::ElementDecl{});
                    {
                        auto target = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                                 .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                                                 .id = UString{"target"},
                                                                 .effect = UI::Effects::transform({200.0f, 0.0f})});
                    }
                    {
                        // A floating panel on top of where the target was moved to.
                        auto cover = ui.element(UI::ElementDecl{
                            .sizing = {UI::SizingAxis::fixed(120.0f), UI::SizingAxis::fixed(60.0f)},
                            .background_color = cover_has_background ? UI::Color{0.0, 0.0, 1.0, 1.0} : UI::Color{0.0, 0.0, 0.0, 0.0},
                            .floating = UI::FloatingConfig{.attach_to = UI::FloatingAttachTo::Root,
                                                                   .element_attach_point = UI::FloatingAttachPoint::LeftTop,
                                                                   .parent_attach_point = UI::FloatingAttachPoint::LeftTop,
                                                                   .offset = {200.0f, 0.0f}}});
                    }
                    result = ui.hovered(UString{"target"});
                }
                (void)ui.finish_frame();
            }
            return result;
        };
        assert(covered_by(false));  // nothing visible on top: the target reacts
        assert(!covered_by(true));  // a solid panel on top covers it
    }

    // ---- Things that only draw (text, images) cover too. ----
    {
        UI::Context::Config text_config{};
        auto text_created = UI::Context::create(text_config);
        assert(text_created.has_value());
        UI::Context &tui = *text_created;
        enum class Cover { Text, Image, None };
        const auto covered_by = [&](Cover cover) {
            bool result = false;
            for (int frame = 0; frame < 2; ++frame) {
                UI::PointerState state;
                state.position = glm::vec2{210.0f, 10.0f};
                tui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
                {
                    auto root = tui.element(UI::ElementDecl{});
                    {
                        auto target = tui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                                  .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                                                  .id = UString{"target"},
                                                                  .effect = UI::Effects::transform({200.0f, 0.0f})});
                    }
                    const UI::FloatingConfig place{.attach_to = UI::FloatingAttachTo::Root,
                                                   .element_attach_point = UI::FloatingAttachPoint::LeftTop,
                                                   .parent_attach_point = UI::FloatingAttachPoint::LeftTop,
                                                   .offset = {200.0f, 0.0f}};
                    if (cover == Cover::Text) {
                        auto holder = tui.element(UI::ElementDecl{.floating = place});
                        tui.text(ustr{"COVER"}, UI::TextStyle{.font_size = 40});
                    } else if (cover == Cover::Image) {
                        tui.image(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(120.0f), UI::SizingAxis::fixed(60.0f)}, .floating = place},
                                  Renderer::TextureHandle{.value = 1});
                    }
                    result = tui.hovered(UString{"target"});
                }
                (void)tui.finish_frame();
            }
            return result;
        };
        assert(covered_by(Cover::None));
        assert(!covered_by(Cover::Text));   // text with no background still covers what is under it
        assert(!covered_by(Cover::Image));  // and so does an image
        tui.destroy();
    }

    // ---- Clipping to the element's own corners cuts the reactive area too. ----
    {
        bool corner_hovered = false;
        bool centre_hovered = false;
        for (int frame = 0; frame < 2; ++frame) {
            for (const bool centre : {false, true}) {
                UI::PointerState state;
                state.position = centre ? glm::vec2{50.0f, 25.0f} : glm::vec2{3.0f, 3.0f};
                ui.begin_layout(glm::vec2{400.0f, 300.0f}, state, 1.0f / 60.0f);
                bool answer = false;
                {
                    auto root = ui.element(UI::ElementDecl{});
                    UI::ElementDecl clipped{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                            .corner_radius = UI::CornerRadius::all(20.0f),
                                            .background_color = UI::Color{1.0, 1.0, 1.0, 1.0},
                                            .id = UString{"rounded"}};
                    clipped.clip.follow_corners = true;
                    auto element = ui.element(clipped);
                    answer = ui.hovered(UString{"rounded"});
                }
                (void)ui.finish_frame();
                (centre ? centre_hovered : corner_hovered) = answer;
            }
        }
        assert(centre_hovered);
        assert(!corner_hovered);
    }

    // ---- The toggle: Fixed is CSS plus reactive masks. ----
    {
        const auto fade = UI::Effects::mask_linear(0.0f, 1.0f, 0.0f); // opaque at the left edge, gone at the right
        // Css: the masked-out right end still reacts. Fixed: it does not, the visible left end still does.
        assert(hovered_at(ui, fade, {95.0f, 25.0f}));
        assert(hovered_at(ui, fade, {5.0f, 25.0f}));
        ui.set_hit_test_mode(UI::HitTestMode::Fixed);
        assert(ui.hit_test_mode() == UI::HitTestMode::Fixed);
        assert(!hovered_at(ui, fade, {95.0f, 25.0f}));
        assert(hovered_at(ui, fade, {5.0f, 25.0f}));
        // The rest of CSS's rules still hold in Fixed mode: transforms, clip-paths, covering.
        assert(hovered_at(ui, moved, {250.0f, 25.0f}));
        assert(!hovered_at(ui, moved, {50.0f, 25.0f}));
        assert(!hovered_at(ui, circle, {5.0f, 5.0f}));
        // An image mask reads the texture's pixels through the installed sampler: a 2x1 mask whose left half is clear and right half opaque.
        ui.set_mask_alpha_sampler([](Renderer::TextureHandle texture, glm::vec2 uv, bool) -> std::optional<f32> {
            if (texture.value != 42) return std::nullopt;
            return uv.x < 0.5f ? 0.0f : 1.0f;
        });
        Renderer::TextureHandle mask_texture;
        mask_texture.value = 42;
        const auto image_mask = UI::Effects::mask_image(mask_texture);
        assert(!hovered_at(ui, image_mask, {20.0f, 25.0f}));   // the clear half
        assert(hovered_at(ui, image_mask, {80.0f, 25.0f}));    // the opaque half
        const auto inverted = UI::Effects::mask_image(mask_texture, false, true);
        assert(hovered_at(ui, inverted, {20.0f, 25.0f}));
        assert(!hovered_at(ui, inverted, {80.0f, 25.0f}));
        // A texture the sampler knows nothing about hides nothing.
        Renderer::TextureHandle unknown;
        unknown.value = 7;
        assert(hovered_at(ui, UI::Effects::mask_image(unknown), {20.0f, 25.0f}));
        ui.set_hit_test_mode(UI::HitTestMode::Css);
        assert(hovered_at(ui, image_mask, {20.0f, 25.0f}));    // Css again: masks do not matter
    }

    ui.destroy();
    SFT::Foundation::log_info("UiTransformHitTestTest: ok");
    return 0;
}
