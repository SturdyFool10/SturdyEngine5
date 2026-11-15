// Per-edge borders (CSS `border-left-color`, `border-top-style`, ...), CPU side: a border whose edges differ becomes one border-shader quad
// per solid edge (with only that edge's width set, in that edge's colour) and straight segment quads for dashed, dotted and double edges.

#include <Renderer/UI/UI.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT;
    using SFT::UI::BorderEdge;
    using SFT::UI::BorderLineStyle;
    using SFT::UI::Color;

    UI::FrameSnapshot layout(UI::Context &ui, std::shared_ptr<const UI::BorderEdges> edges, u16 width = 4) {
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        {
            auto root = ui.element(UI::ElementDecl{});
            auto box = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(100.0f), UI::SizingAxis::fixed(50.0f)},
                                                  .border = UI::BorderStyle{.color = Color{1.0, 1.0, 1.0, 1.0},
                                                                            .width = UI::BorderWidth{width, width, width, width}},
                                                  .border_edges = std::move(edges)});
        }
        return ui.finish_frame();
    }

    usize count_with_fill(const UI::FrameSnapshot &snapshot, f32 r, f32 g, f32 b) {
        usize n = 0;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const glm::vec4 &c = quad.instance.fill_color;
            n += std::abs(c.r - r) < 0.05f && std::abs(c.g - g) < 0.05f && std::abs(c.b - b) < 0.05f && c.a > 0.5f;
        }
        return n;
    }

} // namespace

int main() {
    UI::Context::Config config{};
    config.load_default_font = false;
    auto created = UI::Context::create(config);
    assert(created.has_value());
    UI::Context &ui = *created;

    // Four solid edges in four colours: four border quads, each with exactly one non-zero width.
    {
        auto edges = std::make_shared<UI::BorderEdges>();
        edges->left = BorderEdge{Color{1.0, 0.0, 0.0, 1.0}, BorderLineStyle::Solid};
        edges->right = BorderEdge{Color{0.0, 1.0, 0.0, 1.0}, BorderLineStyle::Solid};
        edges->top = BorderEdge{Color{0.0, 0.0, 1.0, 1.0}, BorderLineStyle::Solid};
        edges->bottom = BorderEdge{Color{1.0, 1.0, 0.0, 1.0}, BorderLineStyle::Solid};
        const UI::FrameSnapshot snapshot = layout(ui, edges);
        usize bordered = 0;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const glm::vec4 &w = quad.instance.border_width;
            const int nonzero = (w.x > 0.0f) + (w.y > 0.0f) + (w.z > 0.0f) + (w.w > 0.0f);
            if (nonzero > 0) {
                assert(nonzero == 1);
                ++bordered;
            }
        }
        assert(bordered == 4);
        // The left edge's quad carries the left width and the red colour (linear red stays 1 in the red channel).
        bool found_left_red = false;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const auto &i = quad.instance;
            found_left_red = found_left_red || (i.border_width.x > 0.0f && i.border_color.r > 0.9f && i.border_color.g < 0.1f);
        }
        assert(found_left_red);
    }

    // A transparent edge draws nothing.
    {
        auto edges = std::make_shared<UI::BorderEdges>();
        edges->left = BorderEdge{Color{1.0, 0.0, 0.0, 1.0}, BorderLineStyle::Solid};
        const UI::FrameSnapshot snapshot = layout(ui, edges);
        usize bordered = 0;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const glm::vec4 &w = quad.instance.border_width;
            bordered += (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f);
        }
        assert(bordered == 1);
    }

    // Dashed top edge (100 px wide, 4 px thick: 12 px dashes with 8 px gaps -> 5 dashes), dotted bottom, double right.
    {
        auto edges = std::make_shared<UI::BorderEdges>();
        edges->top = BorderEdge{Color{0.0, 0.0, 1.0, 1.0}, BorderLineStyle::Dashed};
        edges->bottom = BorderEdge{Color{0.0, 1.0, 0.0, 1.0}, BorderLineStyle::Dotted};
        edges->right = BorderEdge{Color{1.0, 0.0, 0.0, 1.0}, BorderLineStyle::Double};
        const UI::FrameSnapshot snapshot = layout(ui, edges);
        assert(count_with_fill(snapshot, 0.0f, 0.0f, 1.0f) == 5);
        assert(count_with_fill(snapshot, 0.0f, 1.0f, 0.0f) == 13); // dots 4 px wide every 8 px along 100 px: 0, 8, ..., 96
        assert(count_with_fill(snapshot, 1.0f, 0.0f, 0.0f) == 2);
    }

    // No per-edge description: the ordinary single border, as before.
    {
        const UI::FrameSnapshot snapshot = layout(ui, nullptr);
        usize bordered = 0;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const glm::vec4 &w = quad.instance.border_width;
            bordered += (w.x > 0.0f && w.y > 0.0f && w.z > 0.0f && w.w > 0.0f);
        }
        assert(bordered == 1);
    }

    ui.destroy();
    SFT::Foundation::log_info("UiBorderEdgesTest: ok");
    return 0;
}
