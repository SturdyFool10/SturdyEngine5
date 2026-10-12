//! The engine's text package: fonts, OpenType shaping with fallback, line breaking and layout,
//! glyph outlines and rasterization — all CPU-side and usable without an [`crate::Engine`].
//!
//! Fonts are loaded into a process-wide registry and referred to by [`FontId`]. Layout results are
//! in **em** units (multiply by a pixel size to get pixels); [`crate::text_render`] turns a layout
//! into pixels — on screen, in the world, or into a texture your shaders can sample.
//!
//! ```no_run
//! use sturdy::text::{self, FontStack, LayoutOptions};
//! let font = text::default_font().unwrap();
//! let layout = text::layout(&FontStack::new(font), "Hello, wörld!", &LayoutOptions::default()).unwrap();
//! println!("{} px wide at 32 px", layout.width_em * 32.0);
//! ```

use std::path::{Path, PathBuf};

use cxx::UniquePtr;
use glam::Vec2;
use sturdy_sys::text::ffi;

/// Why a text operation failed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TextError(pub String);

impl core::fmt::Display for TextError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for TextError {}

fn check(ok: bool, error: String) -> Result<(), TextError> {
    if ok { Ok(()) } else { Err(TextError(error)) }
}

// -- Fonts -------------------------------------------------------------------------------------

/// A font in the process-wide registry. Ids are never reused after [`unload_font`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FontId(pub(crate) u64);

impl FontId {
    /// The raw registry id (what glyph placements carry as `font_id`).
    pub fn raw(self) -> u64 {
        self.0
    }

    /// Design-unit metrics, or `None` if the font was unloaded.
    pub fn metrics(self) -> Option<FontMetrics> {
        let m = ffi::font_metrics(self.0);
        m.ok.then_some(FontMetrics {
            units_per_em: m.units_per_em,
            ascender: m.ascender,
            descender: m.descender,
            line_gap: m.line_gap,
        })
    }

    /// The OpenType features the font supports for `script`/`language` (empty for the defaults).
    pub fn features(self, script: &str, language: &str) -> Vec<Feature> {
        ffi::font_features(self.0, script, language)
            .into_iter()
            .map(|f| Feature { tag: f.tag, name: f.name })
            .collect()
    }
}

/// Design-unit metrics; `units_per_em` design units make one em.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FontMetrics {
    pub units_per_em: u32,
    pub ascender: i32,
    pub descender: i32,
    pub line_gap: i32,
}

/// One OpenType feature a font supports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Feature {
    /// Four-character tag, e.g. `liga`.
    pub tag: String,
    pub name: String,
}

fn font_result(r: ffi::FontLoadResult) -> Result<FontId, TextError> {
    check(r.ok, r.error)?;
    Ok(FontId(r.id))
}

/// Loads a font from memory (TTF/OTF/TTC; `face_index` selects a face in a collection).
pub fn load_font_bytes(data: &[u8], face_index: u32) -> Result<FontId, TextError> {
    font_result(ffi::font_load_bytes(data, face_index))
}

/// Loads a font file.
pub fn load_font_file(path: impl AsRef<Path>, face_index: u32) -> Result<FontId, TextError> {
    font_result(ffi::font_load_file(&path.as_ref().to_string_lossy(), face_index))
}

/// The engine's embedded default font (the same id on every call).
pub fn default_font() -> Result<FontId, TextError> {
    font_result(ffi::font_default())
}

/// Frees a font. Anything still using its id (stacks, cached layouts) will fail afterwards.
pub fn unload_font(font: FontId) -> bool {
    ffi::font_unload(font.0)
}

/// Directories the platform keeps fonts in.
pub fn font_search_directories() -> Vec<PathBuf> {
    ffi::font_search_directories().into_iter().map(PathBuf::from).collect()
}

/// One installed font face.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FontFace {
    pub family: String,
    pub subfamily: String,
    pub bold: bool,
    pub italic: bool,
    pub path: PathBuf,
    /// Face index within the file (for collections).
    pub face_index: u32,
}

/// A searchable index of installed fonts.
pub struct FontDatabase(UniquePtr<ffi::FontDatabaseBox>);

