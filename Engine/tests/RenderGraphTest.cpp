#include <Engine/RenderGraph.hpp>

#include <algorithm>
#include <iostream>

namespace {

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    bool standard_graph_is_explicit() {
        const SFT::Engine::RenderGraph graph = SFT::Engine::RenderGraph::standard();
        bool passed = check(graph.validate().has_value(), "standard graph failed validation");
        passed &= check(graph.passes().size() == 6, "standard graph does not expose six module nodes");
        passed &= check(graph.textures().size() == 5, "standard graph does not expose five dataflow textures");
        passed &= check(graph.passes().front().kind == SFT::Engine::RenderGraphPassKind::DeferredScene,
                        "standard graph does not begin with deferred scene");
        passed &= check(graph.passes().back().kind == SFT::Engine::RenderGraphPassKind::Present,
                        "standard graph does not end with present");
        passed &= check(graph.presented_texture() == graph.passes()[4].output,
                        "standard present input is not the final module output");
        return passed;
    }

    struct ApplicationEffectPair {
        SFT::Engine::RenderGraphTextureHandle input{};

        [[nodiscard]] SFT::Engine::RenderGraphTextureHandle build(
            SFT::Engine::RenderGraph &graph) const {
            using namespace SFT::Engine;
            RenderGraphTextureHandle output = graph.add_fullscreen_effect(
                input,
                FullscreenEffectDescription{
                    .shader_path = "Shaders/application_first.slang",
                    .module_name = "application_first",
                    .push_constants = {},
                    .label = UString{"application first"_ustr},
                });
            output = graph.add_fullscreen_effect(
                output,
                FullscreenEffectDescription{
                    .shader_path = "Shaders/application_second.slang",
                    .module_name = "application_second",
                    .push_constants = {},
                    .label = UString{"application second"_ustr},
                });
            return output;
        }
    };

    bool application_module_can_declare_safe_passes() {
        using namespace SFT::Engine;

        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::AntiAliasing{.input = color});
        color = graph.compose(ApplicationEffectPair{.input = color});
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        color = graph.compose(RenderModules::DebugOverlay{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});

