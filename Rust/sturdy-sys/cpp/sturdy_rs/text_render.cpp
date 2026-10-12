#include "sturdy_rs/text_render.hpp"

#include <array>
#include <cstring>
#include <string>

#include <Core/Core.hpp>
#include <Engine/Engine.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/Text/Text.hpp>

#include "sturdy_rs/text.hpp"

// Generated from src/text_render.rs.
#include "sturdy_rs/bridge.h"
#include "sturdy_rs/text_render_bridge.h"

namespace sturdy_rs::text_render {
    namespace {

        namespace R = SFT::RHI;
        namespace Rd = SFT::Renderer;

        StatusResult fail(std::string message) {
            return StatusResult{false, rust::String::lossy(message)};
        }

        template <class E>
        std::string describe(const E &error) {
            if constexpr (requires { error.message; }) {
                const auto &message = error.message;
                if constexpr (requires { message.cpp_string_view(); }) {
                    const auto view = message.cpp_string_view();
                    return std::string{view.data(), view.size()};
                } else {
                    return std::string{message};
                }
            }
            return "operation failed";
        }

        std::uint32_t bytes_per_pixel(R::Format format) noexcept {
            switch (format) {
            case R::Format::R8Unorm:
            case R::Format::R8Snorm:
            case R::Format::R8Uint:
            case R::Format::R8Sint: return 1;
            case R::Format::RG8Unorm:
            case R::Format::RG8Snorm:
            case R::Format::RG8Uint:
            case R::Format::RG8Sint:
            case R::Format::R16Float: return 2;
            case R::Format::RGBA8Unorm:
            case R::Format::RGBA8UnormSrgb:
            case R::Format::RGBA8Snorm:
            case R::Format::RGBA8Uint:
            case R::Format::RGBA8Sint:
            case R::Format::BGRA8Unorm:
            case R::Format::BGRA8UnormSrgb:
            case R::Format::RGB10A2Unorm:
            case R::Format::RG16Float:
            case R::Format::R32Float: return 4;
            case R::Format::RGBA16Float: return 8;
            case R::Format::RGBA32Float: return 16;
            default: return 0;
            }
        }

        Rd::TextPipeline *pipeline_for(TextRendererBox &renderer, R::Format format, std::string &error) {
            if (const auto found = renderer.pipelines.find(format); found != renderer.pipelines.end()) {
                return found->second.get();
            }
            auto created = Rd::TextPipeline::create(*renderer.device, format);
            if (!created) {
                error = describe(created.error());
                return nullptr;
            }
            auto owned = std::make_unique<Rd::TextPipeline>(std::move(*created));
            Rd::TextPipeline *raw = owned.get();
            renderer.pipelines.emplace(format, std::move(owned));
            return raw;
        }

        /// Resolves glyph descriptions into engine placements (font pointers from the text registry,
        /// outlines from the renderer's persistent cache). Returns an error message, or empty.
        std::string build_placements(TextRendererBox &renderer, rust::Slice<const GlyphPlacementDesc> glyphs,
                                     std::vector<Rd::GlyphPlacement> &placements) {
            placements.reserve(glyphs.size());
            for (const GlyphPlacementDesc &g : glyphs) {
                const SFT::Text::Font *font = text::registry_font(g.font_id);
                if (font == nullptr) {
                    return "a glyph references an unknown font id";
                }
                Rd::GlyphPlacement placement{};
                placement.position = {g.x, g.y};
                placement.size = {g.pixel_size, g.pixel_size};
                placement.rotation = g.rotation;
                placement.color = {g.color[0], g.color[1], g.color[2], g.color[3]};
                placement.font_id = g.font_id;
                placement.glyph_id = g.glyph_id;
                placement.units_per_em = g.units_per_em;
                placement.pixel_size = g.pixel_size;
                placement.format = g.is_color ? SFT::Text::RasterFormat::Color
                                              : SFT::Text::select_raster_format(g.pixel_size);
                placement.font = font;
                placement.stem_darkening = g.stem_darkening;
                if (!g.is_color) {
                    const auto key = std::make_pair(g.font_id, g.glyph_id);
                    auto cached = renderer.outlines.find(key);
                    if (cached == renderer.outlines.end()) {
                        auto outline = SFT::Text::glyph_outline(*font, g.glyph_id);
                        cached = renderer.outlines
                                     .emplace(key, outline ? std::move(*outline) : SFT::Text::GlyphOutline{})
                                     .first;
                    }
                    placement.outline = &cached->second;
                }
                placements.push_back(placement);
            }
            return {};
        }

        /// Copies a shader-readable texture back to the CPU (leaves it ShaderReadOnly).
        PixelsResult read_texture(R::RhiDevice &device, R::TextureHandle texture, std::uint32_t width,
                                  std::uint32_t height, R::Format format);

