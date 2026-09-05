#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cmath>
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
#include <WindowManager/WindowManager.hpp>
#include <Renderer/Text/Text.hpp>
#include <Renderer/Text/PlatformFonts.hpp>
#include <Renderer/Text/EmbeddedFont.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::optional;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {


        constexpr f32 overlay_pixel_size = 18.0f;

        /// Computes the read file bytes required by the supplied values.
        ///
        /// @param path Filesystem path identifying the target resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
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


        /// Builds font database.
        ///
        /// @return Returns the current build font database value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Text::FontDatabase build_font_database() {
            vector<string> search_dirs{"Fonts"};
            const vector<string> platform_dirs = Text::font_search_directories();
            search_dirs.insert(search_dirs.end(), platform_dirs.begin(), platform_dirs.end());
            return Text::FontDatabase::create(span<const string>{search_dirs.data(), search_dirs.size()});
        }

        /// Finds first available in the available state.
        ///
        /// @param database `database` value used by the operation.
        /// @param preferred_families `preferred_families` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<string> find_first_available(const Text::FontDatabase &database,
                                                             span<const UString> preferred_families) {
            for (const UString &family : preferred_families) {
                if (optional<string> path = database.find(family.as_ustr())) {
                    return path;
                }
            }
            return std::nullopt;
        }


        /// Finds default font path in the available state.
        ///
        /// @param database `database` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<string> find_default_font_path(const Text::FontDatabase &database) {
            static const array<UString, 12> preferred_families{
                UString{"Maple Mono NF"_ustr}, UString{"Segoe UI"_ustr}, UString{"SF Pro Text"_ustr},
                UString{"Roboto"_ustr}, UString{"Noto Sans"_ustr}, UString{"DejaVu Sans"_ustr},
                UString{"Fira Sans"_ustr}, UString{"Cantarell"_ustr}, UString{"Adwaita Sans"_ustr},
                UString{"Liberation Sans"_ustr}, UString{"Arial"_ustr}, UString{"Helvetica"_ustr},
            };
            if (optional<string> path = find_first_available(database, preferred_families)) {
                return path;
            }
            if (!database.faces().empty()) {
                return database.faces().front().file_path;
            }
            return std::nullopt;
        }


        /// Finds default emoji font path in the available state.
        ///
        /// @param database `database` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<string> find_default_emoji_font_path(const Text::FontDatabase &database) {
            static const array<UString, 3> preferred_families{
                UString{"Noto Color Emoji"_ustr}, UString{"Apple Color Emoji"_ustr},
                UString{"Segoe UI Emoji"_ustr},
            };
            return find_first_available(database, preferred_families);
        }

    } // namespace

    /// Finds or creates the text overlay resources required by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::ensure_text_overlay_resources() {
        ZoneScopedN("Renderer::ensure_text_overlay_resources");
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

        // No on-disk font is available at all on Web (no filesystem to search -- see
        // Text/Platform/Web/FontsImpl.cpp's font_search_directories()), and even on native
        // platforms a from-scratch/minimal install can lack every family in the preferred list.
        // Mirrors the shader-embedding fallback in Core/Slang/ShaderImpl.cpp: prefer the real
        // on-disk font when one is found, fall back to the font embedded into the binary
        // (cmake/SturdyFonts.cmake) rather than failing the whole debug overlay.
        optional<string> font_path = find_default_font_path(database);
        optional<vector<std::byte>> font_bytes_storage;
        span<const std::byte> font_bytes;
        string font_label;
        if (font_path) {
            font_bytes_storage = read_file_bytes(*font_path);
            if (!font_bytes_storage) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Failed to read the debug text overlay font file: " + *font_path);
            }
            font_bytes = span<const std::byte>{font_bytes_storage->data(), font_bytes_storage->size()};
            font_label = *font_path;
        } else {
            Foundation::log_info("No on-disk font was found for the debug text overlay; using the embedded copy.");
            font_bytes = Text::embedded_default_font_bytes();
            font_label = "<embedded>";
        }
        if (font_bytes.empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "No usable font was found for the debug text overlay.");
        }

        auto font = Text::Font::load_hinted(font_bytes, overlay_pixel_size);
        if (!font) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Failed to parse the debug text overlay font: " + font_label);
        }
        Foundation::log_info("Debug text overlay font: {}", font_label);

        auto atlas = TextAtlas::create(*device, TextAtlas::Config{});
        if (!atlas) {
            return unexpected(atlas.error());
        }

        auto pipeline = TextPipeline::create(*device, RHI::Format::BGRA8UnormSrgb, recovery_create_info_.enable_shader_disk_cache);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }

        guard->font = std::move(*font);
        guard->atlas = std::move(*atlas);
        guard->pipeline = std::move(*pipeline);
        guard->font_id = 1;
        guard->outline_cache.clear();
        guard->first_cached_line = 0;
        guard->line_cache.clear();
        guard->visible_layout = {};


        guard->has_emoji_font = false;
        if (optional<string> emoji_path = find_default_emoji_font_path(database)) {
            if (optional<vector<std::byte>> emoji_bytes = read_file_bytes(*emoji_path)) {
                if (auto emoji_font = Text::Font::load(span<const std::byte>{emoji_bytes->data(), emoji_bytes->size()})) {
                    guard->emoji_font = std::move(*emoji_font);
                    guard->emoji_font_id = 2;
                    guard->has_emoji_font = true;
                    Foundation::log_info("Debug text overlay emoji font: {}", *emoji_path);
                }
            }
        }

        guard->ready = true;
        return {};
    }

    /// Prepares text overlay for a later operation.
    ///
    /// @param encoder `encoder` value used by the operation.
    /// @param lines `lines` value used by the operation.
    /// @param origin_px `origin_px` value used by the operation.
    /// @param viewport_size_px `viewport_size_px` value used by the operation.
    /// @param frame_resources `frame_resources` value used by the operation.
    /// @param transient_buffers Buffer used or affected by the operation.
    /// @param retired_atlas_resources `retired_atlas_resources` value used by the operation.
    /// @param out_batches `out_batches` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::prepare_text_overlay(RHI::CommandEncoder &encoder, span<const UString> lines,
                                                        glm::vec2 origin_px, glm::vec2 viewport_size_px,
                                                        TextFrameResources &frame_resources,
                                                        vector<RHI::BufferHandle> &transient_buffers,
                                                        TextAtlasRetiredResources &retired_atlas_resources,
                                                        vector<TextDrawBatch> &out_batches) {
        ZoneScopedN("Renderer::prepare_text_overlay");
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
        const f32 ascender_px = static_cast<f32>(guard->font.ascender()) * scale;
        const f32 font_line_height =
            static_cast<f32>(guard->font.ascender() - guard->font.descender() + guard->font.line_gap()) * scale;
        const f32 line_height = std::max(font_line_height, overlay_pixel_size);


        auto clamp_line_index = [&](f32 index) noexcept -> usize {
            if (!(index > 0.0f)) {
                return 0;
            }
            const f32 line_count = static_cast<f32>(lines.size());
            if (!std::isfinite(index) || index >= line_count) {
                return lines.size();
            }
            return static_cast<usize>(index);
        };
        const usize first_visible_line = clamp_line_index(
            std::floor(-origin_px.y / line_height) - 1.0f);
        usize end_visible_line = clamp_line_index(
            std::ceil((viewport_size_px.y - origin_px.y) / line_height) + 1.0f);
        end_visible_line = std::max(end_visible_line, first_visible_line);
        const usize visible_line_count = end_visible_line - first_visible_line;

        TextOverlayResources::CachedVisibleLayout &cached_layout = guard->visible_layout;
        bool layout_matches = cached_layout.valid &&
                              cached_layout.first_line == first_visible_line &&
                              cached_layout.origin_px.x == origin_px.x &&
                              cached_layout.origin_px.y == origin_px.y &&
                              cached_layout.viewport_height_px == viewport_size_px.y &&
                              cached_layout.source_lines.size() == visible_line_count;
        if (layout_matches) {
            for (usize i = 0; i < visible_line_count; ++i) {
                if (cached_layout.source_lines[i] != lines[first_visible_line + i]) {
                    layout_matches = false;
                    break;
                }
            }
        }
        const RHI::Rect2D full_target_scissor{
            .x = 0, .y = 0, .width = static_cast<u32>(viewport_size_px.x), .height = static_cast<u32>(viewport_size_px.y)};

        if (layout_matches) {
            const vector<RHI::Rect2D> scissors(cached_layout.instances.size(), full_target_scissor);
            const vector<u32> paint_groups(cached_layout.instances.size(), 0);
            return guard->pipeline.prepare(*device, guard->atlas, cached_layout.instances, cached_layout.slots,
                                           scissors, paint_groups, frame_resources, out_batches);
        }

        const Text::FontStack fonts{
            .primary = &guard->font,
            .emoji = guard->has_emoji_font ? &guard->emoji_font : nullptr,
            .primary_font_id = guard->font_id,
            .emoji_font_id = guard->emoji_font_id,
        };


        Text::ShapeOptions shape_options;
        shape_options.features.calt = 1;
        shape_options.features.ccmp = 1;
        shape_options.features.clig = 1;
        shape_options.features.kern = 1;
        shape_options.features.liga = 1;
        shape_options.features.locl = 1;
        shape_options.features.mark = 1;
        shape_options.features.mkmk = 1;
        shape_options.features.tnum = 1;


        shape_options.features.zero = 1;


        vector<TextOverlayResources::CachedLine> next_line_cache;
        next_line_cache.reserve(visible_line_count);
        for (usize i = 0; i < visible_line_count; ++i) {
            const usize line_index = first_visible_line + i;
            TextOverlayResources::CachedLine entry;
            if (line_index >= guard->first_cached_line) {
                const usize old_offset = line_index - guard->first_cached_line;
                if (old_offset < guard->line_cache.size() &&
                    guard->line_cache[old_offset].initialized &&
                    guard->line_cache[old_offset].source == lines[line_index]) {
                    entry = std::move(guard->line_cache[old_offset]);
                }
            }
            if (!entry.initialized) {
                entry.source = lines[line_index];
                auto shaped = Text::shape_line_with_fallback(fonts, entry.source.as_ustr(), shape_options);
                if (shaped) {
                    entry.shaped = std::move(*shaped);
                }
                entry.initialized = true;
            }
            next_line_cache.push_back(std::move(entry));
        }
        guard->first_cached_line = first_visible_line;
        guard->line_cache = std::move(next_line_cache);

        vector<GlyphPlacement> placements;
        placements.reserve(visible_line_count * 64u);
        for (usize i = 0; i < guard->line_cache.size(); ++i) {
            const optional<Text::ShapedLine> &shaped = guard->line_cache[i].shaped;
            if (!shaped) {
                continue;
            }
            const usize line_index = first_visible_line + i;
            const glm::vec2 pen{
                origin_px.x,
                origin_px.y + ascender_px + static_cast<f32>(line_index) * line_height,
            };

            f32 visual_run_x = pen.x;
            for (const Text::ShapedRun &run : shaped->runs) {
                const f32 run_scale = overlay_pixel_size / static_cast<f32>(std::max(run.units_per_em, 1u));
                glm::vec2 cursor{visual_run_x + run.pen_origin_em * overlay_pixel_size, pen.y};
                for (const Text::PositionedGlyph &glyph : run.glyphs) {
                    const f32 glyph_scale = run_scale;

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
                        .position = glm::vec2{cursor.x + glyph.x_offset * glyph_scale,
                                             cursor.y - glyph.y_offset * glyph_scale},
                        .size = glm::vec2{overlay_pixel_size, overlay_pixel_size},
                        .color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
                        .font_id = glyph.font_id,
                        .glyph_id = glyph.glyph_id,
                        .units_per_em = run.units_per_em,
                        .pixel_size = overlay_pixel_size,
                        .format = glyph.is_color ? Text::RasterFormat::Color
                                                 : Text::select_raster_format(overlay_pixel_size),
                        .outline = outline,
                        .font = glyph.is_color ? &guard->emoji_font : &guard->font,


                        .stem_darkening = false,
                    });

                    cursor.x += glyph.x_advance * glyph_scale;
                    cursor.y -= glyph.y_advance * glyph_scale;
                }
                visual_run_x += run.advance_em * overlay_pixel_size;
            }
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
        if (!requests.empty()) {
            if (auto resident = guard->atlas.ensure_resident(*device, encoder, requests, slots, transient_buffers,
                                                             retired_atlas_resources);
                !resident) {
                return unexpected(resident.error());
            }
        }

        vector<GlyphInstance> instances;
        instances.reserve(placements.size());
        for (usize i = 0; i < placements.size(); ++i) {
            instances.push_back(make_glyph_instance(placements[i].position, placements[i], slots[i], guard->atlas.pixel_range()));
        }

        cached_layout.first_line = first_visible_line;
        cached_layout.origin_px = origin_px;
        cached_layout.viewport_height_px = viewport_size_px.y;
        cached_layout.source_lines.assign(lines.begin() + static_cast<isize>(first_visible_line),
                                          lines.begin() + static_cast<isize>(end_visible_line));
        cached_layout.slots = std::move(slots);
        cached_layout.instances = std::move(instances);
        cached_layout.valid = true;

        const vector<RHI::Rect2D> scissors(cached_layout.instances.size(), full_target_scissor);
        const vector<u32> paint_groups(cached_layout.instances.size(), 0);
        return guard->pipeline.prepare(*device, guard->atlas, cached_layout.instances, cached_layout.slots, scissors,
                                       paint_groups, frame_resources, out_batches);
    }

    /// Draws text overlay using the current rendering state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param batches `batches` value used by the operation.
    /// @param viewport_size_px `viewport_size_px` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::draw_text_overlay(RHI::RenderPassEncoder &pass, span<const TextDrawBatch> batches,
                                                      glm::vec2 viewport_size_px) {
        ZoneScopedN("Renderer::draw_text_overlay");
        if (batches.empty()) {
            return {};
        }

        auto guard = text_overlay_.lock();
        if (!guard->ready) {
            return {};
        }

        return guard->pipeline.draw(pass, batches, viewport_size_px);
    }

    /// Destroys the text overlay resources locked identified by the supplied parameters.
    ///
    /// @param resources `resources` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_text_overlay_resources_locked(TextOverlayResources &resources) noexcept {
        ZoneScopedN("Renderer::destroy_text_overlay_resources_locked");
        if (RHI::RhiDevice *device = rhi_device()) {
            resources.pipeline.destroy(*device);
            resources.atlas.destroy(*device);
        }
        resources.outline_cache.clear();
        resources.first_cached_line = 0;
        resources.line_cache.clear();
        resources.visible_layout = {};
        resources.ready = false;
    }

    /// Destroys the text overlay resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy text overlay resources value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_text_overlay_resources() noexcept {
        ZoneScopedN("Renderer::destroy_text_overlay_resources");
        auto guard = text_overlay_.lock();
        destroy_text_overlay_resources_locked(*guard);
    }

} // namespace SFT::Renderer