impl FontDatabase {
    /// Indexes the fonts under `directories` (recursively).
    pub fn new(directories: &[PathBuf]) -> Self {
        let dirs: Vec<String> = directories.iter().map(|d| d.to_string_lossy().into_owned()).collect();
        Self(ffi::font_database_new(&dirs))
    }

    /// Indexes the platform's font directories.
    pub fn system() -> Self {
        Self::new(&font_search_directories())
    }

    pub fn faces(&self) -> Vec<FontFace> {
        ffi::font_database_faces(&self.0)
            .into_iter()
            .map(|f| FontFace {
                family: f.family,
                subfamily: f.subfamily,
                bold: f.bold,
                italic: f.italic,
                path: PathBuf::from(f.file_path),
                face_index: f.face_index,
            })
            .collect()
    }

    /// The file of the best match for a family and style.
    pub fn find(&self, family: &str, bold: bool, italic: bool) -> Option<PathBuf> {
        let path = ffi::font_database_find(&self.0, family, bold, italic);
        (!path.is_empty()).then(|| PathBuf::from(path))
    }

    /// Finds and loads a family in one step.
    pub fn load(&self, family: &str, bold: bool, italic: bool) -> Result<FontId, TextError> {
        let path = self.find(family, bold, italic).ok_or_else(|| TextError(format!("no installed font matches {family:?}")))?;
        let face = self
            .faces()
            .into_iter()
            .find(|f| f.path == path && f.bold == bold && f.italic == italic)
            .map_or(0, |f| f.face_index);
        load_font_file(path, face)
    }
}

/// Checks an OpenType feature specification such as `"liga=0, smcp, ss01"`.
pub fn validate_features(specification: &str) -> Result<(), TextError> {
    let error = ffi::feature_spec_error(specification);
    if error.is_empty() { Ok(()) } else { Err(TextError(error)) }
}

// -- Shaping and layout ------------------------------------------------------------------------

/// A primary font plus what to fall back to for characters it lacks.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FontStack {
    pub primary: FontId,
    pub emoji: Option<FontId>,
    /// Whether the emoji font's glyphs are color glyphs (bitmap/layered) rather than outlines.
    pub emoji_is_color: bool,
    pub fallbacks: Vec<FontId>,
}

impl FontStack {
    pub fn new(primary: FontId) -> Self {
        Self { primary, emoji: None, emoji_is_color: true, fallbacks: Vec::new() }
    }

    #[must_use]
    pub fn with_emoji(mut self, emoji: FontId) -> Self {
        self.emoji = Some(emoji);
        self
    }

    #[must_use]
    pub fn with_fallback(mut self, font: FontId) -> Self {
        self.fallbacks.push(font);
        self
    }

    pub(crate) fn to_ffi(&self) -> ffi::FontStackDesc {
        ffi::FontStackDesc {
            primary: self.primary.0,
            emoji: self.emoji.map_or(0, |f| f.0),
            emoji_is_color: self.emoji_is_color,
            fallbacks: self.fallbacks.iter().map(|f| f.0).collect(),
        }
    }
}

/// Base text direction.
pub use ffi::TextDirectionKind as Direction;
/// Horizontal alignment of laid-out lines.
pub use ffi::TextAlignmentKind as Alignment;

/// Shaping controls.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShapeOptions {
    /// ISO 15924 script tag (e.g. `Latn`); empty auto-detects.
    pub script: String,
    /// BCP 47 language tag; empty for none.
    pub language: String,
    pub direction: Direction,
    /// Comma-separated OpenType feature specification, e.g. `"liga=0, smcp"`.
    pub features: String,
}

impl Default for ShapeOptions {
    fn default() -> Self {
        Self { script: String::new(), language: String::new(), direction: Direction::Auto, features: String::new() }
    }
}

impl ShapeOptions {
    fn to_ffi(&self) -> ffi::ShapeOptionsDesc {
        ffi::ShapeOptionsDesc {
            script: self.script.clone(),
            language: self.language.clone(),
            direction: self.direction,
            features: self.features.clone(),
        }
    }
}

