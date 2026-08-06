#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <span>

#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::span;

namespace SFT::Renderer {
    namespace {
        struct PostProcessAaConstants {
            u32 mode = 0;
            f32 subpixel = 0.75f;
            f32 edge_threshold = 0.125f;
        };
        static_assert(sizeof(PostProcessAaConstants) == 12);

        [[nodiscard]] CustomPostProcessEffect post_process_aa_effect(const RenderGraphSettings &settings) {
            const PostProcessAaConstants constants{
                .mode = settings.post_process_aa,
                .subpixel = settings.aa_subpixel_quality,
                .edge_threshold = settings.aa_edge_threshold,
            };
            CustomPostProcessEffect effect{
                .shader_path = "Shaders/fullscreen_anti_aliasing.slang",
                .module_name = "fullscreen_anti_aliasing",
                .fragment_entry_point = "fragmentMain",
                .label = UString{"scene-linear spatial anti-aliasing"_ustr},
                .stage = PostProcessStage::BeforeBloom,
            };
            const span<const PostProcessAaConstants> values{&constants, 1};
            const span<const std::byte> bytes = std::as_bytes(values);
            effect.push_constants.assign(bytes.begin(), bytes.end());
            return effect;
        }
    } // namespace

    Core::RendererResult Renderer::ensure_post_process_aa_resources(
        const RenderGraphSettings &settings,
        RHI::Format color_format) {
        ZoneScopedN("Renderer::ensure_post_process_aa_resources");
        if (settings.post_process_aa == 0) {
            return {};
        }
        return ensure_custom_post_process(post_process_aa_effect(settings), color_format);
    }

    Core::RendererResult Renderer::record_post_process_aa(
        RHI::RenderPassEncoder &pass,
        RHI::TextureViewHandle source_view,
        RHI::Format color_format,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_post_process_aa");
        if (settings.post_process_aa == 0) {
            return {};
        }
        return record_custom_post_process(
            pass,
            source_view,
            color_format,
            post_process_aa_effect(settings),
            transient_bind_groups);
    }
} // namespace SFT::Renderer
