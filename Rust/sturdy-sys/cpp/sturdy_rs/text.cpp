#include "sturdy_rs/text.hpp"

#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <Foundation/Foundation.hpp>
#include <Renderer/Text/EmbeddedFont.hpp>
#include <Renderer/Text/PlatformFonts.hpp>

// Generated from src/text.rs.
#include "sturdy_rs/text_bridge.h"

namespace sturdy_rs::text {
    namespace {

        namespace T = SFT::Text;

        std::mutex g_mutex;
        std::map<std::uint64_t, std::unique_ptr<T::Font>> g_fonts;
        std::uint64_t g_next_id = 1;
        std::uint64_t g_default_id = 0;

        std::string_view sv(rust::Str s) noexcept { return {s.data(), s.size()}; }
        SFT::ustr text_of(rust::Str s) noexcept { return SFT::ustr{sv(s)}; }
        SFT::UString ustring_of(rust::Str s) { return SFT::UString{sv(s)}; }
        rust::String to_rust(const SFT::UString &s) {
            const auto view = s.cpp_string_view();
            return rust::String::lossy(view.data(), view.size());
        }
        rust::String to_rust(const T::TextError &e) { return to_rust(e.message); }

        std::uint64_t add_font(T::Font &&font) {
            const std::lock_guard lock{g_mutex};
            const std::uint64_t id = g_next_id++;
            g_fonts.emplace(id, std::make_unique<T::Font>(std::move(font)));
            return id;
        }

        FontLoadResult load_result(T::TextExpected<T::Font> &&loaded) {
            FontLoadResult out{};
            if (!loaded) {
                out.error = to_rust(loaded.error());
                return out;
            }
            // HarfBuzz accepts arbitrary bytes as a blob; a face with no design units is not a font.
            if (!loaded->valid() || loaded->units_per_em() == 0 || hb_face_get_glyph_count(loaded->face_handle()) == 0) {
                out.error = rust::String("data is not a usable font (no glyph data or design units)");
                return out;
            }
            out.ok = true;
            out.id = add_font(std::move(*loaded));
            return out;
        }

        TextDirectionKind to_kind(T::TextDirection d) noexcept {
            switch (d) {
            case T::TextDirection::LeftToRight: return TextDirectionKind::LeftToRight;
            case T::TextDirection::RightToLeft: return TextDirectionKind::RightToLeft;
            case T::TextDirection::TopToBottom: return TextDirectionKind::TopToBottom;
            case T::TextDirection::BottomToTop: return TextDirectionKind::BottomToTop;
            default: return TextDirectionKind::Auto;
            }
        }

        T::TextDirection from_kind(TextDirectionKind d) noexcept {
            switch (d) {
            case TextDirectionKind::LeftToRight: return T::TextDirection::LeftToRight;
            case TextDirectionKind::RightToLeft: return T::TextDirection::RightToLeft;
            case TextDirectionKind::TopToBottom: return T::TextDirection::TopToBottom;
            case TextDirectionKind::BottomToTop: return T::TextDirection::BottomToTop;
            default: return T::TextDirection::Auto;
            }
        }

        GlyphDesc to_desc(const T::PositionedGlyph &g) noexcept {
            return GlyphDesc{g.glyph_id, g.x_advance, g.y_advance, g.x_offset, g.y_offset, g.cluster, g.font_id, g.is_color};
        }

        struct ResolvedStack {
            std::vector<T::FallbackFont> fallbacks;
            T::FontStack stack;
        };

    } // namespace

    const T::Font *registry_font(std::uint64_t id) {
        const std::lock_guard lock{g_mutex};
        const auto found = g_fonts.find(id);
        return found == g_fonts.end() ? nullptr : found->second.get();
    }

    bool build_font_stack(const FontStackDesc &desc, std::vector<T::FallbackFont> &fallbacks, T::FontStack &stack) {
        stack = T::FontStack{};
        stack.primary = registry_font(desc.primary);
        if (stack.primary == nullptr) {
            return false;
        }
        stack.primary_font_id = desc.primary;
        if (desc.emoji != 0) {
            stack.emoji = registry_font(desc.emoji);
            if (stack.emoji == nullptr) {
                return false;
            }
            stack.emoji_font_id = desc.emoji;
        }
        stack.emoji_is_color = desc.emoji_is_color;
        fallbacks.clear();
        for (std::uint64_t id : desc.fallbacks) {
            const T::Font *font = registry_font(id);
            if (font == nullptr) {
                return false;
            }
            fallbacks.push_back(T::FallbackFont{font, id, false});
        }
        stack.fallbacks = std::span<const T::FallbackFont>{fallbacks};
        return true;
    }