/// Layout controls.
#[derive(Debug, Clone, PartialEq)]
pub struct LayoutOptions {
    /// Wrap lines wider than this many ems; `None` only breaks at newlines.
    pub max_width_em: Option<f32>,
    pub alignment: Alignment,
    pub shape: ShapeOptions,
    /// Language used for line-break rules.
    pub break_language: String,
}

impl Default for LayoutOptions {
    fn default() -> Self {
        Self { max_width_em: None, alignment: Alignment::Start, shape: ShapeOptions::default(), break_language: String::new() }
    }
}

impl LayoutOptions {
    /// Wrap at `width_px` when the text is drawn at `pixel_size`.
    #[must_use]
    pub fn wrap_at_px(mut self, width_px: f32, pixel_size: f32) -> Self {
        self.max_width_em = Some(width_px / pixel_size);
        self
    }

    #[must_use]
    pub fn alignment(mut self, alignment: Alignment) -> Self {
        self.alignment = alignment;
        self
    }

    #[must_use]
    pub fn features(mut self, specification: impl Into<String>) -> Self {
        self.shape.features = specification.into();
        self
    }

    fn to_ffi(&self) -> ffi::LayoutOptionsDesc {
        ffi::LayoutOptionsDesc {
            has_max_width: self.max_width_em.is_some(),
            max_width_em: self.max_width_em.unwrap_or(0.0),
            alignment: self.alignment,
            shape: self.shape.to_ffi(),
            break_language: self.break_language.clone(),
        }
    }
}

/// One shaped glyph. Its advances and offsets are in the glyph's font's *design units*: multiply by
/// [`Run::scale`] to get ems, or use [`Layout::glyph_positions`], which does it for you.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Glyph {
    pub glyph_id: u32,
    pub x_advance: f32,
    pub y_advance: f32,
    pub x_offset: f32,
    pub y_offset: f32,
    /// Byte offset in the text of the character this glyph came from.
    pub cluster: u32,
    pub font: FontId,
    pub is_color: bool,
}

impl From<ffi::GlyphDesc> for Glyph {
    fn from(g: ffi::GlyphDesc) -> Self {
        Self {
            glyph_id: g.glyph_id,
            x_advance: g.x_advance,
            y_advance: g.y_advance,
            x_offset: g.x_offset,
            y_offset: g.y_offset,
            cluster: g.cluster,
            font: FontId(g.font_id),
            is_color: g.is_color,
        }
    }
}

/// A same-font, same-direction stretch of glyphs within a line.
#[derive(Debug, Clone, PartialEq)]
pub struct Run {
    pub font: FontId,
    pub units_per_em: u32,
    pub is_color: bool,
    pub direction: Direction,
    /// Where the run starts within its line, in ems.
    pub pen_origin_em: f32,
    pub advance_em: f32,
    pub glyphs: Vec<Glyph>,
}

impl Run {
    /// Multiplier turning this run's glyph advances/offsets (design units) into ems.
    pub fn scale(&self) -> f32 {
        1.0 / self.units_per_em.max(1) as f32
    }
}

/// One laid-out line.
#[derive(Debug, Clone, PartialEq)]
pub struct Line {
    /// The line's span in the text; `visible_byte_end` excludes trailing break characters.
    pub byte_start: usize,
    pub visible_byte_end: usize,
    pub byte_end: usize,
    pub mandatory_break_after: bool,
    /// Alignment shift of the whole line, in ems.
    pub offset_em: f32,
    pub advance_em: f32,
    pub base_direction: Direction,
    pub runs: Vec<Run>,
}

/// Shaped, broken and aligned text, in em units.
#[derive(Debug, Clone, PartialEq)]
pub struct Layout {
    pub lines: Vec<Line>,
    pub width_em: f32,
    pub height_em: f32,
    pub ascender_em: f32,
    pub descender_em: f32,
    pub line_gap_em: f32,
    pub line_height_em: f32,
}

impl Layout {
    /// Size in pixels when drawn at `pixel_size`.
    pub fn size_px(&self, pixel_size: f32) -> Vec2 {
        Vec2::new(self.width_em, self.height_em) * pixel_size
    }

