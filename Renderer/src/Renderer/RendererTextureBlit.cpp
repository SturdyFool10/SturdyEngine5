#include <Foundation/Foundation.hpp>

#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    /// Adds a render-graph pass that resamples the whole of `source` into the whole of `destination`.
    ///
    /// @param graph Render graph to add the pass to.
    /// @param source Texture read across its full extent.
    /// @param destination Texture written across its full extent.
    /// @param destination_format Format `destination` was created with.
    /// @param out_transient_bind_groups Bind groups this call creates are appended here.
    /// @param label Debug label for the added pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::add_texture_blit_pass(RenderGraph &graph, RenderGraphTextureHandle source,
                                                          RenderGraphTextureHandle destination,
                                                          RHI::Format destination_format,
                                                          vector<RHI::BindGroupHandle> &out_transient_bind_groups,
                                                          const ustr &label) {
        ZoneScopedN("Renderer::add_texture_blit_pass");

        // A pass declared purely as data (source, destination, format) so RenderGraph's own compile
        // step -- barrier insertion, aliasing, transient lifetime -- treats this exactly like any
        // other render pass. The actual sampling happens in the execute callback below, which is the
        // only part that has to run once destination's real extent is known.
        graph.add_render_pass(label)
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                // The whole attachment is about to be overwritten by the blit itself, so there is
                // nothing worth preserving from whatever it held before.
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .set_execute([this, source, destination, destination_format,
                          &out_transient_bind_groups](RenderGraphContext &context) -> Core::RendererResult {
                const RenderGraphTextureAccess destination_access = context.texture(destination);
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(destination_access.extent.width),
                    .height = static_cast<f32>(destination_access.extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_access.extent.width,
                    .height = destination_access.extent.height,
                });

                static const CustomPostProcessEffect blit_effect{
                    .shader_path = "Shaders/fullscreen_blit.slang",
                    .module_name = "fullscreen_blit",
                    .fragment_entry_point = "fragmentMain",
                    .label = UString{"portable texture blit"_ustr},
                };
                return record_custom_post_process(pass, context.texture(source).default_view,
                                                  destination_format, blit_effect, out_transient_bind_groups);
            });
        return {};
    }

} // namespace SFT::Renderer
