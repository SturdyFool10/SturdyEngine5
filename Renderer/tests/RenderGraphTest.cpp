#include <Renderer/RenderGraph.hpp>
#include <Renderer/RenderGraphModule.hpp>

#include <iostream>

namespace {

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    struct TestSceneColor {
        [[maybe_unused]] static constexpr std::string_view name = "test.scene-color";
    };

    struct TestPresentationColor {
        [[maybe_unused]] static constexpr std::string_view name = "test.presentation-color";
    };

    struct TestSceneColorAlias {
        [[maybe_unused]] static constexpr std::string_view name = "test.scene-color";
    };

    bool semantic_blackboard_is_typed_and_reusable() {
        using namespace SFT::Renderer;

        RenderGraphBlackboard blackboard;
        const RenderGraphTextureHandle first{3};
        const RenderGraphTextureHandle replacement{7};
        const RenderGraphTextureHandle presentation{11};

        blackboard.publish_texture<TestSceneColor>(first);
        blackboard.publish_texture<TestPresentationColor>(presentation);
        bool passed = check(blackboard.texture<TestSceneColor>() == first,
                            "blackboard did not return the published scene texture");
        passed &= check(blackboard.texture<TestSceneColorAlias>() == first,
                        "value-based semantic identity did not survive a distinct tag type");
        passed &= check(blackboard.texture<TestPresentationColor>() == presentation,
                        "blackboard confused distinct semantic texture types");
        blackboard.publish_texture<TestSceneColor>(replacement);
        passed &= check(blackboard.texture<TestSceneColor>() == replacement,
                        "publishing a semantic replacement appended instead of updating");
        passed &= check(blackboard.texture_count() == 2,
                        "semantic replacement changed the blackboard entry count");
        blackboard.reset();
        passed &= check(!blackboard.contains_texture<TestSceneColor>() && blackboard.texture_count() == 0,
                        "blackboard reset retained live semantic resources");
        return passed;
    }