    /// Every glyph with its pen position in ems from the layout's top-left (baseline of line `i`
    /// at `ascender_em + i * line_height_em`), y growing downwards.
    pub fn glyph_positions(&self) -> Vec<(Glyph, Vec2)> {
        let mut out = Vec::new();
        for (index, line) in self.lines.iter().enumerate() {
            let baseline = self.ascender_em + index as f32 * self.line_height_em;
            for run in &line.runs {
                let scale = run.scale();
                let mut pen = Vec2::new(line.offset_em + run.pen_origin_em, baseline);
                for glyph in &run.glyphs {
                    out.push((*glyph, pen + Vec2::new(glyph.x_offset * scale, -glyph.y_offset * scale)));
                    pen += Vec2::new(glyph.x_advance * scale, -glyph.y_advance * scale);
                }
            }
        }
        out
    }
}

impl Layout {
    /// Like [`Layout::glyph_positions`], also giving each glyph's run `units_per_em`.
    pub fn glyph_positions_with_units(&self) -> Vec<(Glyph, Vec2, u32)> {
        let mut out = Vec::new();
        for (index, line) in self.lines.iter().enumerate() {
            let baseline = self.ascender_em + index as f32 * self.line_height_em;
            for run in &line.runs {
                let scale = run.scale();
                let mut pen = Vec2::new(line.offset_em + run.pen_origin_em, baseline);
                for glyph in &run.glyphs {
                    out.push((*glyph, pen + Vec2::new(glyph.x_offset * scale, -glyph.y_offset * scale), run.units_per_em));
                    pen += Vec2::new(glyph.x_advance * scale, -glyph.y_advance * scale);
                }
            }
        }
        out
    }
}

/// Shapes `text` with fallback into a flat glyph list (no line breaking).
pub fn shape(stack: &FontStack, text: &str, options: &ShapeOptions) -> Result<Vec<Glyph>, TextError> {
    let r = ffi::text_shape(&stack.to_ffi(), text, &options.to_ffi());
    check(r.ok, r.error)?;
    Ok(r.glyphs.into_iter().map(Glyph::from).collect())
}

/// Shapes, breaks and aligns `text`.
pub fn layout(stack: &FontStack, text: &str, options: &LayoutOptions) -> Result<Layout, TextError> {
    let r = ffi::text_layout(&stack.to_ffi(), text, &options.to_ffi());
    check(r.ok, r.error)?;
    let glyphs: Vec<Glyph> = r.glyphs.iter().copied().map(Glyph::from).collect();
    let runs: Vec<Run> = r
        .runs
        .iter()
        .map(|run| Run {
            font: FontId(run.font_id),
            units_per_em: run.units_per_em,
            is_color: run.is_color,
            direction: run.direction,
            pen_origin_em: run.pen_origin_em,
            advance_em: run.advance_em,
            glyphs: glyphs[run.glyph_start as usize..(run.glyph_start + run.glyph_count) as usize].to_vec(),
        })
        .collect();
    let lines = r
        .lines
        .iter()
        .map(|line| Line {
            byte_start: line.byte_start as usize,
            visible_byte_end: line.visible_byte_end as usize,
            byte_end: line.byte_end as usize,
            mandatory_break_after: line.mandatory_break_after,
            offset_em: line.offset_em,
            advance_em: line.advance_em,
            base_direction: line.base_direction,
            runs: runs[line.run_start as usize..(line.run_start + line.run_count) as usize].to_vec(),
        })
        .collect();
    Ok(Layout {
        lines,
        width_em: r.width_em,
        height_em: r.height_em,
        ascender_em: r.ascender_em,
        descender_em: r.descender_em,
        line_gap_em: r.line_gap_em,
        line_height_em: r.line_height_em,
    })
}

/// A place where a line may (or must) break.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BreakOpportunity {
    pub byte_index: usize,
    /// A forced break (newline) rather than merely an allowed one.
    pub mandatory: bool,
}

/// Unicode line-break opportunities in `text`.
pub fn line_break_opportunities(text: &str, language: &str) -> Vec<BreakOpportunity> {
    let list = ffi::line_break_opportunities(text, language);
    list.byte_index
        .iter()
        .zip(&list.mandatory)
        .map(|(&i, &m)| BreakOpportunity { byte_index: i as usize, mandatory: m })
        .collect()
}

