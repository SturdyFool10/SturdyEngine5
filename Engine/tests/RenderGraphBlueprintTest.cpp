/// Data-driven render graph composition: blueprints, the module registry, composites, JSON.

#include <Engine/RenderGraphBlueprint.hpp>

#include <iostream>

namespace {
    using namespace SFT::Engine;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    template <class T>
    bool is_error(const BlueprintExpected<T> &result, BlueprintErrorCode code) {
        return !result.has_value() && result.error().code == code;
    }
} // namespace

int main() {
    // The standard blueprint lowers to the standard chain.
    {
        auto graph = build_render_graph(RenderGraphBlueprint::standard());
        check(graph.has_value(), "the standard blueprint must build and validate");
        check(graph && graph->passes().size() == 5, "the standard blueprint must produce five passes");
        check(graph && graph->passes().front().kind == RenderGraphPassKind::DeferredScene &&
                  graph->passes().back().kind == RenderGraphPassKind::Present,
              "the standard blueprint must run scene first and present last");
    }

    // Effects and extra inputs, written naturally.
    {
        RenderGraphBlueprint blueprint;
        const NodeRef scene = blueprint.scene();
        const NodeRef aa = blueprint.anti_aliasing(scene);
        NodeRef color = blueprint.tone_mapping(aa);
        const std::array<NodeRef, 1> extras{aa};
        FullscreenEffectDescription grade{.shader_path = "Shaders/grade.slang", .module_name = "grade"};
        grade.set_push_constants(1.5F);
        color = blueprint.fullscreen_effect(color, grade, extras);
        (void)blueprint.present(color);
        auto graph = build_render_graph(blueprint);
        check(graph.has_value(), "a graph with a display-space effect and an extra input must build");
        if (graph) {
            const auto effect = std::ranges::find_if(graph->passes(), [](const RenderGraphPassDescription &pass) {
                return pass.kind == RenderGraphPassKind::FullscreenEffect;
            });
            check(effect != graph->passes().end() && effect->fullscreen_effect.extra_inputs.size() == 1 &&
                      effect->fullscreen_effect.push_constants.size() == sizeof(float) &&
                      effect->fullscreen_effect.module_name == "grade",
                  "effect parameters and extra inputs must survive the blueprint");
        }
    }

    // Nodes may be declared in any order.
    {
        RenderGraphBlueprint blueprint;
        blueprint.add("present", {NodeRef{"tone"}}, {}, "end");
        blueprint.add("tone_mapping", {NodeRef{"scene"}}, {}, "tone");
        blueprint.add("deferred_scene", std::span<const NodeRef>{}, {}, "scene");
        check(build_render_graph(blueprint).has_value(), "declaration order must not matter");
    }

    // Composites: a module defined from other modules, used like a built-in.
    {
        RenderGraphBlueprint grade;
        const NodeRef in = grade.declare_input("in");
        const NodeRef first = grade.fullscreen_effect(in, {.shader_path = "Shaders/a.slang", .module_name = "a"});
        const NodeRef second = grade.fullscreen_effect(first, {.shader_path = "Shaders/b.slang", .module_name = "b"});
        grade.set_output(second);

        RenderModuleRegistry registry = RenderModuleRegistry::with_builtins();
        check(registry.define_composite("grade", grade).has_value(), "defining a composite must succeed");
        check(is_error(registry.define_composite("grade", grade), BlueprintErrorCode::DuplicateModule), "redefining a module must fail");

        RenderGraphBlueprint blueprint;
        NodeRef color = blueprint.tone_mapping(blueprint.scene());
        color = blueprint.add("grade", {color});
        (void)blueprint.present(color);
        auto graph = registry.build(blueprint);
        check(graph.has_value(), "a graph using a composite must build");
        check(graph && graph->passes().size() == 5, "the composite must expand into its two effects");

        // Composites nest.
        RenderGraphBlueprint twice;
        const NodeRef twice_in = twice.declare_input("x");
        twice.set_output(twice.add("grade", {twice.add("grade", {twice_in})}));
        check(registry.define_composite("grade_twice", twice).has_value(), "defining a nested composite must succeed");
        RenderGraphBlueprint nested;
        (void)nested.present(nested.add("grade_twice", {nested.tone_mapping(nested.scene())}));
        auto nested_graph = registry.build(nested);
        check(nested_graph && nested_graph->passes().size() == 7, "nested composites must expand recursively");

        // The built-in registry knows nothing of it.
        check(is_error(build_render_graph(blueprint), BlueprintErrorCode::UnknownModule), "kinds are per registry");

        // A composite defined in terms of itself is caught, not looped.
        RenderGraphBlueprint recursive;
        recursive.set_output(recursive.add("loop", {recursive.declare_input("r")}));
        check(registry.define_composite("loop", recursive).has_value(), "defining a self-referential composite is allowed");
        RenderGraphBlueprint uses_loop;
        (void)uses_loop.present(uses_loop.add("loop", {uses_loop.tone_mapping(uses_loop.scene())}));
        check(is_error(registry.build(uses_loop), BlueprintErrorCode::TooDeep), "a self-referential composite must be rejected");
    }

    // Modules written in code.
    {
        RenderModuleRegistry registry = RenderModuleRegistry::with_builtins();
        check(registry
                  .register_module({.name = "vignette", .min_inputs = 1, .max_inputs = 1},
                                   [](ModuleBuildContext &context) -> BlueprintExpected<ModuleOutput> {
                                       FullscreenEffectDescription effect{.shader_path = "Shaders/vignette.slang", .module_name = "vignette"};
                                       effect.set_push_constants(context.params.float_or("strength", 0.5));
                                       return context.graph.add_fullscreen_effect(context.inputs[0], effect);
                                   })
                  .has_value(),
              "registering a code module must succeed");
        RenderGraphBlueprint blueprint;
        NodeRef color = blueprint.tone_mapping(blueprint.scene());
        color = blueprint.add("vignette", {color}, BlueprintParams{}.set("strength", 0.8));
        (void)blueprint.present(color);
        auto graph = registry.build(blueprint);
        check(graph.has_value(), "a graph using a code module must build");
        bool found_strength = false;
        if (graph) {
            for (const RenderGraphPassDescription &pass : graph->passes()) {
                if (pass.kind == RenderGraphPassKind::FullscreenEffect && pass.fullscreen_effect.push_constants.size() == sizeof(double)) {
                    found_strength = true;
                }
            }
        }
        check(found_strength, "a code module must receive its node's parameters");
    }

    // JSON round trip, including composites.
    {
        const char *text = R"({
            "modules": {
                "grade": { "inputs": ["in"], "output": "b", "nodes": [
                    {"id": "a", "module": "fullscreen_effect", "inputs": ["in"], "params": {"shader_path": "Shaders/a.slang", "module_name": "a"}},
                    {"id": "b", "module": "fullscreen_effect", "inputs": ["a"], "params": {"shader_path": "Shaders/b.slang", "module_name": "b", "push_constants": [0, 0, 128, 63]}}
                ]}
            },
            "nodes": [
                {"id": "scene", "module": "deferred_scene"},
                {"id": "tone", "module": "tone_mapping", "inputs": ["scene"]},
                {"id": "graded", "module": "grade", "inputs": ["tone"]},
                {"id": "end", "module": "present", "inputs": ["graded"]}
            ]
        })";
        RenderModuleRegistry registry = RenderModuleRegistry::with_builtins();
        auto blueprint = load_render_graph_json(text, registry);
        check(blueprint.has_value(), "a JSON graph with a composite must load");
        auto graph = blueprint ? registry.build(*blueprint) : BlueprintExpected<RenderGraph>{std::unexpected(BlueprintError{})};
        check(graph.has_value(), "a loaded JSON graph must build");
        check(graph && graph->passes().size() == 5, "the JSON composite must expand");

        if (blueprint) {
            const UString written = blueprint->to_json();
            auto reread = RenderGraphBlueprint::from_json(written.cpp_string_view());
            check(reread.has_value() && reread->nodes().size() == blueprint->nodes().size(), "a blueprint must survive to_json/from_json");
            check(reread && registry.build(*reread).has_value(), "the re-read blueprint must still build");
        }
    }

    // Errors are specific and located.
    {
        RenderGraphBlueprint unknown;
        (void)unknown.present(unknown.add("does_not_exist", {unknown.scene()}));
        auto result = build_render_graph(unknown);
        check(is_error(result, BlueprintErrorCode::UnknownModule), "an unknown module kind must be reported");

        RenderGraphBlueprint dangling;
        (void)dangling.add("tone_mapping", {NodeRef{"nowhere"}});
        check(is_error(build_render_graph(dangling), BlueprintErrorCode::UnknownNode), "consuming an unknown node must be reported");

        RenderGraphBlueprint duplicate;
        (void)duplicate.scene("s");
        (void)duplicate.scene("s");
        check(is_error(build_render_graph(duplicate), BlueprintErrorCode::DuplicateNode), "duplicate ids must be reported");

        RenderGraphBlueprint cycle;
        cycle.add("bloom", {NodeRef{"b"}}, {}, "a");
        cycle.add("bloom", {NodeRef{"a"}}, {}, "b");
        check(is_error(build_render_graph(cycle), BlueprintErrorCode::Cycle), "a cycle must be reported");

        RenderGraphBlueprint arity;
        (void)arity.add("tone_mapping", {arity.scene(), arity.scene()});
        check(is_error(build_render_graph(arity), BlueprintErrorCode::WrongInputCount), "a wrong input count must be reported");

        RenderGraphBlueprint incomplete;
        (void)incomplete.tone_mapping(incomplete.scene());
        check(is_error(build_render_graph(incomplete), BlueprintErrorCode::InvalidGraph), "a graph the engine cannot validate must be reported");

        RenderGraphBlueprint missing_shader;
        (void)missing_shader.add("fullscreen_effect", {missing_shader.scene()});
        check(is_error(build_render_graph(missing_shader), BlueprintErrorCode::InvalidParameter), "a missing shader parameter must be reported");

        check(is_error(RenderGraphBlueprint::from_json("{ nope"), BlueprintErrorCode::InvalidDocument), "bad JSON must be reported");
        check(is_error(RenderGraphBlueprint::from_json(R"({"nodes": [{"id": "x"}]})"), BlueprintErrorCode::InvalidDocument),
              "a node without a module must be reported");
    }

    return failures == 0 ? 0 : 1;
}
