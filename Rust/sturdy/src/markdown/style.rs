//! [`MarkdownStyle`] and the small types it is made of.

use std::collections::HashMap;
use std::sync::Arc;

use glam::{Vec2, Vec4};

use crate::assets::RendererTextureHandle;
pub use crate::widgets::LineStyle;

/// A texture the application loaded for an image the document asked for (see [`MarkdownEvent::ImageRequested`](super::MarkdownEvent)).
pub type ImageHandle = RendererTextureHandle;

/// One coloured range of a code block (character offsets into the block's text).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CodeSpan {
    pub start: usize,
    pub len: usize,
    pub color: Vec4,
}

/// Colours the code in fenced code blocks: called with the fence's language (may be empty) and the code, returns the
/// coloured ranges. [`MarkdownHighlighter`](super::MarkdownHighlighter) is a ready-made one; plug in `syntect` (or anything) here:
///
/// ```ignore
/// style.code_colorizer = Some(CodeColorizer::new(|language, code| my_syntect_spans(language, code)));
/// ```
#[derive(Clone)]
pub struct CodeColorizer(pub(super) Arc<ColorizeFn>);

type ColorizeFn = dyn Fn(&str, &str) -> Vec<CodeSpan> + Send + Sync;

impl CodeColorizer {
    pub fn new(colorize: impl Fn(&str, &str) -> Vec<CodeSpan> + Send + Sync + 'static) -> Self {
        Self(Arc::new(colorize))
    }
}

impl std::fmt::Debug for CodeColorizer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("CodeColorizer")
    }
}

impl PartialEq for CodeColorizer {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

/// What a [`MathHook`] draws in place of the TeX source.
#[derive(Debug, Clone, PartialEq)]
pub enum MathRender {
    /// Replacement text (for example the formula transliterated to Unicode). Used for inline and display math.
    Text(String),
    /// A pre-rendered picture of the formula, `size` in logical pixels. Only display math (`$$..$$`) can show it; inline math
    /// falls back to the TeX source because a text run cannot hold an image.
    Image { texture: ImageHandle, size: Vec2 },
}

/// Lets an application lay out math: called with the TeX source and whether it is display math, returns what to draw instead of the
/// source, or `None` to keep the default (the source in the mono font). Called once per parse, not per frame.
#[derive(Clone)]
pub struct MathHook(pub(super) Arc<MathFn>);

type MathFn = dyn Fn(&str, bool) -> Option<MathRender> + Send + Sync;

impl MathHook {
    pub fn new(render: impl Fn(&str, bool) -> Option<MathRender> + Send + Sync + 'static) -> Self {
        Self(Arc::new(render))
    }
}

impl std::fmt::Debug for MathHook {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("MathHook")
    }
}

impl PartialEq for MathHook {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

/// Which constructs are interpreted. A disabled construct is not an error: its source text is shown as written (`# not a heading`,
/// `**not bold**`), so a chat box can allow `**bold**` and links but leave everything else literal. Changing one re-parses the document.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MarkdownFeatures {
    pub headings: bool,
    /// `{#id .class}` after a heading.
    pub heading_attributes: bool,
    /// `*italic*`.
    pub emphasis: bool,
    /// `**bold**` (and `__underline__` when the style's `underline_syntax` is on).
    pub strong: bool,
    pub strikethrough: bool,
    /// `` `code` ``.
    pub inline_code: bool,
    /// `[text](url)` and `<https://autolinks>`. Bare URLs are the style's `autolink`.
    pub links: bool,
    /// `[[Page]]`.
    pub wikilinks: bool,
    pub images: bool,
    pub footnotes: bool,
    /// `$x$` and `$$x$$`.
    pub math: bool,
    pub superscript: bool,
    pub subscript: bool,
    pub tables: bool,
    pub task_lists: bool,
    /// Bullet and numbered lists.
    pub lists: bool,
    pub block_quotes: bool,
    /// `> [!NOTE]` (needs `block_quotes`).
    pub alerts: bool,
    /// Fenced and indented code blocks.
    pub code_blocks: bool,
    /// `---`.
    pub rules: bool,
    pub definition_lists: bool,
}

impl MarkdownFeatures {
    /// Everything the renderer knows (the default).
    pub const fn all() -> Self {
        Self {
            headings: true,
            heading_attributes: true,
            emphasis: true,
            strong: true,
            strikethrough: true,
            inline_code: true,
            links: true,
            wikilinks: true,
            images: true,
            footnotes: true,
            math: true,
            superscript: true,
            subscript: true,
            tables: true,
            task_lists: true,
            lists: true,
            block_quotes: true,
            alerts: true,
            code_blocks: true,
            rules: true,
            definition_lists: true,
        }
    }