/// Byte offsets of user-perceived character (grapheme cluster) boundaries.
pub fn grapheme_boundaries(text: &str, language: &str) -> Vec<usize> {
    ffi::grapheme_boundaries(text, language).into_iter().map(|b| b as usize).collect()
}

/// Byte offsets of word boundaries.
pub fn word_boundaries(text: &str, language: &str) -> Vec<usize> {
    ffi::word_boundaries(text, language).into_iter().map(|b| b as usize).collect()
}

/// Whether `c` is an emoji code point (what routes a character to the emoji font).
pub fn is_emoji(c: char) -> bool {
    ffi::is_emoji_codepoint(c as u32)
}

// -- Outlines and rasterization ----------------------------------------------------------------

/// One drawing command of a glyph outline (font design units, y up).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Segment {
    MoveTo(Vec2),
    LineTo(Vec2),
    QuadTo { control: Vec2, to: Vec2 },
    CubicTo { control1: Vec2, control2: Vec2, to: Vec2 },
}

/// A glyph's vector outline: closed contours of segments.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Outline {
    pub contours: Vec<Vec<Segment>>,
}

/// The vector outline of a glyph.
pub fn glyph_outline(font: FontId, glyph_id: u32) -> Result<Outline, TextError> {
    let r = ffi::glyph_outline(font.0, glyph_id);
    check(r.ok, r.error)?;
    let mut contours = Vec::with_capacity(r.contour_lengths.len());
    let mut index = 0usize;
    for &length in &r.contour_lengths {
        let mut contour = Vec::with_capacity(length as usize);
        for _ in 0..length {
            let p = &r.points[index * 6..index * 6 + 6];
            let (c1, c2, to) = (Vec2::new(p[0], p[1]), Vec2::new(p[2], p[3]), Vec2::new(p[4], p[5]));
            contour.push(match r.kinds[index] {
                0 => Segment::MoveTo(to),
                1 => Segment::LineTo(to),
                2 => Segment::QuadTo { control: c1, to },
                _ => Segment::CubicTo { control1: c1, control2: c2, to },
            });
            index += 1;
        }
        contours.push(contour);
    }
    Ok(Outline { contours })
}

/// Bounding box of a glyph outline in font design units; `None` for empty glyphs (spaces).
pub fn glyph_bounds(font: FontId, glyph_id: u32) -> Option<(Vec2, Vec2)> {
    let b = ffi::glyph_bounds(font.0, glyph_id);
    (!b.empty).then(|| (Vec2::new(b.left, b.bottom), Vec2::new(b.right, b.top)))
}

/// How a glyph is rasterized.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RasterFormat {
    /// Single-channel signed distance field (the default for UI-size text).
    #[default]
    Sdf,
    /// Three-channel multi-channel SDF (sharper corners at large sizes).
    Msdf,
    /// RGBA color glyph (emoji).
    Color,
}

impl RasterFormat {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Msdf,
            2 => Self::Color,
            _ => Self::Sdf,
        }
    }

    /// The format the engine picks for a glyph whose long side is `long_side_px` pixels.
    pub fn select(long_side_px: f32, previous: RasterFormat) -> Self {
        Self::from_u8(ffi::select_raster_format(long_side_px, previous as u8))
    }
}

/// Output size and distance-field parameters for [`rasterize_glyph`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RasterParams {
    pub format: RasterFormat,
    pub width: u32,
    pub height: u32,
    /// Design units to pixels.
    pub scale: f32,
    /// Distance-field range in pixels.
    pub pixel_range: f32,
    pub padding_px: f32,
    /// Extra translation applied before rasterizing (design units); `None` centers the glyph.
    pub translation: Option<Vec2>,
}

impl Default for RasterParams {
    fn default() -> Self {
        Self { format: RasterFormat::Sdf, width: 32, height: 32, scale: 0.03, pixel_range: 4.0, padding_px: 4.0, translation: None }
    }
}

