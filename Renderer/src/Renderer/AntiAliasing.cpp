#include <Renderer/AntiAliasing.hpp>

#include <cstddef>
#include <span>

#include <Renderer/FullscreenPass.hpp>
#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    namespace {
        // Must match the push-constant block in Shaders/fullscreen_anti_aliasing.slang.
        struct PostProcessAaConstants {
            u32 mode = 0;
            f32 subpixel = 0.75f;
            f32 edge_threshold = 0.125f;
        };
        static_assert(sizeof(PostProcessAaConstants) == 12);
    } // namespace

    CustomPostProcessEffect post_process_aa_effect(const RenderGraphSettings &settings) {
        const PostProcessAaConstants constants{
            .mode = static_cast<u32>(settings.frame.anti_aliasing.post_process),
            .subpixel = settings.frame.anti_aliasing.subpixel_quality,
            .edge_threshold = settings.frame.anti_aliasing.edge_threshold,
        };
        CustomPostProcessEffect effect{
            .shader_path = "Shaders/fullscreen_anti_aliasing.slang",
            .module_name = "fullscreen_anti_aliasing",
            .fragment_entry_point = "fragmentMain",
            .label = UString{"scene-linear spatial anti-aliasing"_ustr},
            .stage = PostProcessStage::BeforeBloom,
        };
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const PostProcessAaConstants>{&constants, 1});
        effect.push_constants.assign(bytes.begin(), bytes.end());
        return effect;
    }

    Core::RendererResult add_post_process_aa_pass(Renderer &renderer, RenderGraph &graph,
                                                  std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                  RenderGraphTextureHandle source, RenderGraphTextureHandle destination,
                                                  Core::Extent2D extent, RHI::Format format, const RenderGraphSettings &settings) {
        if (settings.frame.anti_aliasing.post_process == RenderSettings::PostProcessAntiAliasing::None) {
            return {};
        }
        return add_fullscreen_effect_pass(renderer, graph, transient_bind_groups,
                                          FullscreenPassDescription{
                                              .label = "scene-linear spatial anti-aliasing",
                                              .destination = destination,
                                              .destination_format = format,
                                              .extent = extent,
                                              .load_op = RHI::LoadOp::DontCare,
                                              .source = source,
                                              .effect = post_process_aa_effect(settings),
                                          });
    }

} // namespace SFT::Renderer