    /// Nothing: the whole document is plain text.
    pub const fn none() -> Self {
        Self {
            headings: false,
            heading_attributes: false,
            emphasis: false,
            strong: false,
            strikethrough: false,
            inline_code: false,
            links: false,
            wikilinks: false,
            images: false,
            footnotes: false,
            math: false,
            superscript: false,
            subscript: false,
            tables: false,
            task_lists: false,
            lists: false,
            block_quotes: false,
            alerts: false,
            code_blocks: false,
            rules: false,
            definition_lists: false,
        }
    }

    /// CommonMark proper: no extensions (tables, strikethrough, task lists, footnotes, definition lists, math, sub/superscript, heading
    /// attributes, alerts, wiki links).
    pub const fn commonmark() -> Self {
        Self {
            heading_attributes: false,
            strikethrough: false,
            wikilinks: false,
            footnotes: false,
            math: false,
            superscript: false,
            subscript: false,
            tables: false,
            task_lists: false,
            alerts: false,
            definition_lists: false,
            ..Self::all()
        }
    }

    /// Inline formatting only, for labels, tooltips, list rows and table cells of your own: emphasis, strikethrough, code, links, math.
    /// Every block construct stays literal.
    pub const fn inline() -> Self {
        Self { emphasis: true, strong: true, strikethrough: true, inline_code: true, links: true, math: true, ..Self::none() }
    }

    /// Chat messages: inline formatting plus code blocks, block quotes and lists.
    pub const fn chat() -> Self {
        Self { code_blocks: true, block_quotes: true, lists: true, ..Self::inline() }
    }
}

impl Default for MarkdownFeatures {
    fn default() -> Self {
        Self::all()
    }
}

/// How math (`$..$`, `$$..$$`) is drawn.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MathMode {
    /// The TeX source in the mono font.
    Source,
    /// Inline and display math as one line of Unicode text (`x²`, `α ≤ β`); fractions as `a/b`.
    Unicode,
    /// Inline math as text with real raised and lowered runs for scripts (`e^{i\pi}`; a text run cannot hold boxes, so fractions and
    /// roots are written as Unicode text); display math laid out as boxes ([`tex::draw`](super::tex::draw)).
    /// A formula that does not parse falls back to the source.
    #[default]
    Layout,
}

/// How a single newline inside a paragraph is drawn.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SoftBreak {
    /// As a space (CommonMark).
    #[default]
    Space,
    /// As a line break (chat apps).
    Newline,
}

/// How the numbers of an ordered list are written.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NumberFormat {
    /// 1. 2. 3.
    #[default]
    Decimal,
    /// a. b. c. ... z. aa.
    LowerAlpha,
    /// A. B. C.
    UpperAlpha,
    /// i. ii. iii.
    LowerRoman,
    /// I. II. III.
    UpperRoman,
}

/// Whether links are underlined.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LinkUnderline {
    #[default]
    Always,
    Never,
    /// Only the link under the pointer.
    OnHover,
}

/// Colours of the token kinds the built-in highlighter distinguishes.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CodeColours {
    pub keyword: Vec4,
    pub string: Vec4,
    pub comment: Vec4,
    pub number: Vec4,
    /// Type names, and object keys in JSON/YAML/TOML.
    pub ty: Vec4,
    pub function: Vec4,
    pub punctuation: Vec4,
}

impl Default for CodeColours {
    fn default() -> Self {
        Self {
            keyword: Vec4::new(0.78, 0.52, 0.92, 1.0),
            string: Vec4::new(0.62, 0.82, 0.45, 1.0),
            comment: Vec4::new(0.45, 0.50, 0.56, 1.0),
            number: Vec4::new(0.95, 0.65, 0.35, 1.0),
            ty: Vec4::new(0.35, 0.78, 0.85, 1.0),
            function: Vec4::new(0.45, 0.65, 1.0, 1.0),
            punctuation: Vec4::new(0.70, 0.72, 0.76, 1.0),
        }
    }
}

/// The accent colour of each GitHub-style alert (`> [!NOTE]`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AlertColours {
    pub note: Vec4,
    pub tip: Vec4,
    pub important: Vec4,
    pub warning: Vec4,
    pub caution: Vec4,
}

