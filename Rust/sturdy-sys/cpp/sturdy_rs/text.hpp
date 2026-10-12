// Rust <-> SturdyEngine 5 shim for the text package (Renderer/Text/*). See src/text.rs.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <rust/cxx.h>

#include <Renderer/Text/Text.hpp>

namespace sturdy_rs::text {

    enum class TextDirectionKind : std::uint8_t;
    enum class TextAlignmentKind : std::uint8_t;
    struct FontLoadResult;
    struct FontMetrics;
    struct FontFace;
    struct FeatureInfo;
    struct FontStackDesc;
    struct ShapeOptionsDesc;
    struct LayoutOptionsDesc;
    struct ShapeResult;
    struct LayoutResult;
    struct BreakList;
    struct OutlineResult;
    struct GlyphBoundsDesc;
    struct RasterParamsDesc;
    struct RasterResult;

    struct ByteRange;
    struct PointDesc;
    struct EditDesc;
    struct ApplyResultDesc;
    struct DocumentStats;

    using FontDatabaseBox = SFT::Text::FontDatabase;
    using DocumentBox = SFT::Text::Document;

    // -- Shared with text_render.cpp: the process-wide font registry ------------------------------

    /// The font registered under `id`, or null. Valid until `font_unload(id)`.
    const SFT::Text::Font *registry_font(std::uint64_t id);
    /// Resolves a Rust font-stack description against the registry; `fallbacks` provides the storage
    /// `stack.fallbacks` points into and must outlive `stack`. False when a referenced font is unknown.
    bool build_font_stack(const FontStackDesc &desc, std::vector<SFT::Text::FallbackFont> &fallbacks,
                          SFT::Text::FontStack &stack);
    SFT::Text::ShapeOptions to_shape_options(const ShapeOptionsDesc &desc, rust::String &error);

    FontLoadResult font_load_bytes(rust::Slice<const std::uint8_t> data, std::uint32_t face_index);
    FontLoadResult font_load_file(rust::Str path, std::uint32_t face_index);
    FontLoadResult font_default();
    bool font_unload(std::uint64_t id);
    FontMetrics font_metrics(std::uint64_t id);
    rust::Vec<rust::String> font_search_directories();
    rust::Vec<FeatureInfo> font_features(std::uint64_t id, rust::Str script, rust::Str language);
    rust::String feature_spec_error(rust::Str specification);

    std::unique_ptr<FontDatabaseBox> font_database_new(const rust::Vec<rust::String> &directories);
    rust::Vec<FontFace> font_database_faces(const FontDatabaseBox &database);
    rust::String font_database_find(const FontDatabaseBox &database, rust::Str family, bool bold, bool italic);

    ShapeResult text_shape(const FontStackDesc &stack, rust::Str text, const ShapeOptionsDesc &options);
    LayoutResult text_layout(const FontStackDesc &stack, rust::Str text, const LayoutOptionsDesc &options);
    BreakList line_break_opportunities(rust::Str text, rust::Str language);
    rust::Vec<std::uint64_t> grapheme_boundaries(rust::Str text, rust::Str language);
    rust::Vec<std::uint64_t> word_boundaries(rust::Str text, rust::Str language);
    bool is_emoji_codepoint(std::uint32_t codepoint) noexcept;

    OutlineResult glyph_outline(std::uint64_t font, std::uint32_t glyph_id);
    GlyphBoundsDesc glyph_bounds(std::uint64_t font, std::uint32_t glyph_id);
    std::uint8_t select_raster_format(float long_side_px, std::uint8_t previous) noexcept;
    RasterResult rasterize_glyph(std::uint64_t font, std::uint32_t glyph_id, const RasterParamsDesc &params);
    std::uint8_t color_glyph_format(std::uint64_t font, std::uint32_t glyph_id);
    RasterResult rasterize_color_glyph(std::uint64_t font, std::uint32_t glyph_id, std::uint32_t width,
                                       std::uint32_t height, float pixel_size, float padding_px);

    std::unique_ptr<DocumentBox> document_new(rust::Str initial);
    std::uint64_t document_revision(const DocumentBox &document);
    rust::String document_text(const DocumentBox &document);
    rust::String document_slice(const DocumentBox &document, ByteRange range);
    std::uint64_t document_line_count(const DocumentBox &document);
    ByteRange document_line_range(const DocumentBox &document, std::uint64_t line, bool &found);
    DocumentStats document_stats(const DocumentBox &document);
    ApplyResultDesc document_apply(DocumentBox &document, const rust::Vec<EditDesc> &edits, std::uint8_t kind);
    ApplyResultDesc document_undo(DocumentBox &document);
    ApplyResultDesc document_redo(DocumentBox &document);
    PointDesc document_offset_to_point(const DocumentBox &document, std::uint64_t offset);
    PointDesc document_offset_to_utf16(const DocumentBox &document, std::uint64_t offset);
    std::uint64_t document_point_to_offset(const DocumentBox &document, std::uint64_t line, std::uint64_t column, bool &found);
    std::uint64_t document_utf16_to_offset(const DocumentBox &document, std::uint64_t line, std::uint64_t column, bool &found);
    std::uint64_t document_clip_offset(const DocumentBox &document, std::uint64_t offset, bool after);
    std::uint8_t document_char_class_at(const DocumentBox &document, std::uint64_t offset);
    ByteRange document_word_range_at(const DocumentBox &document, std::uint64_t offset, bool &found);
    std::uint64_t document_next_word_boundary(const DocumentBox &document, std::uint64_t offset);
    std::uint64_t document_previous_word_boundary(const DocumentBox &document, std::uint64_t offset);
    std::uint64_t document_matching_bracket(const DocumentBox &document, std::uint64_t offset, bool &found);
    ByteRange document_find(const DocumentBox &document, rust::Str pattern, std::uint64_t from, bool forward,
                            bool case_sensitive, bool &found);
    rust::Vec<ByteRange> document_find_all(const DocumentBox &document, rust::Str pattern, bool case_sensitive);
    bool document_is_line_blank(const DocumentBox &document, std::uint64_t line);
    std::array<std::uint64_t, 4> document_line_indent(const DocumentBox &document, std::uint64_t line);
    std::array<std::uint64_t, 2> document_detect_indent(const DocumentBox &document);
    std::uint8_t document_detect_line_ending(const DocumentBox &document);

} // namespace sturdy_rs::text
