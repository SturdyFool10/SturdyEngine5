#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <expected>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>
#include <Text/Text.hpp>

using std::array;
using std::optional;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        constexpr f32 overlay_pixel_size = 16.0f;
        constexpr f32 overlay_line_height = 20.0f;

        [[nodiscard]] optional<vector<std::byte>> read_file_bytes(const string &path) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::nullopt;
            }
            const std::streamsize size = file.tellg();
            if (size <= 0) {
                return std::nullopt;
            }
            file.seekg(0);
            vector<std::byte> bytes(static_cast<usize>(size));
            if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
                return std::nullopt;
            }
            return bytes;
        }

        // Best-effort default UI font pick: Maple Mono NF (the Nerd-Font-patched build of Maple
        // Mono) is preferred when installed, falling back to a handful of common family names
        // likely to exist on any of this engine's target OSes, then to whatever discovery found
        // first.
        [[nodiscard]] optional<string> find_default_font_path() {
            const vector<string> search_dirs = Platform::font_search_directories();
            const Text::FontDatabase database =
                Text::FontDatabase::create(span<const string>{search_dirs.data(), search_dirs.size()});

            static constexpr array<const char *, 6> preferred_families{
                "Maple Mono NF", "DejaVu Sans", "Noto Sans", "Liberation Sans", "Arial", "Helvetica",
            };
            for (const char *family : preferred_families) {
                if (optional<string> path = database.find(family)) {
                    return path;
                }
            }
            if (!database.faces().empty()) {
                return database.faces().front().file_path;
            }
            return std::nullopt;
        }

    } // namespace

    Core::RendererResult Renderer::ensure_text_overlay_resources() {
        auto guard = text_overlay_.lock();
        if (guard->ready) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot build the debug text overlay without an RHI device.");
        }

        optional<string> font_path = find_default_font_path();
        if (!font_path) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "No usable font was found for the debug text overlay.");
        }
        optional<vector<std::byte>> font_bytes = read_file_bytes(*font_path);
        if (!font_bytes) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Failed to read the debug text overlay font file: " + *font_path);
        }

        auto font = Text::Font::load(span<const std::byte>{font_bytes->data(), font_bytes->size()});
        if (!font) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Failed to parse the debug text overlay font: " + *font_path);
        }

        auto atlas = TextAtlas::create(*device, TextAtlas::Config{});
        if (!atlas) {
            return unexpected(atlas.error());
        }

        auto pipeline = TextPipeline::create(*device, RHI::Format::BGRA8UnormSrgb);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }

        guard->font = std::move(*font);
        guard->atlas = std::move(*atlas);
        guard->pipeline = std::move(*pipeline);
        guard->font_id = 1;
        guard->outline_cache.clear();
        guard->ready = true;
        return {};
    }

    Core::RendererResult Renderer::prepare_text_overlay(RHI::CommandEncoder &encoder, span<const string> lines,
                                                        glm::vec2 origin_px, vector<RHI::BufferHandle> &transient_buffers,
                                                        vector<TextDrawBatch> &out_batches) {
        out_batches.clear();
        if (Core::RendererResult ensured = ensure_text_overlay_resources(); !ensured.has_value()) {
            return ensured;
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot draw the debug text overlay without an RHI device.");
        }

        auto guard = text_overlay_.lock();
        if (!guard->ready) {
            return {};
        }

        const u32 units_per_em = guard->font.units_per_em();
        const f32 scale = units_per_em > 0 ? overlay_pixel_size / static_cast<f32>(units_per_em) : 0.0f;

        vector<GlyphPlacement> placements;
        glm::vec2 pen = origin_px;
        for (const string &line : lines) {
            auto shaped = Text::shape(guard->font, line);
            if (!shaped) {
                pen.y += overlay_line_height;
                continue;
            }

            glm::vec2 cursor = pen;
            for (const Text::PositionedGlyph &glyph : *shaped) {
                auto cached = guard->outline_cache.find(glyph.glyph_id);
                if (cached == guard->outline_cache.end()) {
                    if (auto outline = Text::glyph_outline(guard->font, glyph.glyph_id)) {
                        cached = guard->outline_cache.emplace(glyph.glyph_id, std::move(*outline)).first;
                    } else {
                        cached = guard->outline_cache.emplace(glyph.glyph_id, Text::GlyphOutline{}).first;
                    }
                }

                placements.push_back(GlyphPlacement{
                    .position = glm::vec2{cursor.x + glyph.x_offset * scale, cursor.y - glyph.y_offset * scale},
                    .size = glm::vec2{overlay_pixel_size, overlay_pixel_size},
                    .color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
                    .font_id = guard->font_id,
                    .glyph_id = glyph.glyph_id,
                    .units_per_em = units_per_em,
                    .pixel_size = overlay_pixel_size,
                    .format = Text::select_raster_format(overlay_pixel_size),
                    .outline = &cached->second,
                    .font = &guard->font,
                });

                cursor.x += glyph.x_advance * scale;
                cursor.y -= glyph.y_advance * scale;
            }
            pen.y += overlay_line_height;
        }

        if (placements.empty()) {
            return {};
        }

        vector<GlyphRequest> requests;
        requests.reserve(placements.size());
        for (const GlyphPlacement &placement : placements) {
            requests.push_back(GlyphRequest{
                .font_id = placement.font_id,
                .glyph_id = placement.glyph_id,
                .units_per_em = placement.units_per_em,
                .pixel_size = placement.pixel_size,
                .format = placement.format,
                .outline = placement.outline,
                .font = placement.font,
            });
        }

        vector<GlyphSlot> slots;
        if (auto resident = guard->atlas.ensure_resident(*device, encoder, requests, slots, transient_buffers); !resident) {
            return unexpected(resident.error());
        }

        vector<GlyphInstance> instances;
        instances.reserve(placements.size());
        for (usize i = 0; i < placements.size(); ++i) {
            const GlyphSlot &slot = slots[i];
            const f32 instance_scale = slot.cell_size_px > 0.0f ? placements[i].size.x / slot.cell_size_px : 1.0f;
            instances.push_back(GlyphInstance{
                .position = placements[i].position,
                .size = placements[i].size,
                .uv_min = slot.uv_min,
                .uv_max = slot.uv_max,
                .color = placements[i].color,
                .format_kind = format_kind_value(slot.format),
                .screen_px_range = guard->atlas.pixel_range() * instance_scale,
            });
        }

        return guard->pipeline.prepare(*device, instances, slots, out_batches, transient_buffers);
    }

    Core::RendererResult Renderer::draw_text_overlay(RHI::RenderPassEncoder &pass, span<const TextDrawBatch> batches,
                                                      glm::vec2 viewport_size_px,
                                                      vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (batches.empty()) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot draw the debug text overlay without an RHI device.");
        }

        auto guard = text_overlay_.lock();
        if (!guard->ready) {
            return {};
        }

        return guard->pipeline.draw(*device, pass, guard->atlas, batches, viewport_size_px, transient_bind_groups);
    }

    void Renderer::destroy_text_overlay_resources_locked(TextOverlayResources &resources) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            resources.pipeline.destroy(*device);
            resources.atlas.destroy(*device);
        }
        resources.outline_cache.clear();
        resources.ready = false;
    }

    void Renderer::destroy_text_overlay_resources() noexcept {
        auto guard = text_overlay_.lock();
        destroy_text_overlay_resources_locked(*guard);
    }

} // namespace SFT::Renderer
