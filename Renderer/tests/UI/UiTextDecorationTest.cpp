// Text decoration (underline, overline, line-through, styles) and hard text shadow, CPU side: they become extra quads and a second glyph run.

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

    UI::FrameSnapshot layout(UI::Context &ui, const UI::TextStyle &style) {
        ui.begin_layout(glm::vec2{400.0f, 300.0f}, UI::PointerState{}, 1.0f / 60.0f);
        {
            auto root = ui.element(UI::ElementDecl{});
            ui.text(ustr{"Hello"}, style);
        }
        return ui.finish_frame();
    }

} // namespace

int main() {
    UI::Context::Config config{};
    auto created = UI::Context::create(config);
    assert(created.has_value());
    UI::Context &ui = *created;

    UI::TextStyle plain;
    plain.font_size = 20;
    const usize base_glyphs = layout(ui, plain).glyphs().size();
    assert(base_glyphs > 0);
    assert(layout(ui, plain).quads().empty());

    // Underline: one solid quad as wide as the text, 1-2 px thick, in the text's colour.
    UI::TextStyle underlined = plain;
    underlined.decoration.lines = UI::TextDecorationLine::Underline;
    {
        const UI::FrameSnapshot snapshot = layout(ui, underlined);
        assert(snapshot.glyphs().size() == base_glyphs);
        assert(snapshot.quads().size() == 1);
        const UI::UiQuadInstance &line = snapshot.quads()[0].instance;
        assert(line.size.x > 20.0f && line.size.y >= 1.0f && line.size.y <= 3.0f);
        assert(line.fill_color.a > 0.9f);
    }

    // Underline + line-through + overline in a colour of their own, double style: three double lines = six quads.
    UI::TextStyle all = plain;
    all.decoration.lines = UI::TextDecorationLine::Underline | UI::TextDecorationLine::Overline | UI::TextDecorationLine::LineThrough;
    all.decoration.color = UI::Color{1.0, 0.0, 0.0, 1.0};
    all.decoration.style = UI::BorderLineStyle::Double;
    all.decoration.thickness = 6.0f;
    {
        const UI::FrameSnapshot snapshot = layout(ui, all);
        assert(snapshot.quads().size() == 6);
        assert(snapshot.quads()[0].instance.fill_color.r > 0.9f && snapshot.quads()[0].instance.fill_color.g < 0.1f);
    }

    // Dashed and dotted produce many segments.
    UI::TextStyle dashed = underlined;
    dashed.decoration.style = UI::BorderLineStyle::Dashed;
    assert(layout(ui, dashed).quads().size() > 2);
    UI::TextStyle dotted = underlined;
    dotted.decoration.style = UI::BorderLineStyle::Dotted;
    assert(layout(ui, dotted).quads().size() > layout(ui, dashed).quads().size());

    // A shadow is a second run of glyphs, moved by its offset, in its colour, drawn first.
    UI::TextStyle shadowed = plain;
    shadowed.shadow.enabled = true;
    shadowed.shadow.offset = glm::vec2{3.0f, 2.0f};
    shadowed.shadow.color = UI::Color{0.0, 0.0, 1.0, 1.0};
    {
        const UI::FrameSnapshot snapshot = layout(ui, shadowed);
        assert(snapshot.glyphs().size() == base_glyphs * 2);
        const auto &first = snapshot.glyphs()[0];
        const auto &second = snapshot.glyphs()[base_glyphs];
        assert(std::abs((first.position.x - second.position.x) - 3.0f) < 0.01f && std::abs((first.position.y - second.position.y) - 2.0f) < 0.01f);
        assert(first.color.r < 0.1f && first.color.b > 0.9f); // the shadow is blue
        assert(second.color.r > 0.9f);                         // the text on top is the white it always was
    }

    ui.destroy();
    SFT::Foundation::log_info("UiTextDecorationTest: ok");
    return 0;
}
