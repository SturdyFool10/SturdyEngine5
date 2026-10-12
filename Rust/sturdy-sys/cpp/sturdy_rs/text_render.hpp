// Rust <-> SturdyEngine 5 shim for GPU text rendering. See src/text_render.rs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <rust/cxx.h>

#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextCanvas.hpp>
#include <Renderer/TextInstance.hpp>
#include <Renderer/TextRenderTarget.hpp>
#include <RHI/RHI.hpp>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::text_render {

    using EngineView = sturdy_rs::EngineView;

    struct GlyphPlacementDesc;
    struct AtlasConfigDesc;
    struct TargetInfo;
    struct TargetResult;
    struct StatusResult;
    struct PixelsResult;
    struct CanvasResult;
    struct CanvasTilesResult;

    class TextRendererBox {
      public:
        SFT::RHI::RhiDevice *device = nullptr;
        SFT::Renderer::TextAtlas atlas;
        std::map<SFT::RHI::Format, std::unique_ptr<SFT::Renderer::TextPipeline>> pipelines;
        std::map<std::uint64_t, SFT::Renderer::TextRenderTarget> targets;
        std::map<std::uint64_t, SFT::RHI::Format> target_formats;
        std::map<std::pair<std::uint64_t, std::uint32_t>, SFT::Text::GlyphOutline> outlines;
        std::map<std::uint64_t, SFT::Renderer::TextCanvas> canvases;
        /// Last tile size/texture handles per canvas tile, for readback (canvas -> tile -> texture).
        std::map<std::uint64_t, std::map<std::pair<std::int32_t, std::int32_t>, std::uint64_t>> canvas_textures;
        SFT::RHI::SamplerHandle canvas_sampler{};
        std::uint64_t next_target = 1;
        bool destroyed = false;

        ~TextRendererBox();
    };

    std::unique_ptr<TextRendererBox> text_renderer_new(EngineView &engine, const AtlasConfigDesc &config);
    void text_renderer_destroy(TextRendererBox &renderer);
    TargetResult text_target_create(TextRendererBox &renderer, std::uint32_t width, std::uint32_t height, std::uint8_t format);
    TargetInfo text_target_info(const TextRendererBox &renderer, std::uint64_t id);
    StatusResult text_target_render(TextRendererBox &renderer, std::uint64_t id, rust::Slice<const GlyphPlacementDesc> glyphs);
    PixelsResult text_target_read(TextRendererBox &renderer, std::uint64_t id);
    std::uint64_t text_target_adopt(const TextRendererBox &renderer, EngineView &engine, std::uint64_t id);
    bool text_target_destroy(TextRendererBox &renderer, std::uint64_t id);
    CanvasResult text_canvas_create(TextRendererBox &renderer, std::uint32_t desired_tile_size, std::uint32_t max_resident_tiles);
    StatusResult text_canvas_draw(TextRendererBox &renderer, std::uint64_t id, rust::Slice<const GlyphPlacementDesc> glyphs);
    CanvasTilesResult text_canvas_resident(TextRendererBox &renderer, std::uint64_t id, std::int32_t x, std::int32_t y,
                                           std::uint32_t width, std::uint32_t height);
    std::uint64_t text_canvas_sampler(TextRendererBox &renderer);
    bool text_canvas_destroy(TextRendererBox &renderer, std::uint64_t id);

} // namespace sturdy_rs::text_render