/// A rasterized glyph bitmap: `channel_count` bytes per pixel, rows top to bottom.
#[derive(Debug, Clone, PartialEq)]
pub struct RasterizedGlyph {
    pub width: u32,
    pub height: u32,
    pub channel_count: u32,
    pub bearing_x: f32,
    pub bearing_top: f32,
    pub pixels: Vec<u8>,
}

fn raster_result(r: ffi::RasterResult) -> Result<RasterizedGlyph, TextError> {
    check(r.ok, r.error)?;
    Ok(RasterizedGlyph {
        width: r.width,
        height: r.height,
        channel_count: r.channel_count,
        bearing_x: r.bearing_x,
        bearing_top: r.bearing_top,
        pixels: r.pixels,
    })
}

/// Rasterizes a glyph to a distance-field bitmap.
pub fn rasterize_glyph(font: FontId, glyph_id: u32, params: &RasterParams) -> Result<RasterizedGlyph, TextError> {
    raster_result(ffi::rasterize_glyph(
        font.0,
        glyph_id,
        &ffi::RasterParamsDesc {
            format: params.format as u8,
            width: params.width,
            height: params.height,
            scale: params.scale,
            pixel_range: params.pixel_range,
            padding_px: params.padding_px,
            has_translation: params.translation.is_some(),
            translation_x: params.translation.map_or(0.0, |t| t.x),
            translation_y: params.translation.map_or(0.0, |t| t.y),
        },
    ))
}

/// What kind of color data a glyph has.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorGlyphFormat {
    None,
    /// Embedded bitmap (e.g. PNG emoji).
    Bitmap,
    /// Layered vector color (COLR).
    Layered,
}

pub fn color_glyph_format(font: FontId, glyph_id: u32) -> ColorGlyphFormat {
    match ffi::color_glyph_format(font.0, glyph_id) {
        1 => ColorGlyphFormat::Bitmap,
        2 => ColorGlyphFormat::Layered,
        _ => ColorGlyphFormat::None,
    }
}

/// Rasterizes a color (emoji) glyph to RGBA8 at `pixel_size`.
pub fn rasterize_color_glyph(
    font: FontId,
    glyph_id: u32,
    width: u32,
    height: u32,
    pixel_size: f32,
    padding_px: f32,
) -> Result<RasterizedGlyph, TextError> {
    raster_result(ffi::rasterize_color_glyph(font.0, glyph_id, width, height, pixel_size, padding_px))
}

// -- Documents ---------------------------------------------------------------------------------

/// A half-open byte range of a [`Document`]'s UTF-8 text.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ByteRange {
    pub start: usize,
    pub end: usize,
}

impl ByteRange {
    pub fn new(start: usize, end: usize) -> Self {
        Self { start, end }
    }

    pub fn len(&self) -> usize {
        self.end - self.start
    }

    pub fn is_empty(&self) -> bool {
        self.start == self.end
    }

    fn to_ffi(self) -> ffi::ByteRange {
        ffi::ByteRange { start: self.start as u64, end: self.end as u64 }
    }
}

impl From<ffi::ByteRange> for ByteRange {
    fn from(r: ffi::ByteRange) -> Self {
        Self { start: r.start as usize, end: r.end as usize }
    }
}

/// A line/column position. Columns count Unicode scalars, or UTF-16 code units for the `utf16`
/// conversions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Point {
    pub line: usize,
    pub column: usize,
}

/// How an edit groups for undo: consecutive typing (or deletion) edits merge into one undo step.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum EditKind {
    #[default]
    Standalone,
    Typing,
    Deletion,
}

/// One edit: replace `range` (of the text as it is *before* the transaction) with `replacement`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Edit {
    pub range: ByteRange,
    pub replacement: String,
}

impl Edit {
    pub fn replace(range: ByteRange, replacement: impl Into<String>) -> Self {
        Self { range, replacement: replacement.into() }
    }

    pub fn insert(at: usize, text: impl Into<String>) -> Self {
        Self::replace(ByteRange::new(at, at), text)
    }

    pub fn delete(range: ByteRange) -> Self {
        Self::replace(range, "")
    }
}

/// What an applied edit did.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Change {
    pub old_range: ByteRange,
    pub new_range: ByteRange,
    pub inserted: String,
    pub deleted: String,
}

