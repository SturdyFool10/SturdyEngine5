//! Raw `cxx` bindings for the engine's text package (`Renderer/Text/*`): font loading and
//! discovery, OpenType shaping with fallback, line breaking and layout, glyph outlines and
//! rasterization. Everything here is CPU-side and independent of any `Engine`.
//!
//! Fonts live in a process-wide registry on the C++ side and are addressed by a `u64` id (`0` is
//! never valid); that id is also what the GPU text renderer's glyph placements refer to.
//! Positions and advances coming out of layout are in *em* units (multiply by the pixel size).
//!
//! [`crate::text_render`] (GPU rendering to textures) builds on the same font ids.

#[cxx::bridge(namespace = "sturdy_rs::text")]
pub mod ffi {
    #[derive(Debug, Clone)]
    struct FontLoadResult {
        ok: bool,
        error: String,
        id: u64,
    }

    /// Design-unit metrics of a font (`units_per_em` design units make one em).
    #[derive(Debug, Clone, Copy, Default)]
    struct FontMetrics {
        ok: bool,
        units_per_em: u32,
        ascender: i32,
        descender: i32,
        line_gap: i32,
    }

    /// `SFT::Text::FontFaceInfo`.
    #[derive(Debug, Clone, Default)]
    struct FontFace {
        family: String,
        subfamily: String,
        bold: bool,
        italic: bool,
        file_path: String,
        face_index: u32,
    }

    #[derive(Debug, Clone, Default)]
    struct FeatureInfo {
        /// Four-character OpenType tag, e.g. `liga`.
        tag: String,
        name: String,
    }