impl Default for AlertColours {
    fn default() -> Self {
        Self {
            note: Vec4::new(0.35, 0.60, 0.98, 1.0),
            tip: Vec4::new(0.30, 0.78, 0.45, 1.0),
            important: Vec4::new(0.66, 0.50, 0.96, 1.0),
            warning: Vec4::new(0.90, 0.70, 0.20, 1.0),
            caution: Vec4::new(0.95, 0.35, 0.35, 1.0),
        }
    }
}

/// The result the application has for an image URL.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum MarkdownImage {
    /// Loaded: draw it. `size` is the image's own size in logical pixels.
    Ready { texture: ImageHandle, size: Vec2 },
    /// Loading failed: keep showing the alt text.
    Failed,
}

/// What the application gives [`MarkdownView::show_with`](super::MarkdownView::show_with) besides the style.
#[derive(Debug, Clone, PartialEq)]
pub struct MarkdownResources {
    /// Image URL (exactly as written in the document) to what the app has for it. A URL that is absent is pending: the alt text shows
    /// and [`MarkdownEvent::ImageRequested`](super::MarkdownEvent) fires once.
    pub images: HashMap<String, MarkdownImage>,
    /// The scroll container the view sits in. Footnote references, back-links and `#heading` links scroll it to their target; without
    /// it they only report [`MarkdownEvent::AnchorClicked`](super::MarkdownEvent).
    pub scroll_container: Option<String>,
    /// Seconds since the previous frame; drives the image tooltip delay.
    pub delta_seconds: f32,
    /// The width (pixels) to wrap text to before the view has been laid out once, so the first frame is not drawn unwrapped. `None` uses
    /// what the view or a [`MarkdownViews`](super::MarkdownViews) learnt from earlier frames.
    pub width_hint: Option<f32>,
    /// Report [`MarkdownEvent::BlockClicked`](super::MarkdownEvent) when a text block is clicked (editors want it; a read-only view does not).
    pub report_block_clicks: bool,
}

impl Default for MarkdownResources {
    fn default() -> Self {
        Self { images: HashMap::new(), scroll_container: None, delta_seconds: 1.0 / 60.0, width_hint: None, report_block_clicks: false }
    }
}

/// Fonts, colours and spacing of a [`MarkdownView`](super::MarkdownView). Every field added after the first release defaults to the
/// original look (or is a feature that is opt-in).
#[derive(Debug, Clone, PartialEq)]
pub struct MarkdownStyle {
    // -- Original fields -------------------------------------------------------------------------------------------------------------
    /// Colours fenced code blocks; `None` draws them in the plain text colour (or with the built-in highlighter, see `highlight_code`).
    pub code_colorizer: Option<CodeColorizer>,
    pub text: Vec4,
    pub muted: Vec4,
    /// Normal link colour.
    pub link: Vec4,
    pub code_background: Vec4,
    pub quote_bar: Vec4,
    pub rule: Vec4,
    pub font_size: u16,
    /// Height of a line of prose in pixels; 0 uses the font's own.
    pub line_height: u16,
    /// Extra pixels between characters of prose.
    pub letter_spacing: u16,
    pub regular_font: u16,
    pub bold_font: u16,
    pub italic_font: u16,
    pub bold_italic_font: u16,
    pub mono_font: u16,
    /// Size multipliers for heading levels 1 to 6.
    pub heading_scale: [f32; 6],
    /// Space between blocks.
    pub block_gap: u16,
    /// Indent per list level, logical pixels.
    pub list_indent: u16,

    // -- Parser switches (changing one re-parses the document) -------------------------------------------------------------------------
    /// Curly quotes, en/em dashes and ellipses.
    pub smart_punctuation: bool,
    /// `__text__` is underlined instead of bold.
    pub underline_syntax: bool,
    /// How a single newline inside a paragraph is drawn.
    pub soft_break: SoftBreak,
    /// Turns bare `https://..`, `http://..` and `www...` in plain text into links.
    pub autolink: bool,
    /// Writes `^sup^`/`~sub~` with Unicode superscript/subscript characters when every character has one, instead of a smaller run
    /// raised or lowered from the baseline (the default). The Unicode forms avoid needing the font to be consistent between sizes but
    /// cover only digits, signs and a few letters, and depend on the font having those glyphs.
    pub unicode_scripts: bool,
    /// Draw task items as the characters ☑/☐ in the text instead of check boxes.
    pub task_glyphs: bool,
    /// Lays out math, see [`MathHook`]; it takes precedence over `math_mode`.
    pub math_hook: Option<MathHook>,
    /// How math is drawn when there is no hook.
    pub math_mode: MathMode,
    /// Which constructs are interpreted at all.
    pub features: MarkdownFeatures,