/// The outcome of [`Document::apply`], [`Document::undo`] or [`Document::redo`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChangeSet {
    pub revision_before: u64,
    pub revision_after: u64,
    pub changes: Vec<Change>,
}

/// Coarse character classes used for word navigation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CharClass {
    Whitespace,
    Word,
    Punctuation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndentKind {
    Spaces,
    Tabs,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LineEnding {
    Lf,
    CrLf,
}

/// Memory and size figures for a document.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DocumentStats {
    pub bytes: usize,
    pub scalars: usize,
    pub utf16_code_units: usize,
    pub newlines: usize,
    pub ascii: bool,
    pub backing_storage_bytes: usize,
    pub tree_nodes: usize,
    pub pieces: usize,
    pub undo_revisions: usize,
    pub redo_revisions: usize,
}

/// An editable, versioned text buffer (a rope) with undo/redo, line and position mapping,
/// word/bracket navigation and search — the engine's editor-grade document type. Offsets are UTF-8
/// byte offsets; use [`Document::clip_offset`] to snap one to a character boundary.
pub struct Document(UniquePtr<ffi::DocumentBox>);

impl Document {
    pub fn new(initial: &str) -> Self {
        Self(ffi::document_new(initial))
    }

    /// Increases with every applied edit, undo and redo.
    pub fn revision(&self) -> u64 {
        ffi::document_revision(&self.0)
    }

    /// The whole text.
    pub fn text(&self) -> String {
        ffi::document_text(&self.0)
    }

    pub fn slice(&self, range: ByteRange) -> String {
        ffi::document_slice(&self.0, range.to_ffi())
    }

    pub fn line_count(&self) -> usize {
        ffi::document_line_count(&self.0) as usize
    }

    /// Byte range of a line (excluding its line ending), `None` when out of range.
    pub fn line_range(&self, line: usize) -> Option<ByteRange> {
        let mut found = false;
        let range = ffi::document_line_range(&self.0, line as u64, &mut found);
        found.then(|| range.into())
    }

    pub fn stats(&self) -> DocumentStats {
        let s = ffi::document_stats(&self.0);
        DocumentStats {
            bytes: s.logical_bytes as usize,
            scalars: s.scalars as usize,
            utf16_code_units: s.utf16_code_units as usize,
            newlines: s.newlines as usize,
            ascii: s.ascii,
            backing_storage_bytes: s.backing_storage_bytes as usize,
            tree_nodes: s.tree_nodes as usize,
            pieces: s.pieces as usize,
            undo_revisions: s.undo_revisions as usize,
            redo_revisions: s.redo_revisions as usize,
        }
    }

    fn change_set(r: ffi::ApplyResultDesc) -> Result<ChangeSet, TextError> {
        check(r.ok, r.error)?;
        Ok(ChangeSet {
            revision_before: r.revision_before,
            revision_after: r.revision_after,
            changes: r
                .changes
                .into_iter()
                .map(|c| Change {
                    old_range: c.old_range.into(),
                    new_range: c.new_range.into(),
                    inserted: c.inserted,
                    deleted: c.deleted,
                })
                .collect(),
        })
    }

    /// Applies edits (non-overlapping ranges of the current text, on character boundaries) as one
    /// undoable transaction. Errors name the engine's `DocumentError` (`InvalidRange`,
    /// `InvalidBoundary`, `OverlappingSplices`, ...).
    pub fn apply(&mut self, edits: &[Edit], kind: EditKind) -> Result<ChangeSet, TextError> {
        let edits: Vec<ffi::EditDesc> =
            edits.iter().map(|e| ffi::EditDesc { range: e.range.to_ffi(), replacement: e.replacement.clone() }).collect();
        Self::change_set(ffi::document_apply(self.0.pin_mut(), &edits, kind as u8))
    }

    /// Replaces `range` with `text`; a convenience over [`Document::apply`].
    pub fn replace(&mut self, range: ByteRange, text: &str) -> Result<ChangeSet, TextError> {
        self.apply(&[Edit::replace(range, text)], EditKind::Standalone)
    }

    pub fn undo(&mut self) -> Result<ChangeSet, TextError> {
        Self::change_set(ffi::document_undo(self.0.pin_mut()))
    }

