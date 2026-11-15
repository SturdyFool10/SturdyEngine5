// Edge-aware layout, CPU side: with `ElementDecl::inset_for_corners` the padding grows so a child's corner stays inside the element's rounded,
// bevelled, scooped or notched corners.

#include <Renderer/UI/UI.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT;
    using SFT::UI::CornerShape;

    bool near(f32 a, f32 b) { return std::abs(a - b) < 0.01f; }

    /// Where the 10x10 child of a 100x100 box with 20 px corners of `shape` and `padding` ends up (its top-left, from the box's).
    glm::vec2 child_position(UI::Context &ui, CornerShape shape, u16 padding, bool inset) {
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        {
            auto root = ui.element(UI::ElementDecl{});
            auto box = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(100.0f)},
                                                  .padding = UI::Padding::all(padding),
                                                  .corner_radius = UI::CornerRadius::all(20.0f),
                                                  .corner_shapes = UI::CornerShapes::all(shape),
                                                  .inset_for_corners = inset});
            auto child = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(10.0f), UI::SizingAxis::fixed(10.0f)},
                                                    .background_color = UI::Color{1.0, 0.0, 0.0, 1.0}});
        }
        const UI::FrameSnapshot snapshot = ui.finish_frame();
        assert(snapshot.quads().size() >= 1);
        return snapshot.quads().back().instance.position;
    }

} // namespace

int main() {
    UI::Context::Config config{};
    config.load_default_font = false;
    auto created = UI::Context::create(config);
    assert(created.has_value());
    UI::Context &ui = *created;

    assert(near(child_position(ui, CornerShape::round(), 0, false).x, 0.0f));            // off: no change
    assert(near(child_position(ui, CornerShape::round(), 0, true).x, 6.0f));             // 0.29 * 20 = 5.86 -> 6
    assert(near(child_position(ui, CornerShape::squircle(), 0, true).x, 4.0f));          // 0.159 * 20 = 3.2 -> 4
    assert(near(child_position(ui, CornerShape::bevel(), 0, true).x, 10.0f));            // half the radius
    assert(near(child_position(ui, CornerShape::square(), 0, true).x, 0.0f));            // a square corner needs nothing
    assert(near(child_position(ui, CornerShape::scoop(), 0, true).x, 15.0f));            // 0.707 * 20 = 14.1 -> 15
    assert(near(child_position(ui, CornerShape::notch(), 0, true).x, 20.0f));            // the whole radius
    assert(near(child_position(ui, CornerShape::round(), 12, true).x, 12.0f));           // declared padding is a floor
    assert(near(child_position(ui, CornerShape::round(), 12, true).y, 12.0f));

    ui.destroy();
    SFT::Foundation::log_info("UiCornerInsetTest: ok");
    return 0;
}