    // -- Font slots (`None` keeps the default for that element) -----------------------------------------------------------------------
    /// Headings; default `bold_font`.
    pub heading_font: Option<u16>,
    /// Text inside block quotes and alerts; default `regular_font`.
    pub quote_font: Option<u16>,
    /// Table body cells; default `regular_font`.
    pub table_font: Option<u16>,
    /// Table header cells; default `bold_font`.
    pub table_header_font: Option<u16>,
    /// Link text; default the font of the surrounding run.
    pub link_font: Option<u16>,
    /// List bullets and numbers; default `regular_font`.
    pub list_marker_font: Option<u16>,

    // -- Links -----------------------------------------------------------------------------------------------------------------------
    /// Colour of a link whose URL was already clicked in this view; `None` keeps `link`.
    pub link_visited: Option<Vec4>,
    pub link_underline: LinkUnderline,
    /// Colour of a link under the pointer; `None` keeps `link`.
    pub link_hover: Option<Vec4>,
    /// Colour of a link while the pointer is held down on it; `None` keeps `link_hover`.
    pub link_pressed: Option<Vec4>,
    /// Solid, dashed, dotted or double.
    pub link_underline_style: LineStyle,
    /// Underline thickness in pixels; 0 picks one from the font size.
    pub link_underline_thickness: f32,
    /// Underline colour; `None` uses the link's own colour.
    pub link_underline_color: Option<Vec4>,
    /// Show a link's title (`[a](url "title")`) in a tooltip after the pointer rests on it.
    pub link_tooltips: bool,

    // -- Spacing (`None` keeps `block_gap`) -----------------------------------------------------------------------------------------
    pub heading_margin_top: Option<u16>,
    pub heading_margin_bottom: Option<u16>,
    pub rule_margin: Option<u16>,
    /// Gap between consecutive list items (and footnotes, definition entries).
    pub list_item_gap: Option<u16>,
    pub rule_thickness: f32,
    /// Indent per block-quote level (the bar sits left of it).
    pub quote_padding: u16,
    pub quote_bar_width: u16,
    /// Gap between a list marker and its text.
    pub list_marker_gap: u16,
    /// Indent of a definition's text under its term.
    pub definition_indent: u16,

    // -- Lists -----------------------------------------------------------------------------------------------------------------------
    /// Bullet of each list level, cycling.
    pub bullets: Vec<String>,
    pub number_format: NumberFormat,
    /// Written after an ordered-list number.
    pub number_suffix: String,

    // -- Tables ----------------------------------------------------------------------------------------------------------------------
    /// Header row background; `None` keeps `code_background`.
    pub table_header_background: Option<Vec4>,
    /// Background of every second body row; transparent for none.
    pub table_zebra: Vec4,
    /// Border colour; `None` keeps `rule`.
    pub table_border: Option<Vec4>,
    pub table_cell_padding_x: u16,
    pub table_cell_padding_y: u16,

    // -- Code ------------------------------------------------------------------------------------------------------------------------
    pub code_padding: u16,
    pub code_corner_radius: f32,
    /// Colours of the built-in highlighter.
    pub code_colours: CodeColours,
    /// Colours fenced code with [`MarkdownHighlighter`](super::MarkdownHighlighter) when `code_colorizer` is `None`.
    pub highlight_code: bool,
    pub code_language_label: bool,
    pub code_copy_button: bool,
    /// A line-number gutter. Long lines wrap (the engine's text blocks always do), which pushes the numbers out of step with them.
    pub code_line_numbers: bool,
    /// Wrap long lines of code. Off, a line scrolls sideways with a scrollbar (and a line-number gutter stays in step with the code).
    pub code_wrap: bool,
    /// Height of a line of code in pixels; 0 uses the font's own.
    pub code_line_height: u16,
    /// Longest a code block grows before it scrolls.
    pub code_max_height: Option<f32>,

