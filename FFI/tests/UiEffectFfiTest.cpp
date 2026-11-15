/// UI effects, corner shapes and layers through the C ABI: ready-made and custom effects are created, validated and released, an
/// element names an effect (and per-corner shapes), and the resulting frame carries the effect group and splits into layers. CPU only:
/// the layout runs on a headless `UI::Context` bound with `ForeignUiContextBinding`, so no window or GPU is needed.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <Engine/EngineModule.hpp>
#include <Engine/ForeignUi.hpp>
#include <FFI/AbiSupport.hpp>
#include <FFI/Sturdy.h>
#include <Foundation/Foundation.hpp>
#include <Renderer/UI/UI.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            SFT::Foundation::log_error("UiEffectFfiTest: {}", description);
            ++failures;
        }
    }

    SturdyUiEffect make_effect(SturdyUiEffectKind kind, std::initializer_list<float> values, const SturdyUiEffectOptions *options = nullptr) {
        SturdyUiEffectDesc desc;
        check(sturdy_ui_effect_desc_init(&desc) == STURDY_OK, "desc_init");
        desc.kind = kind;
        int i = 0;
        for (const float value : values) {
            desc.values[i++] = value;
        }
        SturdyUiEffect effect{};
        check(sturdy_ui_effect_create(&desc, options, &effect) == STURDY_OK, "create a library effect");
        check(effect.id != 0, "a created effect has a non-zero id");
        return effect;
    }

    SturdyUiElement sized_box(float size) {
        SturdyUiElement element;
        (void)sturdy_ui_element_init(&element);
        element.width_kind = STURDY_UI_SIZING_FIXED;
        element.width_value = size;
        element.height_kind = STURDY_UI_SIZING_FIXED;
        element.height_value = size;
        element.background[0] = 1.0f;
        element.background[3] = 1.0f;
        return element;
    }

} // namespace

