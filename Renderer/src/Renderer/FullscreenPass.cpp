#include <Renderer/FullscreenPass.hpp>

#include <span>

#include <Renderer/RendererModule.hpp>

namespace SFT::Renderer {

    Core::RendererResult add_fullscreen_effect_pass(Renderer &renderer, RenderGraph &graph,
                                                        std::vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                        FullscreenPassDescription desc) {
        if (Core::RendererResult ready = renderer.prepare_fullscreen_effect(desc.effect, desc.destination_format); !ready.has_value()) {
            return ready;
        }
        const UString pass_label{desc.label};
        auto &builder = graph.add_render_pass(pass_label);
        builder
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = desc.destination,
                .load_op = desc.load_op,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = desc.source});
        if (desc.extra_source) {
            builder.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = desc.extra_source});
        }
        builder
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = desc.extent.x, .height = desc.extent.y})
            .set_execute([&renderer, &transient_bind_groups, desc = std::move(desc)](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(desc.extent.x),
                    .height = static_cast<f32>(desc.extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = desc.extent.x, .height = desc.extent.y});
                RHI::TextureViewHandle extras[1]{};
                std::span<const RHI::TextureViewHandle> extra_views{};
                if (desc.extra_source) {
                    extras[0] = context.texture(desc.extra_source).default_view;
                    extra_views = std::span<const RHI::TextureViewHandle>{extras, 1};
                }
                return renderer.record_fullscreen_effect(pass, context.texture(desc.source).default_view, desc.destination_format,
                                                         desc.effect, transient_bind_groups, extra_views);
            });
        return {};
    }

} // namespace SFT::Renderer