    // -- Alerts, scripts, tasks, images -----------------------------------------------------------------------------------------------
    pub alert_colours: AlertColours,
    /// Size multiplier of `^sup^`/`~sub~` runs.
    pub script_scale: f32,
    /// How far a `^superscript^` run is raised from the baseline, as a fraction of the font size.
    pub superscript_shift: f32,
    /// How far a `~subscript~` run is lowered below the baseline (negative), as a fraction of the font size.
    pub subscript_shift: f32,
    /// Colour of inline math; `None` keeps the text colour.
    pub math_colour: Option<Vec4>,
    /// Check boxes cannot be toggled.
    pub task_read_only: bool,
    /// Fill colour of a checked box.
    pub task_accent: Vec4,
    pub task_box_size: f32,
    pub image_max_height: f32,
    /// Stretch images smaller than the column up to its width.
    pub image_upscale: bool,
    /// Inside a scroll container ([`MarkdownResources::scroll_container`]) blocks further than a viewport away from what is visible are drawn
    /// as empty space of the height they had, so a long document (a chat with hundreds of messages) costs only what is on screen. Images
    /// are only requested once they come near. Selection state is kept for the skipped blocks.
    pub cull_offscreen: bool,
}

impl MarkdownStyle {
    /// Chat messages: only the inline constructs plus code blocks, quotes and lists are interpreted, newlines are kept, check boxes and links
    /// are read-only/hover-underlined, code gets syntax colours, a language label and a copy button, and the gaps are tight.
    pub fn chat() -> Self {
        Self {
            features: MarkdownFeatures::chat(),
            soft_break: SoftBreak::Newline,
            task_read_only: true,
            link_underline: LinkUnderline::OnHover,
            highlight_code: true,
            code_language_label: true,
            code_copy_button: true,
            block_gap: 6,
            ..Self::default()
        }
    }

    /// Long documents (READMEs, changelogs, notes): everything on, code highlighted with a copy button.
    pub fn document() -> Self {
        Self { highlight_code: true, code_copy_button: true, code_language_label: true, ..Self::default() }
    }
}

impl Default for MarkdownStyle {
    fn default() -> Self {
        let link = Vec4::new(0.45, 0.65, 1.0, 1.0);
        Self {
            code_colorizer: None,
            text: Vec4::new(0.92, 0.93, 0.95, 1.0),
            muted: Vec4::new(0.62, 0.64, 0.68, 1.0),
            link,
            code_background: Vec4::new(1.0, 1.0, 1.0, 0.06),
            quote_bar: Vec4::new(0.35, 0.55, 0.85, 0.7),
            rule: Vec4::new(1.0, 1.0, 1.0, 0.12),
            font_size: 15,
            line_height: 0,
            letter_spacing: 0,
            regular_font: 0,
            bold_font: 0,
            italic_font: 0,
            bold_italic_font: 0,
            mono_font: 0,
            heading_scale: [1.8, 1.5, 1.3, 1.15, 1.05, 1.0],
            block_gap: 10,
            list_indent: 20,
            smart_punctuation: false,
            underline_syntax: false,
            soft_break: SoftBreak::Space,
            autolink: true,
            unicode_scripts: false,
            task_glyphs: false,
            math_hook: None,
            math_mode: MathMode::Layout,
            features: MarkdownFeatures::all(),
            heading_font: None,
            quote_font: None,
            table_font: None,
            table_header_font: None,
            link_font: None,
            list_marker_font: None,
            link_visited: None,
            link_underline: LinkUnderline::Always,
            link_hover: None,
            link_pressed: None,
            link_underline_style: LineStyle::Solid,
            link_underline_thickness: 0.0,
            link_underline_color: None,
            link_tooltips: true,
            heading_margin_top: None,
            heading_margin_bottom: None,
            rule_margin: None,
            list_item_gap: None,
            rule_thickness: 1.0,
            quote_padding: 12,
            quote_bar_width: 3,
            list_marker_gap: 6,
            definition_indent: 20,
            bullets: vec!["\u{2022}".to_owned()],
            number_format: NumberFormat::Decimal,
            number_suffix: ".".to_owned(),
            table_header_background: None,
            table_zebra: Vec4::ZERO,
            table_border: None,
            table_cell_padding_x: 8,
            table_cell_padding_y: 4,
            code_padding: 10,
            code_corner_radius: 6.0,
            code_colours: CodeColours::default(),
            highlight_code: false,
            code_language_label: false,
            code_copy_button: false,
            code_line_numbers: false,
            code_wrap: true,
            code_line_height: 0,
            code_max_height: None,
            alert_colours: AlertColours::default(),
            script_scale: 0.7,
            superscript_shift: 0.35,
            subscript_shift: -0.15,
            math_colour: None,
            task_read_only: false,
            task_accent: link,
            task_box_size: 16.0,
            image_max_height: 400.0,
            image_upscale: false,
            cull_offscreen: true,
        }
    }
}