        TargetInfo info_of(const TextRendererBox &renderer, std::uint64_t id) {
            TargetInfo info{};
            const auto found = renderer.targets.find(id);
            if (found == renderer.targets.end()) {
                return info;
            }
            const Rd::TextRenderTarget &target = found->second;
            info.ok = true;
            info.id = id;
            info.texture = target.texture().value;
            info.view = target.view().value;
            info.sampler = target.sampler().value;
            info.width = target.width();
            info.height = target.height();
            info.format = static_cast<std::uint8_t>(renderer.target_formats.at(id));
            return info;
        }

    } // namespace

    TextRendererBox::~TextRendererBox() {
        if (!destroyed && device != nullptr) {
            text_renderer_destroy(*this);
        }
    }

    std::unique_ptr<TextRendererBox> text_renderer_new(EngineView &engine, const AtlasConfigDesc &config) {
        R::RhiDevice *device = engine.rhi_device();
        if (device == nullptr) {
            return nullptr;
        }
        Rd::TextAtlas::Config atlas_config{};
        atlas_config.initial_image_size = config.initial_image_size;
        atlas_config.maximum_image_size = config.maximum_image_size;
        atlas_config.pixel_range = config.pixel_range;
        atlas_config.padding_px = config.padding_px;
        auto atlas = Rd::TextAtlas::create(*device, atlas_config);
        if (!atlas) {
            return nullptr;
        }
        auto renderer = std::make_unique<TextRendererBox>();
        renderer->device = device;
        renderer->atlas = std::move(*atlas);
        return renderer;
    }

    void text_renderer_destroy(TextRendererBox &renderer) {
        if (renderer.destroyed || renderer.device == nullptr) {
            return;
        }
        for (auto &[id, canvas] : renderer.canvases) {
            canvas.destroy(*renderer.device);
        }
        renderer.canvases.clear();
        renderer.canvas_textures.clear();
        if (renderer.canvas_sampler) {
            renderer.device->destroy_sampler(renderer.canvas_sampler);
            renderer.canvas_sampler = {};
        }
        for (auto &[id, target] : renderer.targets) {
            target.destroy(*renderer.device);
        }
        renderer.targets.clear();
        renderer.target_formats.clear();
        for (auto &[format, pipeline] : renderer.pipelines) {
            pipeline->destroy(*renderer.device);
        }
        renderer.pipelines.clear();
        renderer.atlas.destroy(*renderer.device);
        renderer.destroyed = true;
    }

    TargetResult text_target_create(TextRendererBox &renderer, std::uint32_t width, std::uint32_t height, std::uint8_t format) {
        TargetResult out{};
        if (renderer.destroyed) {
            out.error = rust::String("text renderer was destroyed");
            return out;
        }
        const auto rhi_format = static_cast<R::Format>(format);
        std::string error;
        if (pipeline_for(renderer, rhi_format, error) == nullptr) {
            out.error = rust::String::lossy(error);
            return out;
        }
        auto target = Rd::TextRenderTarget::create(*renderer.device,
                                                   Rd::TextRenderTarget::Config{width, height, rhi_format});
        if (!target) {
            out.error = rust::String::lossy(describe(target.error()));
            return out;
        }
        const std::uint64_t id = renderer.next_target++;
        renderer.targets.emplace(id, std::move(*target));
        renderer.target_formats.emplace(id, rhi_format);
        out.info = info_of(renderer, id);
        return out;
    }

    TargetInfo text_target_info(const TextRendererBox &renderer, std::uint64_t id) {
        return info_of(renderer, id);
    }

    StatusResult text_target_render(TextRendererBox &renderer, std::uint64_t id, rust::Slice<const GlyphPlacementDesc> glyphs) {
        const auto found = renderer.targets.find(id);
        if (renderer.destroyed || found == renderer.targets.end()) {
            return fail("unknown text target");
        }
        std::string error;
        Rd::TextPipeline *pipeline = pipeline_for(renderer, renderer.target_formats.at(id), error);
        if (pipeline == nullptr) {
            return fail(error);
        }
        std::vector<Rd::GlyphPlacement> placements;
        if (std::string built = build_placements(renderer, glyphs, placements); !built.empty()) {
            return fail(built);
        }
        // `outlines` is a std::map: node addresses stay valid while placements point into it.
        auto rendered = found->second.render(*renderer.device, renderer.atlas, *pipeline, placements);
        if (!rendered) {
            return fail(describe(rendered.error()));
        }
        return StatusResult{true, rust::String()};
    }