    /// `SFT::Text::FontStack`: a primary font, an optional emoji font and further fallbacks tried
    /// in order for characters the primary lacks. `0` means none.
    #[derive(Debug, Clone, Default)]
    struct FontStackDesc {
        primary: u64,
        emoji: u64,
        emoji_is_color: bool,
        fallbacks: Vec<u64>,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TextDirectionKind {
        Auto,
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TextAlignmentKind {
        Start,
        Center,
        End,
    }

    /// `SFT::Text::ShapeOptions`. `features` is a comma-separated OpenType feature specification
    /// (e.g. `"liga=0, smcp, ss01"`); empty leaves the shaper's defaults.
    #[derive(Debug, Clone)]
    struct ShapeOptionsDesc {
        script: String,
        language: String,
        direction: TextDirectionKind,
        features: String,
    }

    /// `SFT::Text::TextLayoutOptions`.
    #[derive(Debug, Clone)]
    struct LayoutOptionsDesc {
        has_max_width: bool,
        max_width_em: f32,
        alignment: TextAlignmentKind,
        shape: ShapeOptionsDesc,
        break_language: String,
    }

    /// `SFT::Text::PositionedGlyph`.
    #[derive(Debug, Clone, Copy, Default)]
    struct GlyphDesc {
        glyph_id: u32,
        x_advance: f32,
        y_advance: f32,
        x_offset: f32,
        y_offset: f32,
        /// Byte offset in the shaped text of the character this glyph came from.
        cluster: u32,
        font_id: u64,
        is_color: bool,
    }

    /// `SFT::Text::ShapedRun`: a same-font, same-direction stretch of glyphs.
    #[derive(Debug, Clone, Copy)]
    struct RunDesc {
        font_id: u64,
        units_per_em: u32,
        is_color: bool,
        direction: TextDirectionKind,
        pen_origin_em: f32,
        advance_em: f32,
        glyph_start: u32,
        glyph_count: u32,
    }

    /// `SFT::Text::LaidOutLine`.
    #[derive(Debug, Clone, Copy)]
    struct LineDesc {
        byte_start: u64,
        visible_byte_end: u64,
        byte_end: u64,
        mandatory_break_after: bool,
        offset_em: f32,
        advance_em: f32,
        base_direction: TextDirectionKind,
        run_start: u32,
        run_count: u32,
    }

    /// `SFT::Text::TextLayout`, flattened: `lines` index into `runs`, which index into `glyphs`.
    #[derive(Debug, Clone)]
    struct LayoutResult {
        ok: bool,
        error: String,
        width_em: f32,
        ascender_em: f32,
        descender_em: f32,
        line_gap_em: f32,
        line_height_em: f32,
        height_em: f32,
        lines: Vec<LineDesc>,
        runs: Vec<RunDesc>,
        glyphs: Vec<GlyphDesc>,
    }

    #[derive(Debug, Clone)]
    struct ShapeResult {
        ok: bool,
        error: String,
        glyphs: Vec<GlyphDesc>,
    }

    /// Break opportunities: `byte_index[i]` is where a break is allowed; `mandatory[i]` says it
    /// is forced (a newline).
    #[derive(Debug, Clone, Default)]
    struct BreakList {
        byte_index: Vec<u64>,
        mandatory: Vec<bool>,
    }

    /// A glyph outline. `contour_lengths[i]` segments belong to contour `i`; each segment is
    /// `kinds[j]` (0 move, 1 line, 2 quadratic, 3 cubic) with 6 floats in `points`:
    /// `control1.xy, control2.xy, to.xy` (unused controls are zero). Font units.
    #[derive(Debug, Clone, Default)]
    struct OutlineResult {
        ok: bool,
        error: String,
        contour_lengths: Vec<u32>,
        kinds: Vec<u8>,
        points: Vec<f32>,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct GlyphBoundsDesc {
        empty: bool,
        left: f32,
        bottom: f32,
        right: f32,
        top: f32,
    }

    /// `SFT::Text::RasterParams` (+ format selection).
    #[derive(Debug, Clone, Copy)]
    struct RasterParamsDesc {
        /// 0 = SDF, 1 = MSDF, 2 = color.
        format: u8,
        width: u32,
        height: u32,
        scale: f32,
        pixel_range: f32,
        padding_px: f32,
        has_translation: bool,
        translation_x: f32,
        translation_y: f32,
    }

    #[derive(Debug, Clone, Default)]
    struct RasterResult {
        ok: bool,
        error: String,
        width: u32,
        height: u32,
        channel_count: u32,
        bearing_x: f32,
        bearing_top: f32,
        pixels: Vec<u8>,
    }

    /// A byte range in a document.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
    struct ByteRange {
        start: u64,
        end: u64,
    }

    /// `line` and `column` (a Unicode scalar column, or UTF-16 code unit column for the utf16 variants).
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
    struct PointDesc {
        found: bool,
        line: u64,
        column: u64,
    }

    /// One edit: replace `range` with `replacement`.
    #[derive(Debug, Clone)]
    struct EditDesc {
        range: ByteRange,
        replacement: String,
    }

    /// One change reported by an applied edit.
    #[derive(Debug, Clone, Default)]
    struct ChangeDesc {
        old_range: ByteRange,
        new_range: ByteRange,
        inserted: String,
        deleted: String,
    }

    #[derive(Debug, Clone, Default)]
    struct ApplyResultDesc {
        ok: bool,
        /// `DocumentError` name when `!ok`.
        error: String,
        revision_before: u64,
        revision_after: u64,
        changes: Vec<ChangeDesc>,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct DocumentStats {
        logical_bytes: u64,
        backing_storage_bytes: u64,
        tree_nodes: u64,
        pieces: u64,
        maximum_tree_depth: u64,
        undo_revisions: u64,
        redo_revisions: u64,
        scalars: u64,
        utf16_code_units: u64,
        newlines: u64,
        ascii: bool,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/text.hpp");

        /// `SFT::Text::Document`: an editable, versioned rope of UTF-8 text with undo/redo, line and
        /// position mapping, word/bracket navigation and search.
        type DocumentBox;

        fn document_new(initial: &str) -> UniquePtr<DocumentBox>;
        fn document_revision(document: &DocumentBox) -> u64;
        fn document_text(document: &DocumentBox) -> String;
        fn document_slice(document: &DocumentBox, range: ByteRange) -> String;
        fn document_line_count(document: &DocumentBox) -> u64;
        /// `found == false` (start == end == 0) when `line` is out of range.
        fn document_line_range(document: &DocumentBox, line: u64, found: &mut bool) -> ByteRange;
        fn document_stats(document: &DocumentBox) -> DocumentStats;
        /// Applies `edits` (non-overlapping ranges of the *current* text) as one transaction.
        fn document_apply(document: Pin<&mut DocumentBox>, edits: &Vec<EditDesc>, kind: u8) -> ApplyResultDesc;
        fn document_undo(document: Pin<&mut DocumentBox>) -> ApplyResultDesc;
        fn document_redo(document: Pin<&mut DocumentBox>) -> ApplyResultDesc;
        fn document_offset_to_point(document: &DocumentBox, offset: u64) -> PointDesc;
        fn document_offset_to_utf16(document: &DocumentBox, offset: u64) -> PointDesc;
        fn document_point_to_offset(document: &DocumentBox, line: u64, scalar_column: u64, found: &mut bool) -> u64;
        fn document_utf16_to_offset(document: &DocumentBox, line: u64, code_unit_column: u64, found: &mut bool) -> u64;
        /// Snaps `offset` to a character boundary (`after` picks the direction).
        fn document_clip_offset(document: &DocumentBox, offset: u64, after: bool) -> u64;
        /// 0 = whitespace, 1 = word, 2 = punctuation.
        fn document_char_class_at(document: &DocumentBox, offset: u64) -> u8;
        fn document_word_range_at(document: &DocumentBox, offset: u64, found: &mut bool) -> ByteRange;
        fn document_next_word_boundary(document: &DocumentBox, offset: u64) -> u64;
        fn document_previous_word_boundary(document: &DocumentBox, offset: u64) -> u64;
        fn document_matching_bracket(document: &DocumentBox, offset: u64, found: &mut bool) -> u64;
        fn document_find(document: &DocumentBox, pattern: &str, from: u64, forward: bool, case_sensitive: bool, found: &mut bool) -> ByteRange;
        fn document_find_all(document: &DocumentBox, pattern: &str, case_sensitive: bool) -> Vec<ByteRange>;
        fn document_is_line_blank(document: &DocumentBox, line: u64) -> bool;
        /// Leading whitespace of a line: `[bytes, scalars, has_tabs, has_spaces]`.
        fn document_line_indent(document: &DocumentBox, line: u64) -> [u64; 4];
        /// `[kind (0 spaces, 1 tabs), width]`.
        fn document_detect_indent(document: &DocumentBox) -> [u64; 2];
        /// 0 = LF, 1 = CRLF.
        fn document_detect_line_ending(document: &DocumentBox) -> u8;

        /// `SFT::Text::FontDatabase`: discovered font faces searchable by family/style.
        type FontDatabaseBox;

        // -- Fonts -------------------------------------------------------------------------------
        fn font_load_bytes(data: &[u8], face_index: u32) -> FontLoadResult;
        fn font_load_file(path: &str, face_index: u32) -> FontLoadResult;
        /// The engine's embedded default font (loaded once; the same id every call).
        fn font_default() -> FontLoadResult;
        /// Frees a font. Ids of unloaded fonts are never reused. `false` if unknown.
        fn font_unload(id: u64) -> bool;
        fn font_metrics(id: u64) -> FontMetrics;
        /// Directories the platform keeps fonts in.
        fn font_search_directories() -> Vec<String>;
        /// OpenType features the font supports for a script/language (empty strings: defaults).
        fn font_features(id: u64, script: &str, language: &str) -> Vec<FeatureInfo>;
        /// Parses an OpenType feature specification; returns an empty string when valid, else why not.
        fn feature_spec_error(specification: &str) -> String;

        fn font_database_new(directories: &Vec<String>) -> UniquePtr<FontDatabaseBox>;
        fn font_database_faces(database: &FontDatabaseBox) -> Vec<FontFace>;
        /// File path of the best match, or an empty string.
        fn font_database_find(database: &FontDatabaseBox, family: &str, bold: bool, italic: bool) -> String;

        // -- Shaping / layout / breaking ---------------------------------------------------------
        fn text_shape(stack: &FontStackDesc, text: &str, options: &ShapeOptionsDesc) -> ShapeResult;
        fn text_layout(stack: &FontStackDesc, text: &str, options: &LayoutOptionsDesc) -> LayoutResult;
        fn line_break_opportunities(text: &str, language: &str) -> BreakList;
        fn grapheme_boundaries(text: &str, language: &str) -> Vec<u64>;
        fn word_boundaries(text: &str, language: &str) -> Vec<u64>;
        fn is_emoji_codepoint(codepoint: u32) -> bool;

        // -- Outlines / rasterization ------------------------------------------------------------
        fn glyph_outline(font: u64, glyph_id: u32) -> OutlineResult;
        fn glyph_bounds(font: u64, glyph_id: u32) -> GlyphBoundsDesc;
        /// Format the engine would pick for glyphs of this pixel size (0 SDF, 1 MSDF, 2 color).
        fn select_raster_format(long_side_px: f32, previous: u8) -> u8;
        fn rasterize_glyph(font: u64, glyph_id: u32, params: &RasterParamsDesc) -> RasterResult;
        /// 0 = not a color glyph, 1 = bitmap (e.g. PNG emoji), 2 = layered (COLR).
        fn color_glyph_format(font: u64, glyph_id: u32) -> u8;
        fn rasterize_color_glyph(font: u64, glyph_id: u32, width: u32, height: u32, pixel_size: f32, padding_px: f32) -> RasterResult;
    }
}
