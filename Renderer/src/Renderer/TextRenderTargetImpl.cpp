#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <expected>
#include <glm/vec2.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/TextRenderTarget.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>
#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    Core::RendererExpected<TextRenderTarget> TextRenderTarget::create(RHI::RhiDevice &device, const Config &config) {
        if (config.width == 0 || config.height == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Text render target size must be non-zero."});
        }
        const RHI::DeviceLimits &limits = device.limits();
        if (limits.max_texture_dimension_2d != 0 &&
            (config.width > limits.max_texture_dimension_2d || config.height > limits.max_texture_dimension_2d)) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Text render target size exceeds the device's max 2D image dimension; use Renderer::TextCanvas for a surface this large.",
            });
        }

        TextRenderTarget target;
        target.config_ = config;

        auto texture = device.create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = config.format,
            .extent = RHI::Extent3D{.width = config.width, .height = config.height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::ColorAttachment,
            .label = "text render target",
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create text render target texture"));
        }
        target.texture_ = *texture;

        auto view = device.create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = "text render target view",
        });
        if (!view) {
            device.destroy_texture(*texture);
            return unexpected(graphics_error_from_rhi(view.error(), "create text render target view"));
        }
        target.view_ = *view;

        auto sampler = device.create_sampler(RHI::SamplerDesc{.label = "text render target sampler"});
        if (!sampler) {
            device.destroy_texture_view(*view);
            device.destroy_texture(*texture);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create text render target sampler"));
        }
        target.sampler_ = *sampler;

        return target;
    }

    Core::RendererResult TextRenderTarget::render(RHI::RhiDevice &device, TextAtlas &atlas, TextPipeline &pipeline,
                                                  span<const GlyphPlacement> glyphs) {
        // Standalone one-shot render (not part of any per-frame render graph), so — unlike the
        // main frame's shared encoder — it owns and submits/waits on its own command encoder for
        // its whole lifetime, atlas upload included.
        auto encoder = device.create_command_encoder(RHI::CommandEncoderDesc{.label = "text render target render"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create text render target encoder"));
        }
        vector<RHI::BufferHandle> transient_buffers;
        TextAtlasRetiredResources retired_atlas_resources;

        if (resources_pipeline_ != nullptr && resources_pipeline_ != &pipeline) {
            destroy_text_frame_resources(device, text_resources_);
        }
        resources_pipeline_ = &pipeline;

        vector<GlyphSlot> slots;
        vector<GlyphInstance> instances;
        if (!glyphs.empty()) {
            vector<GlyphRequest> requests;
            requests.reserve(glyphs.size());
            for (const GlyphPlacement &glyph : glyphs) {
                requests.push_back(GlyphRequest{
                    .font_id = glyph.font_id,
                    .glyph_id = glyph.glyph_id,
                    .units_per_em = glyph.units_per_em,
                    .pixel_size = glyph.pixel_size,
                    .format = glyph.format,
                    .outline = glyph.outline,
                    .font = glyph.font,
                });
            }
            if (auto resident = atlas.ensure_resident(device, **encoder, requests, slots, transient_buffers,
                                                      retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }

            instances.reserve(glyphs.size());
            for (usize i = 0; i < glyphs.size(); ++i) {
                instances.push_back(make_glyph_instance(glyphs[i].position, glyphs[i], slots[i], atlas.pixel_range()));
            }
        }

        vector<TextDrawBatch> batches;
        if (auto prepared = pipeline.prepare(device, atlas, instances, slots, text_resources_, batches); !prepared) {
            return unexpected(prepared.error());
        }

        const RHI::TextureBarrier to_attachment{
            .texture = texture_,
            .src_stage = current_layout_ == RHI::TextureLayout::Undefined ? RHI::PipelineStage::None : RHI::PipelineStage::FragmentShader,
            .src_access = current_layout_ == RHI::TextureLayout::Undefined ? RHI::AccessFlags::None : RHI::AccessFlags::ShaderRead,
            .dst_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .dst_access = RHI::AccessFlags::ColorAttachmentWrite,
            .old_layout = current_layout_,
            .new_layout = RHI::TextureLayout::ColorAttachment,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_attachment, 1});
        current_layout_ = RHI::TextureLayout::ColorAttachment;

        const RHI::ColorAttachment color_attachment{
            .view = view_,
            .load_op = RHI::LoadOp::Clear,
            .store_op = RHI::StoreOp::Store,
            .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
        };
        const RHI::RenderPassDesc pass_desc{
            .color_attachments = span<const RHI::ColorAttachment>{&color_attachment, 1},
            .render_area = RHI::Rect2D{.x = 0, .y = 0, .width = config_.width, .height = config_.height},
            .label = "text render target",
        };
        auto pass = (*encoder)->begin_render_pass(pass_desc);
        if (!pass) {
            return unexpected(graphics_error_from_rhi(pass.error(), "begin text render target pass"));
        }
        (*pass)->set_viewport(RHI::Viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<f32>(config_.width), .height = static_cast<f32>(config_.height)});
        (*pass)->set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = config_.width, .height = config_.height});

        Core::RendererResult draw_result = pipeline.draw(
            **pass, batches, glm::vec2{static_cast<f32>(config_.width), static_cast<f32>(config_.height)});
        (*pass)->end();
        if (!draw_result) {
            return draw_result;
        }

        const RHI::TextureBarrier to_sampled{
            .texture = texture_,
            .src_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .src_access = RHI::AccessFlags::ColorAttachmentWrite,
            .dst_stage = RHI::PipelineStage::FragmentShader,
            .dst_access = RHI::AccessFlags::ShaderRead,
            .old_layout = RHI::TextureLayout::ColorAttachment,
            .new_layout = RHI::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});
        current_layout_ = RHI::TextureLayout::ShaderReadOnly;

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish text render target encoder"));
        }
        auto fence = device.create_fence(RHI::FenceDesc{.label = "text render target fence"});
        if (!fence) {
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create text render target fence"));
        }
        const array command_buffers{*command_buffer};
        const RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "text render target submit",
        };
        if (auto submitted = device.submit(submit_desc); !submitted) {
            device.destroy_fence(*fence);
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit text render target render"));
        }
        if (auto waited = device.wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true); !waited) {
            device.destroy_fence(*fence);
            device.destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(waited.error(), "wait text render target fence"));
        }
        device.destroy_fence(*fence);
        device.destroy_command_buffer(*command_buffer);

        for (RHI::BufferHandle buffer : transient_buffers) {
            device.destroy_buffer(buffer);
        }
        for (RHI::TextureViewHandle view : retired_atlas_resources.texture_views) {
            device.destroy_texture_view(view);
        }
        for (RHI::TextureHandle texture : retired_atlas_resources.textures) {
            device.destroy_texture(texture);
        }

        return {};
    }

    void TextRenderTarget::destroy(RHI::RhiDevice &device) noexcept {
        destroy_text_frame_resources(device, text_resources_);
        if (sampler_) {
            device.destroy_sampler(sampler_);
        }
        if (view_) {
            device.destroy_texture_view(view_);
        }
        if (texture_) {
            device.destroy_texture(texture_);
        }
        *this = TextRenderTarget{};
    }

} // namespace SFT::Renderer