    pub fn redo(&mut self) -> Result<ChangeSet, TextError> {
        Self::change_set(ffi::document_redo(self.0.pin_mut()))
    }

    pub fn offset_to_point(&self, offset: usize) -> Option<Point> {
        let p = ffi::document_offset_to_point(&self.0, offset as u64);
        p.found.then(|| Point { line: p.line as usize, column: p.column as usize })
    }

    pub fn offset_to_utf16(&self, offset: usize) -> Option<Point> {
        let p = ffi::document_offset_to_utf16(&self.0, offset as u64);
        p.found.then(|| Point { line: p.line as usize, column: p.column as usize })
    }

    pub fn point_to_offset(&self, point: Point) -> Option<usize> {
        let mut found = false;
        let o = ffi::document_point_to_offset(&self.0, point.line as u64, point.column as u64, &mut found);
        found.then_some(o as usize)
    }

    pub fn utf16_to_offset(&self, point: Point) -> Option<usize> {
        let mut found = false;
        let o = ffi::document_utf16_to_offset(&self.0, point.line as u64, point.column as u64, &mut found);
        found.then_some(o as usize)
    }

    /// Snaps `offset` to a character boundary, rounding toward the end when `after`.
    pub fn clip_offset(&self, offset: usize, after: bool) -> usize {
        ffi::document_clip_offset(&self.0, offset as u64, after) as usize
    }

    pub fn char_class_at(&self, offset: usize) -> CharClass {
        match ffi::document_char_class_at(&self.0, offset as u64) {
            1 => CharClass::Word,
            2 => CharClass::Punctuation,
            _ => CharClass::Whitespace,
        }
    }

    /// The word containing `offset`.
    pub fn word_range_at(&self, offset: usize) -> Option<ByteRange> {
        let mut found = false;
        let range = ffi::document_word_range_at(&self.0, offset as u64, &mut found);
        found.then(|| range.into())
    }

    pub fn next_word_boundary(&self, offset: usize) -> usize {
        ffi::document_next_word_boundary(&self.0, offset as u64) as usize
    }

    pub fn previous_word_boundary(&self, offset: usize) -> usize {
        ffi::document_previous_word_boundary(&self.0, offset as u64) as usize
    }

    /// The offset of the bracket matching the one at `offset`.
    pub fn matching_bracket(&self, offset: usize) -> Option<usize> {
        let mut found = false;
        let o = ffi::document_matching_bracket(&self.0, offset as u64, &mut found);
        found.then_some(o as usize)
    }

    /// The next (or previous) occurrence of `pattern` from `from`.
    pub fn find(&self, pattern: &str, from: usize, forward: bool, case_sensitive: bool) -> Option<ByteRange> {
        let mut found = false;
        let range = ffi::document_find(&self.0, pattern, from as u64, forward, case_sensitive, &mut found);
        found.then(|| range.into())
    }

    pub fn find_all(&self, pattern: &str, case_sensitive: bool) -> Vec<ByteRange> {
        ffi::document_find_all(&self.0, pattern, case_sensitive).into_iter().map(Into::into).collect()
    }

    pub fn is_line_blank(&self, line: usize) -> bool {
        ffi::document_is_line_blank(&self.0, line as u64)
    }

    /// Leading whitespace of `line` as `(bytes, scalars)`.
    pub fn line_indent(&self, line: usize) -> (usize, usize) {
        let i = ffi::document_line_indent(&self.0, line as u64);
        (i[0] as usize, i[1] as usize)
    }

    /// The document's dominant indentation.
    pub fn detect_indent(&self) -> (IndentKind, usize) {
        let i = ffi::document_detect_indent(&self.0);
        (if i[0] == 1 { IndentKind::Tabs } else { IndentKind::Spaces }, i[1] as usize)
    }

    pub fn detect_line_ending(&self) -> LineEnding {
        if ffi::document_detect_line_ending(&self.0) == 1 { LineEnding::CrLf } else { LineEnding::Lf }
    }
}

// SAFETY: a Document is plain owned data (a rope of strings) with no thread affinity.
unsafe impl Send for Document {}
