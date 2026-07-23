#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <span>
#pragma endregion

#include "UiRenderer.hpp"

using std::unexpected;

namespace SFT::UI {

    Core::RendererExpected<UiRenderer> UiRenderer::create(RHI::RhiDevice &device, RHI::Format color_format) {
        UiRenderer renderer;

        auto atlas = Renderer::TextAtlas::create(device, Renderer::TextAtlas::Config{});
        if (!atlas) {
            return unexpected(atlas.error());
        }
        renderer.text_atlas_ = std::move(*atlas);

        auto text_pipeline = Renderer::TextPipeline::create(device, color_format);
        if (!text_pipeline) {
            renderer.text_atlas_.destroy(device);
            return unexpected(text_pipeline.error());
        }
        renderer.text_pipeline_ = std::move(*text_pipeline);

        auto quad_pipeline = UiQuadPipeline::create(device, color_format);
        if (!quad_pipeline) {
            renderer.text_pipeline_.destroy(device);
            renderer.text_atlas_.destroy(device);
            return unexpected(quad_pipeline.error());
        }
        renderer.quad_pipeline_ = std::move(*quad_pipeline);

        renderer.ready_ = true;
        return renderer;
    }

    Core::RendererResult UiRenderer::prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                             const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                             vector<RHI::BufferHandle> &out_transient_buffers,
                                             Renderer::TextAtlasRetiredResources &out_retired_atlas_resources) {
        text_batches_.clear();
        quad_batches_.clear();
        full_viewport_scissor_ = snapshot.full_viewport_scissor_;
        if (!ready_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "UiRenderer was not created.");
        }

        if (texture_resolver == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiRenderer::prepare requires a texture_resolver.");
        }
        if (!white_texture_) {
            // Renderer::ensure_default_white_texture() is private (Renderer's own material-fallback
            // internal, not part of its public surface) — UiRenderer creates its own 1x1 white
            // texture the same way any other texture-owning consumer would, through the public
            // create_texture() path, and caches the resulting handle for the pipeline's lifetime.
            const std::array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
            auto white_handle = texture_resolver->create_texture(1, 1, RHI::Format::RGBA8Unorm,
                                                                  span<const std::byte>{white.data(), white.size()},
                                                                  "ui quad white");
            if (!white_handle) {
                return unexpected(white_handle.error());
            }
            white_texture_ = *white_handle;
        }
        RHI::TextureViewHandle white_view{};
        if (Renderer::TextureResource *resource = texture_resolver->texture(white_texture_)) {
            white_view = resource->view;
        }
        if (!white_view) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "UiRenderer::prepare: the default white texture is no longer valid.");
        }

        vector<UiQuadInstance> quad_instances;
        vector<RHI::TextureViewHandle> quad_texture_views;
        vector<RHI::Rect2D> quad_scissors;
        quad_instances.reserve(snapshot.quads_.size());
        quad_texture_views.reserve(snapshot.quads_.size());
        quad_scissors.reserve(snapshot.quads_.size());
        for (const QuadDraw &quad : snapshot.quads_) {
            RHI::TextureViewHandle view = white_view;
            if (quad.image_ref != nullptr && texture_resolver != nullptr) {
                if (Renderer::TextureResource *resource = texture_resolver->texture(quad.image_ref->texture)) {
                    view = resource->view;
                }
            }
            quad_instances.push_back(quad.instance);
            quad_texture_views.push_back(view);
            quad_scissors.push_back(quad.scissor);
        }

        if (Core::RendererResult quad_prepared =
                quad_pipeline_.prepare(device, quad_instances, quad_texture_views, quad_scissors, quad_frame_resources_, quad_batches_);
            !quad_prepared) {
            return quad_prepared;
        }

        if (!snapshot.glyphs_.empty()) {
            vector<Renderer::GlyphRequest> requests;
            requests.reserve(snapshot.glyphs_.size());
            for (const Renderer::GlyphPlacement &placement : snapshot.glyphs_) {
                requests.push_back(Renderer::GlyphRequest{
                    .font_id = placement.font_id,
                    .glyph_id = placement.glyph_id,
                    .units_per_em = placement.units_per_em,
                    .pixel_size = placement.pixel_size,
                    .format = placement.format,
                    .outline = placement.outline,
                    .font = placement.font,
                });
            }
            vector<Renderer::GlyphSlot> slots;
            if (auto resident = text_atlas_.ensure_resident(device, encoder, requests, slots, out_transient_buffers,
                                                            out_retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }

            vector<Renderer::GlyphInstance> instances;
            instances.reserve(snapshot.glyphs_.size());
            for (usize i = 0; i < snapshot.glyphs_.size(); ++i) {
                instances.push_back(
                    Renderer::make_glyph_instance(snapshot.glyphs_[i].position, snapshot.glyphs_[i], slots[i], text_atlas_.pixel_range()));
            }
            if (Core::RendererResult text_prepared =
                    text_pipeline_.prepare(device, text_atlas_, instances, slots, text_frame_resources_, text_batches_);
                !text_prepared) {
                return text_prepared;
            }
        }

        return {};
    }

    Core::RendererResult UiRenderer::draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size) {
        if (Core::RendererResult quad_drawn = quad_pipeline_.draw(pass, quad_batches_, viewport_size); !quad_drawn) {
            return quad_drawn;
        }
        if (!text_batches_.empty()) {
            pass.set_scissor(full_viewport_scissor_);
            if (Core::RendererResult text_drawn = text_pipeline_.draw(pass, text_batches_, viewport_size); !text_drawn) {
                return text_drawn;
            }
        }
        return {};
    }

    void UiRenderer::destroy(RHI::RhiDevice &device) noexcept {
        quad_pipeline_.destroy(device);
        text_pipeline_.destroy(device);
        text_atlas_.destroy(device);
        destroy_ui_quad_frame_resources(device, quad_frame_resources_);
        destroy_text_frame_resources(device, text_frame_resources_);
        ready_ = false;
    }

} // namespace SFT::UI
