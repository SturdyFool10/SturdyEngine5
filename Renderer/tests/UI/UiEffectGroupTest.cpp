// UI effect groups, CPU side: an element with `ElementDecl::effect` becomes an EFFECT_START / EFFECT_END pair in SEClay's command stream
// and an `EffectGroupDraw` in the snapshot, with the draws it contains inside its paint-order range, nested effects nested, and a bad
// effect degrading to an ordinary element instead of breaking the frame. No GPU is involved.

#include <Renderer/UI/UI.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT;
    using SFT::UI::Context;
    using SFT::UI::ElementDecl;
    using SFT::UI::SizingAxis;

    Context make() {
        Context::Config config{};
        config.load_default_font = false;
        auto created = Context::create(config);
        assert(created.has_value());
        return std::move(*created);
    }

    std::shared_ptr<UI::UiEffect> blur_effect() {
        auto effect = std::make_shared<UI::UiEffect>();
        effect->name = UString{"test blur"};
        UI::UiEffectPass horizontal;
        horizontal.label = UString{"blur horizontal"};
        horizontal.shader_path = UString{"Shaders/ui_effect_blur.slang"};
        horizontal.module_name = UString{"ui_effect_blur"};
        horizontal.inputs.push_back(UI::UiEffectInput{.kind = UI::UiEffectInputKind::GroupContent});
        horizontal.output.scale = 0.5f;
        UI::UiEffectPass vertical = horizontal;
        vertical.label = UString{"blur vertical"};
        vertical.inputs = {UI::UiEffectInput{.kind = UI::UiEffectInputKind::PassOutput, .pass = 0}};
        effect->passes = {horizontal, vertical};
        return effect;
    }

    ElementDecl box(f32 w, f32 h, UI::Color color, std::shared_ptr<const UI::UiEffect> effect = nullptr) {
        ElementDecl decl{.sizing = {SizingAxis::fixed(w), SizingAxis::fixed(h)}, .background_color = color};
        decl.effect = std::move(effect);
        return decl;
    }

    void validation() {
        auto effect = blur_effect();
        assert(!UI::validate_effect(*effect).has_value());

        const auto rejected = [](UI::UiEffect copy, const char *what) {
            const auto problem = UI::validate_effect(copy);
            if (!problem.has_value()) {
                std::fprintf(stderr, "validate_effect accepted: %s\n", what);
                std::abort();
            }
        };
        { auto e = *effect; e.passes.clear(); rejected(e, "no passes"); }
        { auto e = *effect; e.passes[0].shader_path.clear(); rejected(e, "no shader path"); }
        { auto e = *effect; e.passes[0].inputs[0] = {.kind = UI::UiEffectInputKind::PassOutput, .pass = 0}; rejected(e, "pass reads itself"); }
        { auto e = *effect; e.passes[1].inputs[0].pass = 1; rejected(e, "pass reads a later pass"); }
        { auto e = *effect; e.passes[0].inputs[0] = {.kind = UI::UiEffectInputKind::Texture}; rejected(e, "texture input without a texture"); }
        { auto e = *effect; e.passes[0].output.scale = 0.0f; rejected(e, "zero scale"); }
        { auto e = *effect; e.passes[0].output.mip_levels = 40; rejected(e, "too many mips"); }
        { auto e = *effect; e.passes[0].parameters.resize(6); rejected(e, "parameters not a multiple of 4"); }
        { auto e = *effect; e.passes[0].parameters.resize(88); rejected(e, "too many parameter bytes"); }
        { auto e = *effect; e.opacity = 1.5f; rejected(e, "opacity above 1"); }
        { auto e = *effect; e.passes[0].inputs.resize(UI::kUiEffectMaxInputs + 1); rejected(e, "too many inputs"); }
    }

    void groups() {
        Context context = make();
        const auto effect = blur_effect();
        const auto inner_effect = blur_effect();

        context.begin_layout({400.0f, 300.0f});
        {
            auto root = context.element(ElementDecl{.sizing = {SizingAxis::fixed(400.0f), SizingAxis::fixed(300.0f)},
                                                    .direction = UI::LayoutDirection::TopToBottom});
            { auto before = context.element(box(100.0f, 20.0f, UI::Color{1, 0, 0, 1})); }
            {
                // The effect group: its own background plus two children, the second of which has an effect of its own.
                auto group = context.element(ElementDecl{.sizing = {SizingAxis::fixed(200.0f), SizingAxis::fixed(120.0f)},
                                                         .padding = UI::Padding::all(10),
                                                         .child_gap = 10,
                                                         .direction = UI::LayoutDirection::TopToBottom,
                                                         .background_color = UI::Color{0, 1, 0, 1},
                                                         .effect = effect});
                { auto a = context.element(box(50.0f, 20.0f, UI::Color{0, 0, 1, 1})); }
                { auto b = context.element(box(60.0f, 30.0f, UI::Color{1, 1, 0, 1}, inner_effect)); }
            }
            { auto after = context.element(box(100.0f, 20.0f, UI::Color{1, 0, 1, 1})); }
        }
        const UI::FrameSnapshot snapshot = context.finish_frame();

        const auto groups = snapshot.effect_groups();
        assert(groups.size() == 2);
        // Outer first, then the group nested in it.
        assert(groups[0].effect == effect.get());
        assert(groups[0].parent == -1);
        assert(groups[1].effect == inner_effect.get());
        assert(groups[1].parent == 0);
        // Bounds are the elements' own boxes: the outer starts below the 20 px row and is 200x120; the inner is 60x30.
        assert(groups[0].position.y == 20.0f && groups[0].size.x == 200.0f && groups[0].size.y == 120.0f);
        assert(groups[1].size.x == 60.0f && groups[1].size.y == 30.0f);
        assert(groups[1].position.x == 10.0f && groups[1].position.y == 20.0f + 10.0f + 20.0f + 10.0f);
        // The ranges nest and do not overlap the neighbours.
        assert(groups[0].paint.order < groups[1].paint.order);
        assert(groups[1].paint.order < groups[1].last_order);
        assert(groups[1].last_order < groups[0].last_order);

        // Which quads fall inside which group, by colour: red before, magenta after, the other three inside the outer group, and
        // only yellow inside the inner one.
        const auto inside = [&](const UI::EffectGroupDraw &group, const UI::QuadDraw &quad) {
            return quad.paint.order > group.paint.order && quad.paint.order < group.last_order;
        };
        usize in_outer = 0;
        usize in_inner = 0;
        for (const UI::QuadDraw &quad : snapshot.quads()) {
            const glm::vec4 &c = quad.instance.fill_color;
            const bool red = c.x > 0.5f && c.y < 0.5f && c.z < 0.5f;
            const bool magenta = c.x > 0.5f && c.y < 0.5f && c.z > 0.5f;
            const bool yellow = c.x > 0.5f && c.y > 0.5f && c.z < 0.5f;
            if (red || magenta) {
                assert(!inside(groups[0], quad));
            } else {
                assert(inside(groups[0], quad));
                ++in_outer;
            }
            if (yellow) {
                assert(inside(groups[1], quad));
                ++in_inner;
            } else {
                assert(!inside(groups[1], quad));
            }
        }
        assert(in_outer == 3);
        assert(in_inner == 1);
    }

    void pixel_scale_and_bad_effect() {
        Context context = make();
        context.set_pixel_scale(2.0f);
        auto bad = std::make_shared<UI::UiEffect>(); // no passes
        context.begin_layout({800.0f, 600.0f});
        {
            auto root = context.element(ElementDecl{.sizing = {SizingAxis::fixed(400.0f), SizingAxis::fixed(300.0f)}});
            auto good = context.element(box(30.0f, 40.0f, UI::Color{1, 1, 1, 1}, blur_effect()));
            auto broken = context.element(box(30.0f, 40.0f, UI::Color{1, 1, 1, 1}, bad));
        }
        const UI::FrameSnapshot snapshot = context.finish_frame();
        // The broken effect is ignored; the element still draws.
        assert(snapshot.effect_groups().size() == 1);
        assert(snapshot.quads().size() == 2);
        // Group geometry is in framebuffer pixels (twice the logical size) and remembers the scale.
        const UI::EffectGroupDraw &group = snapshot.effect_groups()[0];
        assert(group.size.x == 60.0f && group.size.y == 80.0f);
        assert(group.pixel_scale == 2.0f);
    }

} // namespace

int main() {
    validation();
    groups();
    pixel_scale_and_bad_effect();
    std::printf("UiEffectGroupTest: ok\n");
    return 0;
}