        bool passed = check(graph.validate().has_value(), "application-defined module failed validation");
        passed &= check(graph.passes().size() == 7,
                        "application-defined module did not append both declarative passes");
        passed &= check(graph.passes()[2].kind == RenderGraphPassKind::FullscreenEffect &&
                        graph.passes()[3].kind == RenderGraphPassKind::FullscreenEffect,
                        "application-defined module passes were not represented in graph data");
        return passed;
    }

    bool branches_are_valid_and_presentation_lowering_is_reachable_only() {
        using namespace SFT::Engine;

        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::AntiAliasing{.input = color});
        RenderGraphTextureHandle diagnostic_branch = graph.add_fullscreen_effect(
            color,
            FullscreenEffectDescription{
                .shader_path = "Shaders/diagnostic_branch.slang",
                .module_name = "diagnostic_branch",
                .push_constants = {},
                .label = UString{"diagnostic branch"_ustr},
            });

        color = graph.compose(RenderModules::Bloom{.input = color});
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        color = graph.compose(RenderModules::DebugOverlay{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});

        // Declaration order no longer has to end at Present. This remains a dead branch because no
        // observable graph output consumes it; current Renderer lowering follows presentation ancestry.
        diagnostic_branch = graph.add_fullscreen_effect(
            diagnostic_branch,
            FullscreenEffectDescription{
                .shader_path = "Shaders/diagnostic_branch_finish.slang",
                .module_name = "diagnostic_branch_finish",
                .push_constants = {},
                .label = UString{"diagnostic branch finish"_ustr},
            });
        (void)diagnostic_branch;

        const std::vector<RenderGraphPassHandle> path = graph.presentation_path();
        bool passed = check(graph.validate().has_value(), "branched graph failed validation");
        passed &= check(graph.passes().size() == 8, "branched graph lost declared pass nodes");
        passed &= check(path.size() == 6, "presentation ancestry included dead branch passes");
        passed &= check(path.front() == graph.passes()[0].handle &&
                        graph.passes()[path.back().index].kind == RenderGraphPassKind::Present,
                        "presentation ancestry endpoints are incorrect");
        passed &= check(std::ranges::none_of(path, [](RenderGraphPassHandle handle) {
                            return handle.index == 2 || handle.index == 7;
                        }),
                        "dead branch pass handles leaked into presentation ancestry");
        passed &= check(graph.contains_pass(RenderGraphPassKind::FullscreenEffect) &&
                        !graph.presentation_contains_pass(RenderGraphPassKind::FullscreenEffect),
                        "graph-wide and presentation-path pass queries were not distinguished");
        return passed;
    }

    bool fullscreen_modules_compose_by_dataflow() {
        using namespace SFT::Engine;

        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::AntiAliasing{.input = color});

        FullscreenEffectDescription before_bloom{
            .shader_path = "Shaders/test_before_bloom.slang",
            .module_name = "test_before_bloom",
            .fragment_entry_point = "fragmentMain",
            .push_constants = {},
            .label = UString{"before bloom"_ustr},
        };
        before_bloom.set_push_constants(42u);
        color = graph.compose(RenderModules::FullscreenEffect{
            .input = color,
            .effect = before_bloom,
        });
        color = graph.compose(RenderModules::Bloom{.input = color});
        color = graph.compose(RenderModules::FullscreenEffect{
            .input = color,
            .effect = FullscreenEffectDescription{
                .shader_path = "Shaders/test_after_bloom.slang",
                .module_name = "test_after_bloom",
                .push_constants = {},
                .label = UString{"after bloom"_ustr},
            },
        });
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        color = graph.compose(RenderModules::DebugOverlay{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});

        bool passed = check(graph.validate().has_value(), "composed fullscreen graph failed validation");
        passed &= check(graph.passes().size() == 8, "composed graph lost module nodes");
        passed &= check(graph.passes()[2].kind == RenderGraphPassKind::FullscreenEffect,
                        "before-bloom effect is not represented as a graph pass");
        passed &= check(graph.passes()[4].kind == RenderGraphPassKind::FullscreenEffect,
                        "after-bloom effect is not represented as a graph pass");
        passed &= check(graph.passes()[2].fullscreen_effect.push_constants.size() == sizeof(u32),
                        "typed fullscreen constants were not copied into graph-owned storage");
        return passed;
    }

    bool explicit_compute_copy_outputs_control_execution() {
        using namespace SFT::Engine;

        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::AntiAliasing{.input = color});
        const RenderGraphTextureHandle branch_input = color;
        const RenderGraphTextureHandle computed = graph.compose(RenderModules::ComputeEffect{
            .input = branch_input,
            .effect = ComputeEffectDescription{
                .shader_path = "Shaders/test_compute_branch.slang",
                .module_name = "test_compute_branch",
                .compute_entry_point = "computeMain",
                .push_constants = {},
                .label = UString{"test compute branch"_ustr},
            },
        });
        const RenderGraphTextureHandle copied = graph.compose(RenderModules::Copy{
            .input = computed,
            .copy = CopyDescription{.label = UString{"test copied branch"_ustr}},
        });
        color = graph.compose(RenderModules::Bloom{.input = color});
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        color = graph.compose(RenderModules::DebugOverlay{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});

        const std::vector<RenderGraphPassHandle> before_mark = graph.execution_passes();
        bool passed = check(std::ranges::none_of(before_mark, [&graph](RenderGraphPassHandle handle) {
                                const RenderGraphPassKind kind = graph.passes()[handle.index].kind;
                                return kind == RenderGraphPassKind::ComputeEffect || kind == RenderGraphPassKind::Copy;
                            }),
                            "unmarked compute/copy branch was retained");

        graph.mark_output(copied);
        const std::vector<RenderGraphPassHandle> after_mark = graph.execution_passes();
        passed &= check(graph.validate().has_value(), "marked compute/copy branch failed validation");
        passed &= check(std::ranges::any_of(after_mark, [&graph](RenderGraphPassHandle handle) {
                            return graph.passes()[handle.index].kind == RenderGraphPassKind::ComputeEffect;
                        }) &&
                        std::ranges::any_of(after_mark, [&graph](RenderGraphPassHandle handle) {
                            return graph.passes()[handle.index].kind == RenderGraphPassKind::Copy;
                        }),
                        "explicit output did not retain the complete compute/copy ancestry");
        passed &= check(graph.textures()[computed.index].format == RenderGraphTextureFormat::Inherit &&
                        graph.textures()[copied.index].format == RenderGraphTextureFormat::Inherit &&
                        graph.textures()[computed.index].extent.input == branch_input &&
                        graph.textures()[copied.index].extent.input == computed,
                        "compute/copy outputs did not inherit their input format and extent");

        RenderGraph copy = graph;
        passed &= check(copy.outputs().size() == 1 && copy.outputs().front().index == copied.index &&
                        copy.outputs().front().generation != copied.generation && copy.validate().has_value(),
                        "graph copy did not rebase its explicit output handle");
        return passed;
    }

    bool copied_graphs_rebase_graph_local_handles() {
        using namespace SFT::Engine;

        const RenderGraph source = RenderGraph::standard();
        RenderGraph copy = source;
        bool passed = check(copy.validate().has_value(), "copied graph failed after internal handle rebasing");
        passed &= check(source.passes()[1].output.index == copy.passes()[1].output.index &&
                        source.passes()[1].output.generation != copy.passes()[1].output.generation,
                        "copied graph retained the source graph generation");

        (void)copy.add_fullscreen_effect(
            source.passes()[1].output,
            FullscreenEffectDescription{
                .shader_path = "Shaders/foreign_handle.slang",
                .module_name = "foreign_handle",
                .push_constants = {},
                .label = UString{"foreign handle"_ustr},
            });
        const RenderGraphResult validation = copy.validate();
        passed &= check(!validation.has_value() &&
                        validation.error().code == RenderGraphErrorCode::InvalidPassGraph,
                        "a handle from a divergent source graph was accepted by its copy");
        return passed;
    }

    bool invalid_graph_normalizes_to_safe_standard() {
        using namespace SFT::Engine;

        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::FullscreenEffect{
            .input = color,
            .effect = FullscreenEffectDescription{},
        });
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});

        const RenderGraphResult validation = graph.validate();
        bool passed = check(!validation.has_value(), "invalid fullscreen shader identity passed validation");
        passed &= check(validation.error().code == RenderGraphErrorCode::InvalidFullscreenEffect,
                        "invalid fullscreen effect returned the wrong error code");

        const RenderGraph normalized = graph.normalized();
        passed &= check(normalized.validate().has_value(), "normalization did not produce a safe graph");
        passed &= check(normalized.passes().size() == RenderGraph::standard().passes().size(),
                        "invalid topology did not fall back to the standard module chain");
        return passed;
    }

} // namespace

int main() {
    bool passed = true;
    passed &= standard_graph_is_explicit();
    passed &= application_module_can_declare_safe_passes();
    passed &= branches_are_valid_and_presentation_lowering_is_reachable_only();
    passed &= fullscreen_modules_compose_by_dataflow();
    passed &= explicit_compute_copy_outputs_control_execution();
    passed &= copied_graphs_rebase_graph_local_handles();
    passed &= invalid_graph_normalizes_to_safe_standard();
    return passed ? 0 : 1;
}