    namespace {
        PixelsResult read_texture(R::RhiDevice &device, R::TextureHandle texture, std::uint32_t width,
                                  std::uint32_t height, R::Format format) {
            PixelsResult out{};
            const std::uint32_t bpp = bytes_per_pixel(format);
            if (bpp == 0) {
                out.error = rust::String("readback is not supported for this format");
                return out;
            }
        const std::uint64_t size = static_cast<std::uint64_t>(width) * height * bpp;

        auto buffer = device.create_buffer(R::BufferDesc{
            .size = size, .usage = R::BufferUsage::TransferDst, .memory = R::MemoryLocation::HostReadback,
            .label = "text target readback"});
        if (!buffer) {
            out.error = rust::String::lossy(describe(buffer.error()));
            return out;
        }
        auto encoder = device.create_command_encoder(R::CommandEncoderDesc{.label = "text target readback"});
        if (!encoder) {
            device.destroy_buffer(*buffer);
            out.error = rust::String::lossy(describe(encoder.error()));
            return out;
        }
        // `render()` leaves the target shader-readable; move it to transfer-source and back.
        const R::TextureBarrier to_transfer{
            .texture = texture,
            .src_stage = R::PipelineStage::FragmentShader,
            .src_access = R::AccessFlags::ShaderRead,
            .dst_stage = R::PipelineStage::Transfer,
            .dst_access = R::AccessFlags::TransferRead,
            .old_layout = R::TextureLayout::ShaderReadOnly,
            .new_layout = R::TextureLayout::TransferSrc,
        };
        (*encoder)->barrier({}, {}, std::span<const R::TextureBarrier>{&to_transfer, 1});
        R::BufferTextureCopy copy{};
        copy.buffer_offset = 0;
        copy.buffer_row_length = width;
        copy.buffer_image_height = height;
        copy.mip_level = 0;
        copy.base_array_layer = 0;
        copy.array_layer_count = 1;
        copy.texture_offset = {0, 0, 0};
        copy.texture_extent = {width, height, 1};
        (*encoder)->copy_texture_to_buffer(texture, *buffer, copy);
        const R::TextureBarrier back{
            .texture = texture,
            .src_stage = R::PipelineStage::Transfer,
            .src_access = R::AccessFlags::TransferRead,
            .dst_stage = R::PipelineStage::FragmentShader,
            .dst_access = R::AccessFlags::ShaderRead,
            .old_layout = R::TextureLayout::TransferSrc,
            .new_layout = R::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, std::span<const R::TextureBarrier>{&back, 1});
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            device.destroy_buffer(*buffer);
            out.error = rust::String::lossy(describe(command_buffer.error()));
            return out;
        }
        auto fence = device.create_fence(R::FenceDesc{.label = "text target readback fence"});
        if (!fence) {
            device.destroy_command_buffer(*command_buffer);
            device.destroy_buffer(*buffer);
            out.error = rust::String::lossy(describe(fence.error()));
            return out;
        }
        const std::array command_buffers{*command_buffer};
        auto submitted = device.submit(R::SubmitDesc{
            .command_buffers = std::span<const R::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = R::SubmitFlags::OneShot,
            .label = "text target readback submit"});
        bool ok = submitted.has_value();
        if (ok) {
            auto waited = device.wait_fences(std::span<const R::FenceHandle>{&*fence, 1}, true);
            ok = waited.has_value() && *waited;
        }
        device.destroy_fence(*fence);
        device.destroy_command_buffer(*command_buffer);
        if (!ok) {
            device.destroy_buffer(*buffer);
            out.error = rust::String("text target readback submission failed");
            return out;
        }
        auto mapped = device.map_buffer(*buffer);
        if (!mapped) {
            device.destroy_buffer(*buffer);
            out.error = rust::String::lossy(describe(mapped.error()));
            return out;
        }
        out.ok = true;
        out.width = width;
        out.height = height;
        out.bytes_per_pixel = bpp;
        out.pixels.reserve(mapped->size());
        for (std::byte b : *mapped) {
            out.pixels.push_back(static_cast<std::uint8_t>(b));
        }
        device.unmap_buffer(*buffer);
        device.destroy_buffer(*buffer);
        return out;
        }
    } // namespace

    PixelsResult text_target_read(TextRendererBox &renderer, std::uint64_t id) {
        const auto found = renderer.targets.find(id);
        if (renderer.destroyed || found == renderer.targets.end()) {
            PixelsResult out{};
            out.error = rust::String("unknown text target");
            return out;
        }
        return read_texture(*renderer.device, found->second.texture(), found->second.width(), found->second.height(),
                            renderer.target_formats.at(id));
    }

    std::uint64_t text_target_adopt(const TextRendererBox &renderer, EngineView &engine, std::uint64_t id) {
        const auto found = renderer.targets.find(id);
        Rd::Renderer *rd = engine.renderer();
        if (found == renderer.targets.end() || rd == nullptr) {
            return 0;
        }
        const Rd::TextRenderTarget &target = found->second;
        auto adopted = rd->adopt_texture(target.texture(), target.view(), target.sampler(), "text target");
        if (!adopted) {
            return 0;
        }
        if (Rd::TextureResource *resource = rd->texture(*adopted)) {
            resource->width = target.width();
            resource->height = target.height();
            resource->format = renderer.target_formats.at(id);
            resource->mip_levels = 1;
        }
        return adopted->value;
    }

    bool text_target_destroy(TextRendererBox &renderer, std::uint64_t id) {
        const auto found = renderer.targets.find(id);
        if (found == renderer.targets.end()) {
            return false;
        }
        found->second.destroy(*renderer.device);
        renderer.targets.erase(found);
        renderer.target_formats.erase(id);
        return true;
    }

    CanvasResult text_canvas_create(TextRendererBox &renderer, std::uint32_t desired_tile_size,
                                    std::uint32_t max_resident_tiles) {
        CanvasResult out{};
        if (renderer.destroyed) {
            out.error = rust::String("text renderer was destroyed");
            return out;
        }
        std::string error;
        Rd::TextPipeline *pipeline = pipeline_for(renderer, R::Format::RGBA8Unorm, error);
        if (pipeline == nullptr) {
            out.error = rust::String::lossy(error);
            return out;
        }
        Rd::TextCanvas::Config config{};
        config.desired_tile_size = desired_tile_size;
        config.max_resident_tiles = max_resident_tiles;
        auto canvas = Rd::TextCanvas::create(*renderer.device, config, renderer.atlas, *pipeline);
        if (!canvas) {
            out.error = rust::String::lossy(describe(canvas.error()));
            return out;
        }
        const std::uint64_t id = renderer.next_target++;
        out.tile_size = canvas->tile_size();
        renderer.canvases.emplace(id, std::move(*canvas));
        out.ok = true;
        out.id = id;
        return out;
    }

    StatusResult text_canvas_draw(TextRendererBox &renderer, std::uint64_t id, rust::Slice<const GlyphPlacementDesc> glyphs) {
        const auto found = renderer.canvases.find(id);
        if (renderer.destroyed || found == renderer.canvases.end()) {
            return fail("unknown text canvas");
        }
        std::vector<Rd::GlyphPlacement> placements;
        if (std::string built = build_placements(renderer, glyphs, placements); !built.empty()) {
            return fail(built);
        }
        found->second.draw_run(placements);
        return StatusResult{true, rust::String()};
    }

    CanvasTilesResult text_canvas_resident(TextRendererBox &renderer, std::uint64_t id, std::int32_t x, std::int32_t y,
                                           std::uint32_t width, std::uint32_t height) {
        CanvasTilesResult out{};
        const auto found = renderer.canvases.find(id);
        if (renderer.destroyed || found == renderer.canvases.end()) {
            out.error = rust::String("unknown text canvas");
            return out;
        }
        auto tiles = found->second.ensure_viewport_resident(
            *renderer.device, R::Rect2D{.x = x, .y = y, .width = width, .height = height});
        if (!tiles) {
            out.error = rust::String::lossy(describe(tiles.error()));
            return out;
        }
        auto &textures = renderer.canvas_textures[id];
        textures.clear();
        for (const Rd::ResidentCanvasTile &tile : *tiles) {
            CanvasTile info{};
            info.tile_x = tile.coord.x;
            info.tile_y = tile.coord.y;
            info.view = tile.view.value;
            info.logical_x = tile.logical_rect.x;
            info.logical_y = tile.logical_rect.y;
            info.logical_width = tile.logical_rect.width;
            info.logical_height = tile.logical_rect.height;
            out.tiles.push_back(info);
        }
        out.ok = true;
        return out;
    }

    std::uint64_t text_canvas_sampler(TextRendererBox &renderer) {
        if (renderer.destroyed) {
            return 0;
        }
        if (!renderer.canvas_sampler) {
            auto sampler = renderer.device->create_sampler(R::SamplerDesc{.label = "text canvas sampler"});
            if (!sampler) {
                return 0;
            }
            renderer.canvas_sampler = *sampler;
        }
        return renderer.canvas_sampler.value;
    }

    bool text_canvas_destroy(TextRendererBox &renderer, std::uint64_t id) {
        const auto found = renderer.canvases.find(id);
        if (found == renderer.canvases.end()) {
            return false;
        }
        found->second.destroy(*renderer.device);
        renderer.canvases.erase(found);
        renderer.canvas_textures.erase(id);
        return true;
    }

} // namespace sturdy_rs::text_render
