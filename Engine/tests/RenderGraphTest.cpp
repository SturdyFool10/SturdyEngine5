#include <Engine/RenderGraph.hpp>
#include <Renderer/SpectralPathTracing.hpp>

#include <algorithm>
#include <iostream>

namespace {

    using SFT::Engine::ComputeEffectDescription;
    using SFT::Engine::CopyDescription;
    using SFT::Engine::FullscreenEffectDescription;
    using SFT::Engine::RenderGraph;
    using SFT::Engine::RenderGraphErrorCode;
    using SFT::Engine::RenderGraphPassHandle;
    using SFT::Engine::RenderGraphPassKind;
    using SFT::Engine::RenderGraphResult;
    using SFT::Engine::RenderGraphTextureFormat;
    using SFT::Engine::RenderGraphTextureHandle;
    using SFT::Engine::RenderTargetHandle;
    using SFT::Engine::SceneIntegrator;
    using SFT::Renderer::SpectralIntegratorPolicy;
    using SFT::Renderer::SpectralRenderMode;
    using SFT::Renderer::spectral_integrator_policy;
    namespace RenderModules = SFT::Engine::RenderModules;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Reports whether standard graph is explicit.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
        passed &= check(!graph.selected_render_target(),
                        "standard graph does not target the frame surface");
        passed &= check(!graph.passes().back().target,
                        "standard Present node stores a non-surface target");
        return passed;
    }

    /// Returns the current or globally available overlay only disables scene post processing value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool overlay_only_disables_scene_post_processing() {
        const SFT::Engine::RenderGraph graph = SFT::Engine::RenderGraph::overlay_only();
        bool passed = check(graph.validate().has_value(), "overlay-only graph failed validation");
        passed &= check(!graph.scene().enabled, "overlay-only graph still enables scene rendering");
        passed &= check(!graph.bloom().enabled, "overlay-only graph still enables bloom");
        passed &= check(!graph.tone_mapping().enabled, "overlay-only graph still enables tone mapping");
        passed &= check(graph.debug_overlay().enabled,
                        "overlay-only graph disabled timing collection with scene rendering");
        return passed;
    }

    struct ApplicationEffectPair {
        SFT::Engine::RenderGraphTextureHandle input{};

        /// Builds the requested object or derived state.
        ///
        /// @param graph `graph` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] SFT::Engine::RenderGraphTextureHandle build(
            SFT::Engine::RenderGraph &graph) const {
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

    /// Returns the current or globally available application module can declare safe passes value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool application_module_can_declare_safe_passes() {

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

    /// Fullscreen effects may follow tone mapping (display space); compute/copy may not.
    bool display_space_effects_follow_tone_mapping() {
        const auto effect = [](const char *label) {
            return FullscreenEffectDescription{
                .shader_path = "Shaders/grade.slang",
                .module_name = "grade",
                .push_constants = {},
                .label = UString{label},
            };
        };
        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        color = graph.add_fullscreen_effect(color, effect("grade"));
        color = graph.add_fullscreen_effect(color, effect("grain"));
        color = graph.compose(RenderModules::DebugOverlay{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});
        bool passed = check(graph.validate().has_value(), "fullscreen effects after tone mapping must validate");

        // A display effect placed after the debug overlay is rejected.
        RenderGraph late = RenderGraph::empty();
        RenderGraphTextureHandle late_color = late.compose(RenderModules::DeferredScene{});
        late_color = late.compose(RenderModules::ToneMapping{.input = late_color});
        late_color = late.compose(RenderModules::DebugOverlay{.input = late_color});
        late_color = late.add_fullscreen_effect(late_color, effect("late"));
        (void)late.compose(RenderModules::Present{.input = late_color});
        passed &= check(!late.validate().has_value(), "a display effect after the debug overlay must be rejected");

        // Compute after tone mapping is still HDR-only.
        RenderGraph compute = RenderGraph::empty();
        RenderGraphTextureHandle compute_color = compute.compose(RenderModules::DeferredScene{});
        compute_color = compute.compose(RenderModules::ToneMapping{.input = compute_color});
        compute_color = compute.add_compute_effect(
            compute_color, SFT::Engine::ComputeEffectDescription{.shader_path = "Shaders/c.slang", .module_name = "c", .label = UString{"c"_ustr}});
        (void)compute.compose(RenderModules::Present{.input = compute_color});
        passed &= check(!compute.validate().has_value(), "a compute effect after tone mapping must be rejected");
        return passed;
    }

    /// Extra sampled inputs must name textures produced earlier in the same graph.
    bool fullscreen_effects_accept_only_produced_extra_inputs() {
        RenderGraph graph = RenderGraph::empty();
        const RenderGraphTextureHandle scene = graph.compose(RenderModules::DeferredScene{});
        FullscreenEffectDescription effect{
            .shader_path = "Shaders/composite.slang",
            .module_name = "composite",
            .push_constants = {},
            .label = UString{"composite"_ustr},
            .extra_inputs = {scene},
        };
        RenderGraphTextureHandle color = graph.add_fullscreen_effect(scene, effect);
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color});
        bool passed = check(graph.validate().has_value(), "an effect sampling an earlier graph texture must validate");

        RenderGraph bad = RenderGraph::empty();
        const RenderGraphTextureHandle bad_scene = bad.compose(RenderModules::DeferredScene{});
        effect.extra_inputs = {RenderGraphTextureHandle{.index = 99, .generation = 1}};
        RenderGraphTextureHandle bad_color = bad.add_fullscreen_effect(bad_scene, effect);
        bad_color = bad.compose(RenderModules::ToneMapping{.input = bad_color});
        (void)bad.compose(RenderModules::Present{.input = bad_color});
        passed &= check(!bad.validate().has_value(), "an effect sampling an unknown texture must be rejected");

        // The extras survive a copy of the graph with their handles rebased.
        const RenderGraph copy = graph;
        passed &= check(copy.validate().has_value(), "a copied graph must keep valid extra-input handles");
        return passed;
    }

    /// Reports whether branches are valid and presentation lowering is reachable only.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool branches_are_valid_and_presentation_lowering_is_reachable_only() {

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

    /// Returns the current or globally available fullscreen modules compose by dataflow value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool fullscreen_modules_compose_by_dataflow() {

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

    /// Returns the current or globally available explicit compute copy outputs control execution value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool explicit_compute_copy_outputs_control_execution() {

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

    /// Returns the current or globally available offscreen target survives graph copies without rebasing value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool offscreen_target_survives_graph_copies_without_rebasing() {

        constexpr RenderTargetHandle target{.value = 0x1020304050607080ull};
        RenderGraph source = RenderGraph::empty();
        RenderGraphTextureHandle color = source.compose(RenderModules::DeferredScene{});
        color = source.compose(RenderModules::ToneMapping{.input = color});
        (void)source.compose(RenderModules::Present{.input = color, .target = target});

        RenderGraph constructed = source;
        RenderGraph assigned;
        assigned = source;
        RenderGraph normalized = source.normalized();

        bool passed = check(source.validate().has_value(), "targeted source graph failed validation");
        passed &= check(constructed.validate().has_value() && assigned.validate().has_value() &&
                            normalized.validate().has_value(),
                        "targeted graph copy failed validation");
        passed &= check(constructed.selected_render_target() == target &&
                            assigned.selected_render_target() == target &&
                            normalized.selected_render_target() == target,
                        "offscreen target identity changed across graph copies");
        passed &= check(constructed.passes().back().target == target &&
                            assigned.passes().back().target == target &&
                            normalized.passes().back().target == target,
                        "copied Present node did not retain its offscreen target");
        passed &= check(constructed.passes().front().handle.generation !=
                            source.passes().front().handle.generation &&
                        assigned.passes().front().handle.generation !=
                            source.passes().front().handle.generation,
                        "graph-local handles were not rebased while preserving the target");
        return passed;
    }

    /// Returns the current or globally available copied graphs rebase graph local handles value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool copied_graphs_rebase_graph_local_handles() {

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

    /// Returns the current or globally available spectral integrator contracts are explicit value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool spectral_integrator_contracts_are_explicit() {
        const SpectralIntegratorPolicy shadow = spectral_integrator_policy(SpectralRenderMode::ShadowOnly);
        const SpectralIntegratorPolicy reflection = spectral_integrator_policy(SpectralRenderMode::ReflectionOnly);
        const SpectralIntegratorPolicy ao = spectral_integrator_policy(SpectralRenderMode::AmbientOcclusionOnly);
        const SpectralIntegratorPolicy transmission = spectral_integrator_policy(SpectralRenderMode::ShadowAndTransmission);
        const SpectralIntegratorPolicy full = spectral_integrator_policy(SpectralRenderMode::FullPathTracing);
        bool passed = check(shadow.uses_ray_queries && shadow.raster_primary_visibility &&
                                shadow.raster_shadow_atlas,
                            "shadow-only policy does not preserve punctual-light shadow maps");
        passed &= check(reflection.uses_ray_queries && reflection.raster_deferred_lighting,
                        "reflection-only policy unexpectedly replaces deferred lighting");
        passed &= check(ao.uses_ray_queries && ao.raster_primary_visibility,
                        "AO-only policy unexpectedly replaces primary visibility");
        passed &= check(transmission.traces_transmission && transmission.raster_shadow_atlas,
                        "shadow+transmission policy does not preserve punctual-light shadow maps");
        passed &= check(full.writes_canonical_gbuffer &&
                            !full.raster_primary_visibility && !full.raster_deferred_lighting,
                        "full path tracing policy does not replace the raster scene producer");
        return passed;
    }

    /// Returns the current or globally available spectral settings validate and normalize value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool spectral_settings_validate_and_normalize() {
        RenderGraph graph = RenderGraph::standard();
        graph.scene().integrator = SceneIntegrator::FullPathTracing;
        graph.scene().wavelength_min_nm = 780.0f;
        graph.scene().wavelength_max_nm = 380.0f;
        bool passed = check(!graph.validate().has_value(),
                            "inverted spectral wavelength interval passed validation");
        graph.scene().path_samples_per_pixel = 500;
        graph.scene().path_max_bounces = 0;
        graph.anti_aliasing().msaa_samples = 8;
        const RenderGraph normalized = graph.normalized();
        passed &= check(normalized.validate().has_value(), "spectral settings did not normalize safely");
        passed &= check(normalized.scene().path_samples_per_pixel == 64 &&
                            normalized.scene().path_max_bounces == 1,
                        "spectral sample/bounce limits were not clamped");
        passed &= check(normalized.anti_aliasing().msaa_samples == 1,
                        "full path tracing did not disable raster MSAA");
        return passed;
    }

    /// Returns the current or globally available invalid graph normalizes to safe standard value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool invalid_graph_normalizes_to_safe_standard() {

        constexpr RenderTargetHandle target{.value = 77};
        RenderGraph graph = RenderGraph::empty();
        RenderGraphTextureHandle color = graph.compose(RenderModules::DeferredScene{});
        color = graph.compose(RenderModules::FullscreenEffect{
            .input = color,
            .effect = FullscreenEffectDescription{},
        });
        color = graph.compose(RenderModules::ToneMapping{.input = color});
        (void)graph.compose(RenderModules::Present{.input = color, .target = target});

        const RenderGraphResult validation = graph.validate();
        bool passed = check(!validation.has_value(), "invalid fullscreen shader identity passed validation");
        passed &= check(validation.error().code == RenderGraphErrorCode::InvalidFullscreenEffect,
                        "invalid fullscreen effect returned the wrong error code");

        const RenderGraph normalized = graph.normalized();
        passed &= check(normalized.validate().has_value(), "normalization did not produce a safe graph");
        passed &= check(normalized.passes().size() == RenderGraph::standard().passes().size(),
                        "invalid topology did not fall back to the standard module chain");
        passed &= check(normalized.selected_render_target() == target,
                        "normalization redirected an off-screen graph to the window surface");
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = true;
    passed &= standard_graph_is_explicit();
    passed &= overlay_only_disables_scene_post_processing();
    passed &= application_module_can_declare_safe_passes();
    passed &= display_space_effects_follow_tone_mapping();
    passed &= fullscreen_effects_accept_only_produced_extra_inputs();
    passed &= branches_are_valid_and_presentation_lowering_is_reachable_only();
    passed &= fullscreen_modules_compose_by_dataflow();
    passed &= explicit_compute_copy_outputs_control_execution();
    passed &= offscreen_target_survives_graph_copies_without_rebasing();
    passed &= copied_graphs_rebase_graph_local_handles();
    passed &= spectral_integrator_contracts_are_explicit();
    passed &= spectral_settings_validate_and_normalize();
    passed &= invalid_graph_normalizes_to_safe_standard();
    return passed ? 0 : 1;
}