    bool explicit_outputs_control_liveness() {
        using namespace SFT::Renderer;

        RenderGraph graph;
        const RenderGraphTextureHandle unused_cache = graph.import_texture(RenderGraphImportedTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 32, .height = 32, .depth_or_layers = 1},
            .label = "unused imported cache",
        });
        const RenderGraphTextureHandle side_effect_target = graph.import_texture(RenderGraphImportedTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 32, .height = 32, .depth_or_layers = 1},
            .label = "side-effect target",
        });
        const RenderGraphTextureHandle presentation = graph.import_texture(RenderGraphImportedTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 32, .height = 32, .depth_or_layers = 1},
            .label = "presentation output",
        });

        graph.add_render_pass("dead imported write")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = unused_cache});
        graph.add_render_pass("explicit side effect")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = side_effect_target})
            .set_side_effect();
        graph.add_render_pass("presentation")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = presentation});
        graph.mark_output(presentation);

        const RenderGraph::CompileResult compiled = graph.compile();
        bool passed = check(compiled.has_value(), "explicit-output graph failed to compile");
        if (!compiled) {
            return false;
        }
        passed &= check(compiled->order.size() == 2,
                        "unmarked imported writes were retained or explicit side effect was culled");
        passed &= check(compiled->order[0].index == 1 && compiled->order[1].index == 2,
                        "explicit output/side-effect liveness selected the wrong passes");
        return passed;
    }

    bool invalid_buffer_ranges_and_access_intent_are_rejected() {
        using namespace SFT::Renderer;

        const auto compile_invalid_access = [](RenderGraphBufferAccessDesc access) {
            RenderGraph graph;
            const RenderGraphBufferHandle buffer = graph.import_buffer(RenderGraphImportedBufferDesc{
                .size = 128,
                .initial_stage = SFT::RHI::PipelineStage::None,
                .initial_access = SFT::RHI::AccessFlags::None,
                .final_stage = SFT::RHI::PipelineStage::None,
                .final_access = SFT::RHI::AccessFlags::None,
                .label = "validation buffer",
            });
            access.buffer = buffer;
            graph.add_compute_pass("invalid buffer access").add_buffer(access).set_side_effect();
            return graph.compile();
        };

        const RenderGraph::CompileResult empty_range = compile_invalid_access(RenderGraphBufferAccessDesc{
            .stages = SFT::RHI::PipelineStage::ComputeShader,
            .access = SFT::RHI::AccessFlags::ShaderRead,
            .offset = 128,
            .size = 0,
        });
        bool passed = check(!empty_range.has_value() &&
                            empty_range.error().code == RenderGraphCompileErrorCode::InvalidBufferAccess,
                            "empty to-end buffer range passed validation");

        const RenderGraph::CompileResult mismatched_intent = compile_invalid_access(RenderGraphBufferAccessDesc{
            .stages = SFT::RHI::PipelineStage::ComputeShader,
            .access = SFT::RHI::AccessFlags::ShaderRead,
            .read = false,
            .write = true,
        });
        passed &= check(!mismatched_intent.has_value() &&
                        mismatched_intent.error().code == RenderGraphCompileErrorCode::InvalidBufferAccess,
                        "buffer dependency intent disagreed with its barrier access mask");
        return passed;
    }

    bool imported_buffers_drive_dependencies_and_liveness() {
        using namespace SFT::Renderer;

        RenderGraph graph;
        const RenderGraphBufferHandle culled_instances = graph.import_buffer(RenderGraphImportedBufferDesc{
            .size = 4096,
            .initial_stage = SFT::RHI::PipelineStage::None,
            .initial_access = SFT::RHI::AccessFlags::None,
            .final_stage = SFT::RHI::PipelineStage::None,
            .final_access = SFT::RHI::AccessFlags::None,
            .label = "culled instances",
        });
        const RenderGraphTextureHandle output = graph.import_texture(RenderGraphImportedTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 64, .height = 64, .depth_or_layers = 1},
            .label = "buffer dependency output",
        });
        graph.mark_output(output);

        graph.add_compute_pass("produce culled instances")
            .add_buffer(RenderGraphBufferAccessDesc{
                .buffer = culled_instances,
                .stages = SFT::RHI::PipelineStage::ComputeShader,
                .access = SFT::RHI::AccessFlags::ShaderWrite,
                .read = false,
                .write = true,
            });
        graph.add_render_pass("consume culled instances")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = output})
            .add_buffer(RenderGraphBufferAccessDesc{
                .buffer = culled_instances,
                .stages = SFT::RHI::PipelineStage::VertexShader,
                .access = SFT::RHI::AccessFlags::ShaderRead,
            });

        const RenderGraph::CompileResult compiled = graph.compile();
        bool passed = check(compiled.has_value(), "buffer producer/consumer graph failed to compile");
        if (!compiled) {
            return false;
        }
        passed &= check(compiled->order.size() == 2,
                        "buffer producer was culled despite feeding a live render pass");
        passed &= check(compiled->order[0].kind == RenderGraph::PassKind::Compute &&
                        compiled->order[1].kind == RenderGraph::PassKind::Render,
                        "buffer dependency did not preserve producer-before-consumer order");
        passed &= check(compiled->levels.size() == 2 &&
                        compiled->levels[0] == 0 && compiled->levels[1] == 1,
                        "buffer hazard did not serialize parallel recording levels");
        return passed;
    }

    bool transient_chain_contributes_to_compile_levels() {
        using namespace SFT::Renderer;

        RenderGraph graph;
        const RenderGraphTextureHandle first = graph.create_texture(RenderGraphTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 64, .height = 64, .depth_or_layers = 1},
            .label = "first transient",
        });
        const RenderGraphTextureHandle second = graph.create_texture(RenderGraphTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 64, .height = 64, .depth_or_layers = 1},
            .label = "second transient",
        });
        const RenderGraphTextureHandle output = graph.import_texture(RenderGraphImportedTextureDesc{
            .format = SFT::RHI::Format::RGBA16Float,
            .extent = SFT::RHI::Extent3D{.width = 64, .height = 64, .depth_or_layers = 1},
            .label = "external output",
        });
        graph.mark_output(output);

        graph.add_render_pass("produce first")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = first});
        graph.add_render_pass("produce second")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = second})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = first});
        graph.add_render_pass("export")
            .add_color_attachment(RenderGraphColorAttachmentDesc{.texture = output})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = second});

        const RenderGraph::CompileResult compiled = graph.compile();
        bool passed = check(compiled.has_value(), "transient chain failed to compile");
        if (!compiled) {
            return false;
        }
        passed &= check(compiled->order.size() == 3, "live transient chain was culled");
        passed &= check(compiled->levels.size() == 3, "compiled level array has the wrong size");
        passed &= check(compiled->levels[0] == 0 && compiled->levels[1] == 1 && compiled->levels[2] == 2,
                        "transient producer/consumer dependencies did not serialize recording levels");
        return passed;
    }

} // namespace

int main() {
    const bool passed = semantic_blackboard_is_typed_and_reusable() &&
                        explicit_outputs_control_liveness() &&
                        invalid_buffer_ranges_and_access_intent_are_rejected() &&
                        imported_buffers_drive_dependencies_and_liveness() &&
                        transient_chain_contributes_to_compile_levels();
    return passed ? 0 : 1;
}
