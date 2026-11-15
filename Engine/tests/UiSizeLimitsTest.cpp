/// Minimum and maximum sizes on `Fit` and `Grow` axes (CSS `min-width` / `max-width`) through the foreign element description every
/// binding shares, laid out by a real context. CPU only.

#include <Engine/ForeignUi.hpp>
#include <Renderer/UI/UI.hpp>

#include <string>

namespace {
    using namespace SFT;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
    }

    Engine::ForeignUiElement element(Engine::ForeignUiSizing kind, const char *id) {
        Engine::ForeignUiElement out;
        out.width_kind = kind;
        out.height_kind = Engine::ForeignUiSizing::Fixed;
        out.height_value = 20.0f;
        out.id = id;
        return out;
    }

    void layout(UI::Context &ui, const std::vector<Engine::ForeignUiElement> &children, glm::vec2 viewport) {
        for (int frame = 0; frame < 2; ++frame) {
            ui.begin_layout(viewport, UI::PointerState{}, 1.0f / 60.0f);
            {
                auto root = ui.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(viewport.x), UI::SizingAxis::fixed(viewport.y)},
                                                       .direction = UI::LayoutDirection::LeftToRight});
                (void)root;
                for (const Engine::ForeignUiElement &child : children) {
                    auto decl = Engine::make_foreign_ui_decl(child);
                    check(static_cast<bool>(decl), "the element converts");
                    if (decl) {
                        auto scope = ui.element(*decl);
                        (void)scope;
                    }
                }
            }
            (void)ui.finish_frame();
        }
    }

    f32 width_of(UI::Context &ui, const char *id) {
        const auto bounds = ui.element_bounds(UString{id});
        check(bounds.has_value(), "the element was laid out");
        return bounds ? bounds->size.x : -1.0f;
    }
} // namespace

int main() {
    auto created = UI::Context::create(UI::Context::Config{});
    check(static_cast<bool>(created), "a headless context");
    if (!created) {
        return 1;
    }
    UI::Context &ui = *created;

    // The limits land on the axis, and only on Fit and Grow.
    {
        Engine::ForeignUiElement grow = element(Engine::ForeignUiSizing::Grow, "g");
        grow.width_min = 50.0f;
        grow.width_max = 120.0f;
        const auto decl = Engine::make_foreign_ui_decl(grow);
        check(decl && decl->sizing.width.min == 50.0f && decl->sizing.width.max == 120.0f, "a grow axis takes both limits");
        Engine::ForeignUiElement fixed = element(Engine::ForeignUiSizing::Fixed, "f");
        fixed.width_value = 70.0f;
        fixed.width_min = 10.0f;
        fixed.width_max = 20.0f;
        const auto fixed_decl = Engine::make_foreign_ui_decl(fixed);
        check(fixed_decl && fixed_decl->sizing.width.min == 70.0f && fixed_decl->sizing.width.max == 70.0f, "a fixed axis ignores them");
        const auto none = Engine::make_foreign_ui_decl(element(Engine::ForeignUiSizing::Grow, "n"));
        check(none && none->sizing.width.min == 0.0f && none->sizing.width.max > 1.0e30f, "zero limits mean no limit");
    }

    // A maximum stops a Grow element short of the whole row; a minimum holds a Fit element open.
    {
        Engine::ForeignUiElement capped = element(Engine::ForeignUiSizing::Grow, "capped");
        capped.width_max = 100.0f;
        layout(ui, {capped}, glm::vec2{400.0f, 100.0f});
        check(width_of(ui, "capped") == 100.0f, "a grow element stops at its maximum");

        Engine::ForeignUiElement floor = element(Engine::ForeignUiSizing::Fit, "floor");
        floor.width_min = 80.0f;
        layout(ui, {floor}, glm::vec2{400.0f, 100.0f});
        check(width_of(ui, "floor") == 80.0f, "an empty fit element is held open to its minimum");

        Engine::ForeignUiElement wide = element(Engine::ForeignUiSizing::Grow, "wide");
        wide.width_min = 30.0f;
        Engine::ForeignUiElement squeezed = element(Engine::ForeignUiSizing::Grow, "squeezed");
        squeezed.width_min = 300.0f;
        layout(ui, {wide, squeezed}, glm::vec2{400.0f, 100.0f});
        check(width_of(ui, "squeezed") >= 300.0f, "a grow element keeps its minimum while another shares the row");
    }
    ui.destroy();

    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    return 0;
}