    T::ShapeOptions to_shape_options(const ShapeOptionsDesc &desc, rust::String &error) {
        T::ShapeOptions options;
        options.script = ustring_of(rust::Str{desc.script.data(), desc.script.size()});
        options.language = ustring_of(rust::Str{desc.language.data(), desc.language.size()});
        options.direction = from_kind(desc.direction);
        if (!desc.features.empty()) {
            auto parsed = T::parse_feature_settings(SFT::ustr{std::string_view{desc.features.data(), desc.features.size()}});
            if (!parsed) {
                error = to_rust(parsed.error());
            } else {
                options.features.custom = std::move(*parsed);
            }
        }
        return options;
    }

    FontLoadResult font_load_bytes(rust::Slice<const std::uint8_t> data, std::uint32_t face_index) {
        return load_result(T::Font::load(std::as_bytes(std::span{data.data(), data.size()}), face_index));
    }

    FontLoadResult font_load_file(rust::Str path, std::uint32_t face_index) {
        std::ifstream file(std::string(sv(path)), std::ios::binary);
        if (!file) {
            FontLoadResult out{};
            out.error = rust::String("cannot open font file");
            return out;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return load_result(T::Font::load(std::as_bytes(std::span{bytes.data(), bytes.size()}), face_index));
    }

    FontLoadResult font_default() {
        {
            const std::lock_guard lock{g_mutex};
            if (g_default_id != 0 && g_fonts.count(g_default_id) != 0) {
                return FontLoadResult{true, rust::String(), g_default_id};
            }
        }
        FontLoadResult out = load_result(T::Font::load(T::embedded_default_font_bytes()));
        if (out.ok) {
            const std::lock_guard lock{g_mutex};
            g_default_id = out.id;
        }
        return out;
    }

    bool font_unload(std::uint64_t id) {
        const std::lock_guard lock{g_mutex};
        if (id == g_default_id) {
            g_default_id = 0;
        }
        return g_fonts.erase(id) != 0;
    }

    FontMetrics font_metrics(std::uint64_t id) {
        FontMetrics out{};
        if (const T::Font *font = registry_font(id)) {
            out.ok = true;
            out.units_per_em = font->units_per_em();
            out.ascender = font->ascender();
            out.descender = font->descender();
            out.line_gap = font->line_gap();
        }
        return out;
    }

    rust::Vec<rust::String> font_search_directories() {
        rust::Vec<rust::String> out;
        for (const std::string &dir : T::font_search_directories()) {
            out.push_back(rust::String::lossy(dir));
        }
        return out;
    }

    rust::Vec<FeatureInfo> font_features(std::uint64_t id, rust::Str script, rust::Str language) {
        rust::Vec<FeatureInfo> out;
        const T::Font *font = registry_font(id);
        if (font == nullptr) {
            return out;
        }
        for (const T::OpenTypeFeature &feature : T::available_features(*font, text_of(script), text_of(language))) {
            const std::uint32_t tag = feature.tag;
            const char code[4] = {static_cast<char>(tag >> 24), static_cast<char>(tag >> 16),
                                  static_cast<char>(tag >> 8), static_cast<char>(tag)};
            out.push_back(FeatureInfo{rust::String::lossy(code, 4), to_rust(feature.name)});
        }
        return out;
    }

    rust::String feature_spec_error(rust::Str specification) {
        auto parsed = T::parse_feature_settings(text_of(specification));
        return parsed ? rust::String() : to_rust(parsed.error());
    }

    std::unique_ptr<FontDatabaseBox> font_database_new(const rust::Vec<rust::String> &directories) {
        std::vector<std::string> dirs;
        for (const rust::String &dir : directories) {
            dirs.emplace_back(std::string(dir));
        }
        return std::make_unique<FontDatabaseBox>(FontDatabaseBox::create(std::span<const std::string>{dirs}));
    }

    rust::Vec<FontFace> font_database_faces(const FontDatabaseBox &database) {
        rust::Vec<FontFace> out;
        for (const T::FontFaceInfo &face : database.faces()) {
            out.push_back(FontFace{to_rust(face.family), to_rust(face.subfamily), face.bold, face.italic,
                                   rust::String::lossy(face.file_path), face.face_index});
        }
        return out;
    }

    rust::String font_database_find(const FontDatabaseBox &database, rust::Str family, bool bold, bool italic) {
        const auto found = database.find(text_of(family), bold, italic);
        return found ? rust::String::lossy(*found) : rust::String();
    }

    ShapeResult text_shape(const FontStackDesc &stack_desc, rust::Str text, const ShapeOptionsDesc &options) {
        ShapeResult out{};
        std::vector<T::FallbackFont> fallbacks;
        T::FontStack stack;
        if (!build_font_stack(stack_desc, fallbacks, stack)) {
            out.error = rust::String("font stack references an unknown font id");
            return out;
        }
        rust::String error;
        const T::ShapeOptions shape_options = to_shape_options(options, error);
        if (!error.empty()) {
            out.error = error;
            return out;
        }
        auto shaped = T::shape_with_fallback(stack, text_of(text), shape_options);
        if (!shaped) {
            out.error = to_rust(shaped.error());
            return out;
        }
        out.ok = true;
        for (const T::PositionedGlyph &glyph : *shaped) {
            out.glyphs.push_back(to_desc(glyph));
        }
        return out;
    }

    LayoutResult text_layout(const FontStackDesc &stack_desc, rust::Str text, const LayoutOptionsDesc &options) {
        LayoutResult out{};
        std::vector<T::FallbackFont> fallbacks;
        T::FontStack stack;
        if (!build_font_stack(stack_desc, fallbacks, stack)) {
            out.error = rust::String("font stack references an unknown font id");
            return out;
        }
        T::TextLayoutOptions layout_options;
        rust::String error;
        layout_options.shape = to_shape_options(options.shape, error);
        if (!error.empty()) {
            out.error = error;
            return out;
        }
        if (options.has_max_width) {
            layout_options.max_width_em = options.max_width_em;
        }
        layout_options.alignment = options.alignment == TextAlignmentKind::Center ? T::TextAlignment::Center
                                   : options.alignment == TextAlignmentKind::End   ? T::TextAlignment::End
                                                                                   : T::TextAlignment::Start;
        layout_options.break_language = ustring_of(rust::Str{options.break_language.data(), options.break_language.size()});

        auto laid_out = T::layout_text(stack, text_of(text), layout_options);
        if (!laid_out) {
            out.error = to_rust(laid_out.error());
            return out;
        }
        out.ok = true;
        out.width_em = laid_out->width_em;
        out.ascender_em = laid_out->ascender_em;
        out.descender_em = laid_out->descender_em;
        out.line_gap_em = laid_out->line_gap_em;
        out.line_height_em = laid_out->line_height_em;
        out.height_em = laid_out->height_em;
        for (const T::LaidOutLine &line : laid_out->lines) {
            LineDesc line_desc{};
            line_desc.byte_start = line.byte_start;
            line_desc.visible_byte_end = line.visible_byte_end;
            line_desc.byte_end = line.byte_end;
            line_desc.mandatory_break_after = line.mandatory_break_after;
            line_desc.offset_em = line.offset_em;
            line_desc.advance_em = line.shaped.advance_em;
            line_desc.base_direction = to_kind(line.shaped.base_direction);
            line_desc.run_start = static_cast<std::uint32_t>(out.runs.size());
            line_desc.run_count = static_cast<std::uint32_t>(line.shaped.runs.size());
            for (const T::ShapedRun &run : line.shaped.runs) {
                RunDesc run_desc{};
                run_desc.font_id = run.font_id;
                run_desc.units_per_em = run.units_per_em;
                run_desc.is_color = run.is_color;
                run_desc.direction = to_kind(run.direction);
                run_desc.pen_origin_em = run.pen_origin_em;
                run_desc.advance_em = run.advance_em;
                run_desc.glyph_start = static_cast<std::uint32_t>(out.glyphs.size());
                run_desc.glyph_count = static_cast<std::uint32_t>(run.glyphs.size());
                for (const T::PositionedGlyph &glyph : run.glyphs) {
                    out.glyphs.push_back(to_desc(glyph));
                }
                out.runs.push_back(run_desc);
            }
            out.lines.push_back(line_desc);
        }
        return out;
    }

    BreakList line_break_opportunities(rust::Str text, rust::Str language) {
        BreakList out{};
        for (const T::LineBreakOpportunity &b : T::line_break_opportunities(text_of(text), text_of(language))) {
            out.byte_index.push_back(b.byte_index);
            out.mandatory.push_back(b.kind == T::LineBreakKind::Mandatory);
        }
        return out;
    }

    rust::Vec<std::uint64_t> grapheme_boundaries(rust::Str text, rust::Str language) {
        rust::Vec<std::uint64_t> out;
        for (SFT::usize b : T::grapheme_boundaries(text_of(text), text_of(language))) out.push_back(b);
        return out;
    }

    rust::Vec<std::uint64_t> word_boundaries(rust::Str text, rust::Str language) {
        rust::Vec<std::uint64_t> out;
        for (SFT::usize b : T::word_boundaries(text_of(text), text_of(language))) out.push_back(b);
        return out;
    }

    bool is_emoji_codepoint(std::uint32_t codepoint) noexcept {
        return T::is_emoji_codepoint(static_cast<char32_t>(codepoint));
    }

    OutlineResult glyph_outline(std::uint64_t font_id, std::uint32_t glyph_id) {
        OutlineResult out{};
        const T::Font *font = registry_font(font_id);
        if (font == nullptr) {
            out.error = rust::String("unknown font id");
            return out;
        }
        auto outline = T::glyph_outline(*font, glyph_id);
        if (!outline) {
            out.error = to_rust(outline.error());
            return out;
        }
        out.ok = true;
        for (const T::Contour &contour : outline->contours) {
            out.contour_lengths.push_back(static_cast<std::uint32_t>(contour.size()));
            for (const T::OutlineSegment &s : contour) {
                out.kinds.push_back(static_cast<std::uint8_t>(s.kind));
                for (float v : {s.control1.x, s.control1.y, s.control2.x, s.control2.y, s.to.x, s.to.y}) {
                    out.points.push_back(v);
                }
            }
        }
        return out;
    }

    GlyphBoundsDesc glyph_bounds(std::uint64_t font_id, std::uint32_t glyph_id) {
        GlyphBoundsDesc out{};
        out.empty = true;
        const T::Font *font = registry_font(font_id);
        if (font == nullptr) {
            return out;
        }
        auto outline = T::glyph_outline(*font, glyph_id);
        if (!outline) {
            return out;
        }
        const T::GlyphBounds bounds = T::glyph_bounds(*outline);
        return GlyphBoundsDesc{bounds.empty, bounds.left, bounds.bottom, bounds.right, bounds.top};
    }

    std::uint8_t select_raster_format(float long_side_px, std::uint8_t previous) noexcept {
        return static_cast<std::uint8_t>(
            T::select_raster_format(long_side_px, static_cast<T::RasterFormat>(previous)));
    }

    namespace {
        RasterResult to_result(T::TextExpected<T::RasterizedGlyph> &&raster) {
            RasterResult out{};
            if (!raster) {
                out.error = to_rust(raster.error());
                return out;
            }
            out.ok = true;
            out.width = raster->width;
            out.height = raster->height;
            out.channel_count = raster->channel_count;
            out.bearing_x = raster->bearing_x;
            out.bearing_top = raster->bearing_top;
            for (std::uint8_t byte : raster->pixels) out.pixels.push_back(byte);
            return out;
        }
    } // namespace

    RasterResult rasterize_glyph(std::uint64_t font_id, std::uint32_t glyph_id, const RasterParamsDesc &params) {
        const T::Font *font = registry_font(font_id);
        if (font == nullptr) {
            RasterResult out{};
            out.error = rust::String("unknown font id");
            return out;
        }
        auto outline = T::glyph_outline(*font, glyph_id);
        if (!outline) {
            RasterResult out{};
            out.error = to_rust(outline.error());
            return out;
        }
        T::RasterParams raster_params;
        raster_params.width = params.width;
        raster_params.height = params.height;
        raster_params.scale = params.scale;
        raster_params.pixel_range = params.pixel_range;
        raster_params.padding_px = params.padding_px;
        if (params.has_translation) {
            raster_params.translation = T::RasterTranslation{params.translation_x, params.translation_y};
        }
        return to_result(T::rasterize_glyph(*outline, static_cast<T::RasterFormat>(params.format), raster_params));
    }

    std::uint8_t color_glyph_format(std::uint64_t font_id, std::uint32_t glyph_id) {
        const T::Font *font = registry_font(font_id);
        return font == nullptr ? 0 : static_cast<std::uint8_t>(T::detect_color_format(*font, glyph_id));
    }

    RasterResult rasterize_color_glyph(std::uint64_t font_id, std::uint32_t glyph_id, std::uint32_t width,
                                       std::uint32_t height, float pixel_size, float padding_px) {
        const T::Font *font = registry_font(font_id);
        if (font == nullptr) {
            RasterResult out{};
            out.error = rust::String("unknown font id");
            return out;
        }
        return to_result(T::rasterize_color_glyph(*font, glyph_id,
                                                  T::ColorRasterParams{width, height, pixel_size, padding_px}));
    }

} // namespace sturdy_rs::text
