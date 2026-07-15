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

        // Maple Mono NF at 20pt/weight 400 (its only static weight), paired with Noto Color Emoji
        // as the emoji fallback — sized up a notch from Vertex's own 18pt UI default for this
        // engine's debug HUD.
        constexpr f32 overlay_pixel_size = 20.0f;
        constexpr f32 overlay_line_height = 25.0f;

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

        // "Fonts" is a bundled, engine-shipped directory (mirrors how Engine::shaders_directory
        // defaults to a relative "Shaders" folder) so a preferred family is always found even on a
        // machine that never had it installed system-wide.
        [[nodiscard]] Text::FontDatabase build_font_database() {
            vector<string> search_dirs{"Fonts"};
            const vector<string> platform_dirs = Platform::font_search_directories();
            search_dirs.insert(search_dirs.end(), platform_dirs.begin(), platform_dirs.end());
            return Text::FontDatabase::create(span<const string>{search_dirs.data(), search_dirs.size()});
        }

        [[nodiscard]] optional<string> find_first_available(const Text::FontDatabase &database,
                                                             span<const char *const> preferred_families) {
            for (const char *family : preferred_families) {
                if (optional<string> path = database.find(family)) {
                    return path;
                }
            }
            return std::nullopt;
        }

        // Best-effort default UI font pick: Maple Mono NF (the Nerd-Font-patched build of Maple
        // Mono) is preferred when installed, falling back to a handful of common family names
        // likely to exist on any of this engine's target OSes, then to whatever discovery found
        // first.
        [[nodiscard]] optional<string> find_default_font_path(const Text::FontDatabase &database) {
            static constexpr array<const char *, 6> preferred_families{
                "Maple Mono NF", "DejaVu Sans", "Noto Sans", "Liberation Sans", "Arial", "Helvetica",
            };
            if (optional<string> path = find_first_available(database, preferred_families)) {
                return path;
            }
            if (!database.faces().empty()) {
                return database.faces().front().file_path;
            }
            return std::nullopt;
        }

        // Best-effort default emoji font pick: Noto Color Emoji (bundled alongside Maple Mono NF —
        // see Fonts/), falling back to whatever other color-emoji family common OSes tend to ship.
        // No last-resort fallback to "whatever discovery found first" the way the UI font has one:
        // an arbitrary non-emoji font picked here would just render emoji as tofu/missing-glyph
        // boxes, no better than having no emoji font at all — see the `has_emoji_font` degradation
        // path in ensure_text_overlay_resources().
        [[nodiscard]] optional<string> find_default_emoji_font_path(const Text::FontDatabase &database) {
            static constexpr array<const char *, 3> preferred_families{
                "Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji",
            };
            return find_first_available(database, preferred_families);
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

        const Text::FontDatabase database = build_font_database();

        optional<string> font_path = find_default_font_path(database);
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

        // Best-effort: an emoji font that fails to load or parse just means emoji render as
        // whatever tofu the primary font has for those codepoints, not a broken overlay.
        guard->has_emoji_font = false;
        if (optional<string> emoji_path = find_default_emoji_font_path(database)) {
            if (optional<vector<std::byte>> emoji_bytes = read_file_bytes(*emoji_path)) {
                if (auto emoji_font = Text::Font::load(span<const std::byte>{emoji_bytes->data(), emoji_bytes->size()})) {
                    guard->emoji_font = std::move(*emoji_font);
                    guard->emoji_font_id = 2;
                    guard->has_emoji_font = true;
                }
            }
        }

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
        const u32 emoji_units_per_em = guard->has_emoji_font ? guard->emoji_font.units_per_em() : 0;
        const f32 emoji_scale = emoji_units_per_em > 0 ? overlay_pixel_size / static_cast<f32>(emoji_units_per_em) : 0.0f;

        const Text::FontStack fonts{
            .primary = &guard->font,
            .emoji = guard->has_emoji_font ? &guard->emoji_font : nullptr,
            .primary_font_id = guard->font_id,
            .emoji_font_id = guard->emoji_font_id,
        };

        vector<GlyphPlacement> placements;
        glm::vec2 pen = origin_px;
        for (const string &line : lines) {
            auto shaped = Text::shape_with_fallback(fonts, line);
            if (!shaped) {
                pen.y += overlay_line_height;
                continue;
            }

            glm::vec2 cursor = pen;
            for (const Text::PositionedGlyph &glyph : *shaped) {
                const f32 glyph_scale = glyph.is_color ? emoji_scale : scale;

                const Text::GlyphOutline *outline = nullptr;
                if (!glyph.is_color) {
                    auto cached = guard->outline_cache.find(glyph.glyph_id);
                    if (cached == guard->outline_cache.end()) {
                        if (auto extracted = Text::glyph_outline(guard->font, glyph.glyph_id)) {
                            cached = guard->outline_cache.emplace(glyph.glyph_id, std::move(*extracted)).first;
                        } else {
                            cached = guard->outline_cache.emplace(glyph.glyph_id, Text::GlyphOutline{}).first;
                        }
                    }
                    outline = &cached->second;
                }

                placements.push_back(GlyphPlacement{
                    .position = glm::vec2{cursor.x + glyph.x_offset * glyph_scale, cursor.y - glyph.y_offset * glyph_scale},
                    .size = glm::vec2{overlay_pixel_size, overlay_pixel_size},
                    .color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
                    .font_id = glyph.font_id,
                    .glyph_id = glyph.glyph_id,
                    .units_per_em = glyph.is_color ? emoji_units_per_em : units_per_em,
                    .pixel_size = overlay_pixel_size,
                    .format = glyph.is_color ? Text::RasterFormat::Color : Text::select_raster_format(overlay_pixel_size),
                    .outline = outline,
                    .font = glyph.is_color ? &guard->emoji_font : &guard->font,
                });

                cursor.x += glyph.x_advance * glyph_scale;
                cursor.y -= glyph.y_advance * glyph_scale;
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
            instances.push_back(make_glyph_instance(placements[i].position, placements[i], slots[i], guard->atlas.pixel_range()));
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