int main() {
    const SturdyEngine engine{};
    // -- Library effects of every kind are accepted. --
    const SturdyUiEffectKind plain_kinds[] = {STURDY_UI_EFFECT_COLOR_ADJUST, STURDY_UI_EFFECT_TINT,         STURDY_UI_EFFECT_BLUR,
                                              STURDY_UI_EFFECT_FAST_BLUR,    STURDY_UI_EFFECT_DROP_SHADOW,  STURDY_UI_EFFECT_GLOW,
                                              STURDY_UI_EFFECT_OUTLINE,      STURDY_UI_EFFECT_INSET_SHADOW, STURDY_UI_EFFECT_FROSTED_GLASS,
                                              STURDY_UI_EFFECT_TRANSFORM,    STURDY_UI_EFFECT_CLIP_INSET,   STURDY_UI_EFFECT_CLIP_CIRCLE,
                                              STURDY_UI_EFFECT_CLIP_ELLIPSE, STURDY_UI_EFFECT_MASK_LINEAR};
    for (const SturdyUiEffectKind kind : plain_kinds) {
        const SturdyUiEffect effect = make_effect(kind, {8.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f});
        check(sturdy_ui_effect_release(effect) == STURDY_OK, "release a library effect");
        check(sturdy_ui_effect_release(effect) == STURDY_ERROR_INVALID_ARGUMENT, "releasing twice is refused");
    }

    // -- Gradients: stops are required, colour spaces are checked, per-stop interpolation is accepted. --
    {
        SturdyUiEffectDesc desc;
        (void)sturdy_ui_effect_desc_init(&desc);
        desc.kind = STURDY_UI_EFFECT_LINEAR_GRADIENT;
        SturdyUiEffect effect{};
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "a gradient without stops is refused");
        SturdyUiGradientStop stops[2]{};
        stops[0] = SturdyUiGradientStop{{1.0f, 0.0f, 0.0f, 1.0f}, 0.0f, STURDY_UI_COLOR_SPACE_SRGB, 1, STURDY_UI_COLOR_SPACE_OKLCH, STURDY_UI_HUE_LONGER};
        stops[1] = SturdyUiGradientStop{{0.5f, 0.2f, 250.0f, 1.0f}, 1.0f, STURDY_UI_COLOR_SPACE_OKLCH, 0, STURDY_UI_COLOR_SPACE_SRGB, STURDY_UI_HUE_SHORTER};
        desc.stops = stops;
        desc.stop_count = 2;
        desc.interpolation_space = STURDY_UI_COLOR_SPACE_HSL;
        desc.interpolation_hue = STURDY_UI_HUE_INCREASING;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_OK, "a two-stop gradient in a polar space is accepted");
        check(sturdy_ui_effect_release(effect) == STURDY_OK, "release the gradient");
        desc.interpolation_space = static_cast<SturdyUiColorSpace>(99);
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown colour space is refused");
        desc.interpolation_space = STURDY_UI_COLOR_SPACE_OKLAB;
        desc.stops = nullptr;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "stop_count with null stops is refused");
    }

    // -- Clip polygons and image masks have their own requirements. --
    {
        SturdyUiEffectDesc desc;
        (void)sturdy_ui_effect_desc_init(&desc);
        desc.kind = STURDY_UI_EFFECT_CLIP_POLYGON;
        const float triangle[6] = {0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        for (int i = 0; i < 6; ++i) {
            desc.values[i] = triangle[i];
        }
        desc.value_count = 6;
        SturdyUiEffect effect{};
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_OK, "a clip triangle is accepted");
        check(sturdy_ui_effect_release(effect) == STURDY_OK, "release the clip polygon");
        desc.value_count = 4;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "a clip polygon with two points is refused");
        (void)sturdy_ui_effect_desc_init(&desc);
        desc.kind = STURDY_UI_EFFECT_MASK_IMAGE;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "an image mask without a texture is refused");
        desc.texture = 7;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_OK, "an image mask with a texture is accepted");
        check(sturdy_ui_effect_release(effect) == STURDY_OK, "release the image mask");
    }

    // -- The hit-test mode. --
    {
        const SturdyEngine mode_engine{};
        check(sturdy_ui_set_hit_test_mode(mode_engine, STURDY_UI_HIT_TEST_FIXED) == STURDY_OK, "fixed hit testing is accepted");
        check(sturdy_ui_set_hit_test_mode(mode_engine, STURDY_UI_HIT_TEST_CSS) == STURDY_OK, "CSS hit testing is accepted");
        check(sturdy_ui_set_hit_test_mode(mode_engine, static_cast<SturdyUiHitTestMode>(9)) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown mode is refused");
        SturdyUiTreeDesc tree_desc;
        (void)sturdy_ui_tree_desc_init(&tree_desc);
        check(tree_desc.hit_test_mode == STURDY_UI_HIT_TEST_CSS, "trees default to CSS hit testing");
    }

    // -- Struct-size and null checks. --
    {
        SturdyUiEffectDesc desc;
        (void)sturdy_ui_effect_desc_init(&desc);
        SturdyUiEffect effect{};
        check(sturdy_ui_effect_create(nullptr, nullptr, &effect) == STURDY_ERROR_INVALID_ARGUMENT, "null desc");
        check(sturdy_ui_effect_create(&desc, nullptr, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "null output");
        desc.struct_size = 4;
        check(sturdy_ui_effect_create(&desc, nullptr, &effect) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "wrong desc size");
    }

    // -- Custom effects: passes are validated as they are added. --
    {
        SturdyUiEffect effect{};
        check(sturdy_ui_effect_create_custom("custom", nullptr, &effect) == STURDY_OK, "create a custom effect");
        SturdyUiEffectPass pass;
        (void)sturdy_ui_effect_pass_init(&pass);
        check(sturdy_ui_effect_add_pass(effect, &pass) == STURDY_ERROR_INVALID_ARGUMENT, "a pass with no shader is refused");
        pass.shader_path = "Shaders/ui_effect_tint.slang";
        pass.module_name = "ui_effect_tint";
        const float parameters[8] = {1, 1, 1, 1, 0, 0, 0, 0};
        pass.parameters = parameters;
        pass.parameter_size = sizeof(parameters);
        SturdyUiEffectInput input{};
        input.kind = STURDY_UI_EFFECT_INPUT_PASS_OUTPUT;
        input.pass = 0; // reads itself: pass 0 cannot read pass 0
        pass.inputs = &input;
        pass.input_count = 1;
        check(sturdy_ui_effect_add_pass(effect, &pass) == STURDY_ERROR_INVALID_ARGUMENT, "a pass reading a pass that has not run is refused");
        input.kind = STURDY_UI_EFFECT_INPUT_GROUP_CONTENT;
        check(sturdy_ui_effect_add_pass(effect, &pass) == STURDY_OK, "a valid pass is accepted");
        pass.parameter_size = 3;
        check(sturdy_ui_effect_add_pass(effect, &pass) == STURDY_ERROR_INVALID_ARGUMENT, "parameters that are not whole words are refused");
        check(sturdy_ui_effect_release(effect) == STURDY_OK, "release the custom effect");
        check(sturdy_ui_effect_add_pass(effect, &pass) == STURDY_ERROR_INVALID_ARGUMENT, "adding to a released effect is refused");
    }

    // -- Elements: an effect and corner shapes through begin_element_ex; a layer splits the frame. --
    {
        auto context = SFT::UI::Context::create(SFT::UI::Context::Config{});
        check(static_cast<bool>(context), "a headless UI context");
        if (!context) {
            return 1;
        }
        SturdyUiEffectOptions layer_options;
        (void)sturdy_ui_effect_options_init(&layer_options);
        layer_options.layer = 1;
        const SturdyUiEffect blur = make_effect(STURDY_UI_EFFECT_BLUR, {4.0f});
        const SturdyUiEffect glass = make_effect(STURDY_UI_EFFECT_FROSTED_GLASS, {8.0f, 0.1f, 1.0f}, &layer_options);
        const SturdyUiEffect marker = make_effect(STURDY_UI_EFFECT_LAYER, {}, &layer_options);

        context->begin_layout(glm::vec2{256.0f, 256.0f}, SFT::UI::PointerState{}, 1.0f / 60.0f);
        {
            SFT::Engine::ForeignUiContextBinding binding{*context, glm::vec2{256.0f, 256.0f}};
            check(!binding.rejected(), "the context binds");
            SturdyUiElement root = sized_box(256.0f);
            root.background[3] = 0.0f;
            check(sturdy_ui_begin_element(engine, &root) == STURDY_OK, "open the root");
            SturdyUiElementExtras extras;
            (void)sturdy_ui_element_extras_init(&extras);
            extras.effect = blur;
            extras.corner_shape[0] = 2.0f;
            extras.corner_shape[1] = -std::numeric_limits<float>::infinity();
            SturdyUiElement box = sized_box(64.0f);
            box.corner_radius[0] = box.corner_radius[1] = 12.0f;
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_OK, "an element with an effect and corner shapes");
            check(sturdy_ui_end_element(engine) == STURDY_OK, "close it");
            extras.effect = glass;
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_OK, "an element with a layered effect");
            check(sturdy_ui_end_element(engine) == STURDY_OK, "close the layered one");
            extras.effect = marker;
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_OK, "an element with a layer marker");
            check(sturdy_ui_end_element(engine) == STURDY_OK, "close the marker one");
            extras.effect = SturdyUiEffect{};
            extras.has_border_edges = STURDY_TRUE;
            extras.border_edge_color[0] = 1.0f; // left: opaque red
            extras.border_edge_color[3] = 1.0f;
            extras.border_edge_color[2 * 4 + 2] = 1.0f; // top: opaque blue
            extras.border_edge_color[2 * 4 + 3] = 1.0f;
            extras.border_edge_width[0] = 3.0f;
            extras.border_edge_width[2] = 3.0f;
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_OK, "an element with per-edge borders");
            check(sturdy_ui_end_element(engine) == STURDY_OK, "close the bordered one");
            extras.border_edge_style[0] = 9;
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown border style is refused");
            extras.has_border_edges = STURDY_FALSE;
            extras.effect = SturdyUiEffect{9999};
            check(sturdy_ui_begin_element_ex(engine, &box, &extras) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown effect id is refused");
            check(sturdy_ui_end_element(engine) == STURDY_OK, "close the root");
            check(binding.balanced(), "the tree is balanced");
        }
        auto snapshot = std::make_shared<SFT::UI::FrameSnapshot>(context->finish_frame());
        {
            usize edge_quads = 0;
            for (const SFT::UI::QuadDraw &quad : snapshot->quads()) {
                const glm::vec4 &w = quad.instance.border_width;
                edge_quads += ((w.x > 0.0f) + (w.y > 0.0f) + (w.z > 0.0f) + (w.w > 0.0f)) == 1;
            }
            check(edge_quads == 2, "the per-edge border became one quad per visible edge");
        }
        check(snapshot->effect_groups().size() == 3, "three effect groups (blur, glass, layer marker)");
        const auto pieces = SFT::UI::FrameSnapshot::split_layers(snapshot);
        check(pieces.size() == 2, "the frame splits into the base pass and layer 1");
        if (pieces.size() == 2) {
            check(pieces[0]->effect_groups().size() == 1, "the base pass keeps only the blur group");
            check(pieces[1]->effect_groups().size() == 1, "layer 1 holds the glass group; the bare marker needs no group");
            check(pieces[1]->quads().size() >= 2, "layer 1 holds the hoisted elements' draws");
        }
        context->destroy();
        (void)sturdy_ui_effect_release(blur);
        (void)sturdy_ui_effect_release(glass);
        (void)sturdy_ui_effect_release(marker);
    }

    // -- Retained UI nodes through the C ABI, driving a real (CPU-only) engine. --
    {
        SFT::Engine::Engine engine_object;
        const SFT::Ffi::ScopedHandle engine_handle{SFT::Ffi::HandleKind::Engine, &engine_object};
        const SturdyEngine node_engine{engine_handle.token()};

        SturdyUiNodeDesc desc;
        check(sturdy_ui_node_desc_init(&desc) == STURDY_OK, "node desc init");
        desc.element.width_kind = STURDY_UI_SIZING_FIXED;
        desc.element.width_value = 100.0f;
        desc.element.height_kind = STURDY_UI_SIZING_FIXED;
        desc.element.height_value = 100.0f;
        desc.element.id = "root";
        SturdyEntity root{};
        check(sturdy_ui_node_spawn(node_engine, &desc, &root) == STURDY_OK, "spawn a root node");

        desc.element.id = "child";
        desc.parent = root;
        desc.text = "hello";
        SturdyEntity child{};
        SturdyEntity none_placeholder{};
        check(sturdy_ui_node_spawn(node_engine, &desc, &child) == STURDY_OK, "spawn a child node");
        SturdyUiNodeState state{};
        check(sturdy_ui_node_state(node_engine, child, &state) == STURDY_OK && !state.hovered && !state.clicked, "a fresh node has no state");

        SturdyUiNodeDesc styled = desc;
        styled.text_style.decoration_lines = 1;
        styled.text_style.shadow_enabled = STURDY_TRUE;
        styled.extras.inset_for_corners = STURDY_TRUE;
        styled.element.id = "styled";
        SturdyEntity styled_node{};
        check(sturdy_ui_node_spawn(node_engine, &styled, &styled_node) == STURDY_OK, "a node with text decoration, shadow and corner inset");
        styled.text_style.decoration_style = 9;
        check(sturdy_ui_node_spawn(node_engine, &styled, &none_placeholder) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown decoration style is refused");
        SturdyUiNodeDesc bad = desc;
        bad.parent = SturdyEntity{999, 1};
        SturdyEntity none{};
        check(sturdy_ui_node_spawn(node_engine, &bad, &none) == STURDY_ERROR_INVALID_ARGUMENT, "a missing parent is refused");
        bad = desc;
        bad.struct_size = 4;
        check(sturdy_ui_node_spawn(node_engine, &bad, &none) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong struct size is refused");

        // A parent that would make a loop is refused.
        SturdyUiNodeDesc loop = desc;
        loop.parent = child;
        check(sturdy_ui_node_update(node_engine, root, &loop) == STURDY_ERROR_INVALID_ARGUMENT, "a parent loop is refused");

        // A node tree lays the nodes out once, and again only after a change.
        SturdyUiTreeDesc tree_desc;
        (void)sturdy_ui_tree_desc_init(&tree_desc);
        tree_desc.target.kind = STURDY_UI_TREE_TARGET_HEADLESS;
        tree_desc.target.headless_width = 200;
        tree_desc.target.headless_height = 200;
        tree_desc.name = "nodes";
        SturdyEntity tree{};
        check(sturdy_ui_node_tree_spawn(node_engine, root, &tree_desc, &tree) == STURDY_OK, "spawn a node tree");
        SturdyUiTreeStats stats{};
        engine_object.update(1.0 / 60.0);
        engine_object.update(1.0 / 60.0);
        check(sturdy_ui_tree_stats(node_engine, tree, &stats) == STURDY_OK && stats.builds == 1, "an unchanged node tree is laid out once");
        desc.element.background[0] = 0.5f;
        check(sturdy_ui_node_update(node_engine, child, &desc) == STURDY_OK, "update a node");
        engine_object.update(1.0 / 60.0);
        check(sturdy_ui_tree_stats(node_engine, tree, &stats) == STURDY_OK && stats.builds == 2, "an updated node lays the tree out again");

        tree_desc.builder = nullptr;
        check(sturdy_ui_node_destroy(node_engine, root, STURDY_TRUE) == STURDY_OK, "destroy a node recursively");
        check(sturdy_ui_node_state(node_engine, child, &state) == STURDY_ERROR_INVALID_ARGUMENT, "its child went with it");
    }

    if (failures == 0) {
        SFT::Foundation::log_info("UiEffectFfiTest: ok");
    }
    return failures == 0 ? 0 : 1;
}
