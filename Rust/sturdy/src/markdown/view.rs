//! [`MarkdownView`]: the parsed document and the code that draws it with the engine's UI.

use std::collections::{HashMap, HashSet};
use std::ops::Range;

use glam::{Vec2, Vec4};

use super::highlight::MarkdownHighlighter;
use super::parse::{
    anchors_of, parse_with, AlertKind, Block, ColumnAlign, InlineFormat, LinkKind, LinkTarget, ParseConfig, Parsed, Prose, Script, TaskItem,
};
use super::style::{AlertColours, LineStyle, LinkUnderline, MarkdownImage, MarkdownResources, MarkdownStyle, MathRender, NumberFormat};
use super::tex::{self, TexStyle};
use super::{BlockInfo, BlockKind, MarkdownEvent, MarkdownHeading};
use crate::input::MouseButton;
use crate::keys::Key;
use crate::ui::{AlignX, AlignY, Border, ChildAlignment, Clip, CornerRadius, Cursor, Direction, ElementDesc, Padding, Sizing, TextStyle, Ui};
use crate::ui_kit::{text_tooltip, Shortcut, TooltipState, TooltipStyle};
use crate::widgets::{
    ButtonState, ButtonStyle, RichSpan, ScrollAreaState, ScrollbarStyle, ScrollbarVisibility, TextEditState, TextInputStyle, ToggleState,
    ToggleStyle,
};

pub(super) fn pack_color(color: Vec4) -> u32 {
    let channel = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u32;
    (channel(color.x) << 24) | (channel(color.y) << 16) | (channel(color.z) << 8) | channel(color.w)
}

pub(super) fn unpack_color(packed: u32) -> Vec4 {
    let channel = |shift: u32| ((packed >> shift) & 0xFF) as f32 / 255.0;
    Vec4::new(channel(24), channel(16), channel(8), channel(0))
}

// -- Pure helpers (unit tested) ------------------------------------------------------------------------------------------------------

/// `number` written in `format` (no suffix). Numbers with no alphabetic or Roman form (zero, Roman above 3999) fall back to decimal.
pub(super) fn format_number(number: u64, format: NumberFormat) -> String {
    match format {
        NumberFormat::Decimal => number.to_string(),
        NumberFormat::LowerAlpha | NumberFormat::UpperAlpha => {
            if number == 0 {
                return number.to_string();
            }
            let base = if format == NumberFormat::LowerAlpha { b'a' } else { b'A' };
            let mut letters = Vec::new();
            let mut rest = number;
            while rest > 0 {
                rest -= 1;
                letters.push(char::from(base + (rest % 26) as u8));
                rest /= 26;
            }
            letters.iter().rev().collect()
        }
        NumberFormat::LowerRoman | NumberFormat::UpperRoman => {
            if number == 0 || number > 3999 {
                return number.to_string();
            }
            const NUMERALS: [(u64, &str); 13] =
                [(1000, "M"), (900, "CM"), (500, "D"), (400, "CD"), (100, "C"), (90, "XC"), (50, "L"), (40, "XL"), (10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")];
            let mut rest = number;
            let mut text = String::new();
            for (value, symbol) in NUMERALS {
                while rest >= value {
                    text.push_str(symbol);
                    rest -= value;
                }
            }
            if format == NumberFormat::LowerRoman {
                text.make_ascii_lowercase();
            }
            text
        }
    }
}

/// The marker drawn before a list item: the style's bullet for this level, or the formatted number plus the suffix.
pub(super) fn marker_text(ordinal: Option<u64>, depth: u8, style: &MarkdownStyle) -> String {
    match ordinal {
        Some(number) => format!("{}{}", format_number(number, style.number_format), style.number_suffix),
        None => {
            let level = usize::from(depth);
            style.bullets.get(level.checked_rem(style.bullets.len()).unwrap_or(0)).cloned().unwrap_or_else(|| "\u{2022}".to_owned())
        }
    }
}

fn alert_colour(kind: AlertKind, colours: &AlertColours) -> Vec4 {
    match kind {
        AlertKind::Note => colours.note,
        AlertKind::Tip => colours.tip,
        AlertKind::Important => colours.important,
        AlertKind::Warning => colours.warning,
        AlertKind::Caution => colours.caution,
    }
}

fn quote_of(block: &Block) -> &[Option<AlertKind>] {
    match block {
        Block::Paragraph { quote, .. } | Block::ListItem { quote, .. } | Block::Image { quote, .. } | Block::AlertTitle { quote, .. } => quote,
        _ => &[],
    }
}

fn is_entry(block: &Block) -> bool {
    matches!(block, Block::ListItem { .. } | Block::Footnote { .. } | Block::DefTerm { .. } | Block::DefDetail { .. })
}

/// The space between two neighbouring blocks. A heading or rule that sets its own margin wins over `block_gap`; consecutive list
/// items use `list_item_gap`.
pub(super) fn gap_between(previous: &Block, next: &Block, style: &MarkdownStyle) -> u16 {
    if is_entry(previous) && is_entry(next) {
        return style.list_item_gap.unwrap_or(style.block_gap);
    }
    let top = |block: &Block| match block {
        Block::Heading { .. } => style.heading_margin_top,
        Block::Rule => style.rule_margin,
        _ => None,
    };
    let bottom = |block: &Block| match block {
        Block::Heading { .. } => style.heading_margin_bottom,
        Block::Rule => style.rule_margin,
        _ => None,
    };
    top(next).or_else(|| bottom(previous)).unwrap_or(style.block_gap)
}

/// The size an image is drawn at: its own size, shrunk to the column (or stretched to it when `upscale`), then to `max_height`.
pub(super) fn fit_image(size: Vec2, column_width: Option<f32>, max_height: f32, upscale: bool) -> Vec2 {
    if size.x <= 0.0 || size.y <= 0.0 {
        return Vec2::ZERO;
    }
    let mut scale = 1.0f32;
    if let Some(width) = column_width.filter(|w| *w > 0.0)
        && (size.x > width || upscale)
    {
        scale = width / size.x;
    }
    if max_height > 0.0 && size.y * scale > max_height {
        scale = max_height / size.y;
    }
    Vec2::new((size.x * scale).max(1.0), (size.y * scale).max(1.0))
}

/// The coloured runs of a code block: every char is covered, gaps between colourer ranges are plain code runs.
pub(super) fn code_prose(language: &str, text: &str, style: &MarkdownStyle) -> Prose {
    let total = text.chars().count();
    let plain = InlineFormat { code: true, ..InlineFormat::default() };
    let mut spans = match &style.code_colorizer {
        Some(colorizer) => (colorizer.0)(language, text),
        None if style.highlight_code => MarkdownHighlighter::new(style.code_colours).spans(language, text),
        None => Vec::new(),
    };
    spans.sort_by_key(|span| span.start);
    let mut runs = Vec::new();
    let mut at = 0usize;
    for span in spans {
        let start = span.start.max(at).min(total);
        let end = span.start.saturating_add(span.len).min(total);
        if start > at {
            runs.push((at, start - at, plain));
        }
        if end > start {
            runs.push((start, end - start, InlineFormat { color: Some(pack_color(span.color)), ..plain }));
            at = end;
        }
    }
    if at < total {
        runs.push((at, total - at, plain));
    }
    Prose { text: text.to_owned(), runs, links: Vec::new(), chars: total, anchors: Vec::new() }
}

fn math_prose(text: &str) -> Prose {
    let mut prose = Prose::default();
    prose.push(text, InlineFormat { math: true, code: true, ..InlineFormat::default() });
    prose
}

/// The plain text of a document: no markup, blocks separated by blank lines, list items and table rows by single newlines.
pub(super) fn plain_text_of(blocks: &[Block]) -> String {
    let mut out = String::new();
    let mut previous_entry = false;
    let mut previous_title = false;
    for block in blocks {
        let (text, entry) = match block {
            Block::Paragraph { prose, .. } | Block::Heading { prose, .. } | Block::DefTerm { prose } | Block::DefDetail { prose } => {
                (prose.text.trim().to_owned(), matches!(block, Block::DefTerm { .. } | Block::DefDetail { .. }))
            }
            Block::ListItem { marker, prose, task, .. } => {
                let check = match task {
                    Some(TaskItem { checked: true, .. }) => "[x] ",
                    Some(_) => "[ ] ",
                    None => "",
                };
                (format!("{marker} {check}{}", prose.text), true)
            }
            Block::Footnote { number, prose, .. } => (format!("{number}. {}", prose.text.trim_end_matches(|c: char| c == '\u{21a9}' || c.is_whitespace())), true),
            Block::Code { text, .. } => (text.clone(), false),
            Block::Rule => continue,
            Block::Table { header, rows, .. } => {
                let line = |cells: &[Prose]| cells.iter().map(|cell| cell.text.as_str()).collect::<Vec<_>>().join("\t");
                let mut lines = vec![line(header)];
                lines.extend(rows.iter().map(|row| line(row)));
                (lines.join("\n"), false)
            }
            Block::Image { alt, .. } => (alt.clone(), false),
            Block::AlertTitle { kind, .. } => (kind.title().to_owned(), false),
            Block::Math { tex, .. } => (tex.clone(), false),
        };
        if !out.is_empty() {
            out.push_str(if (entry && previous_entry) || previous_title { "\n" } else { "\n\n" });
        }
        out.push_str(&text);
        previous_entry = entry;
        previous_title = matches!(block, Block::AlertTitle { .. });
    }
    out
}

/// Every link target in the document, in order, without repeats. Footnote jumps are not links.
pub(super) fn links_of(blocks: &[Block]) -> Vec<String> {
    fn from_prose<'a>(prose: &'a Prose, out: &mut Vec<&'a str>) {
        for (_, _, format) in &prose.runs {
            let target = format.link.and_then(|index| prose.links.get(index));
            if let Some(target) = target.filter(|t| !matches!(t.kind, LinkKind::Footnote | LinkKind::FootnoteBack)) {
                out.push(&target.url);
            }
        }
    }
    let mut all: Vec<&str> = Vec::new();
    for block in blocks {
        match block {
            Block::Paragraph { prose, .. }
            | Block::Heading { prose, .. }
            | Block::ListItem { prose, .. }
            | Block::DefTerm { prose }
            | Block::DefDetail { prose }
            | Block::Footnote { prose, .. } => from_prose(prose, &mut all),
            Block::Table { header, rows, .. } => header.iter().chain(rows.iter().flatten()).for_each(|cell| from_prose(cell, &mut all)),
            Block::Image { link: Some(link), .. } => all.push(link),
            _ => {}
        }
    }
    let mut seen = HashSet::new();
    all.into_iter().filter(|url| seen.insert(*url)).map(str::to_owned).collect()
}

/// Whether a block drawn at `position`/`size` is far enough from the visible `view` (a viewport above and below it) to be skipped.
pub(super) fn is_culled(view_position: Vec2, view_size: Vec2, position: Vec2, size: Vec2) -> bool {
    position.y + size.y < view_position.y - view_size.y || position.y > view_position.y + view_size.y * 2.0
}

/// How far (pixels, negative = up) a drag with the pointer at `pointer_y` scrolls a container spanning `top..top + height` this frame: nothing
/// until the pointer is within 40 pixels of an edge or beyond it, then a quarter of the distance in, at most 20.
pub(super) fn auto_scroll_step(top: f32, height: f32, pointer_y: f32) -> f32 {
    let edge = 40.0;
    let above = top + edge - pointer_y;
    let below = pointer_y - (top + height - edge);
    if above > 0.0 {
        -above.min(80.0) * 0.25
    } else if below > 0.0 {
        below.min(80.0) * 0.25
    } else {
        0.0
    }
}

/// The selected char range of cell `index` when a selection runs from `first` to `last` (each a (cell, char) boundary): the end of the first
/// cell, the whole of the cells between, the start of the last, nothing elsewhere. `chars` is the length of cell `index`.
pub(super) fn cell_selection(index: usize, chars: usize, first: (usize, usize), last: (usize, usize)) -> (usize, usize) {
    if index < first.0 || index > last.0 {
        (0, 0)
    } else if index == first.0 && index == last.0 {
        (first.1.min(chars), last.1.min(chars).max(first.1.min(chars)))
    } else if index == first.0 {
        (first.1.min(chars), chars)
    } else if index == last.0 {
        (0, last.1.min(chars))
    } else {
        (0, chars)
    }
}

// -- Drawing state -------------------------------------------------------------------------------------------------------------------

/// How one block of prose is drawn.
#[derive(Debug, Clone, Copy)]
pub(super) struct Look {
    /// Multiplies the font size (headings).
    scale: f32,
    /// The block's own font (spans without a font of their own use it).
    base_font: u16,
    /// A font every span is forced into (headings, table headers, code).
    force_font: Option<u16>,
    color: Option<Vec4>,
    /// Size the text block to its content instead of the column (aligned table cells).
    fit_width: bool,
}

#[derive(Clone, Copy)]
enum Source<'a> {
    Prose(&'a Prose),
    Code { language: &'a str, text: &'a str },
    Math(&'a str),
}

/// Everything about one drawn block of prose that does not change between frames with the same style.
struct Cell {
    desc: ElementDesc,
    text_style: TextInputStyle,
    spans: Vec<RichSpan>,
    /// Characters of text.
    chars: usize,
    /// A block of code that does not wrap: it scrolls sideways.
    scrolls_sideways: bool,
}

/// The link the pointer is over.
#[derive(Debug, Clone, PartialEq, Eq)]
struct HoverLink {
    cell: usize,
    /// Index into the prose's links.
    link: usize,
    url: String,
    title: String,
}

/// How a link under the pointer is drawn.
#[derive(Debug, Clone, Copy)]
struct Hover {
    link: usize,
    pressed: bool,
}

/// A drag that started inside a text block, which may end in another one.
#[derive(Debug, Clone, Copy)]
struct Drag {
    cell: usize,
    /// Caret position (a boundary between characters) in the cell where the press landed.
    at: usize,
}

/// A selection that spans several text blocks, in document order of their cells.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CrossSelection {
    first: (usize, usize),
    last: (usize, usize),
}

struct Pass<'a> {
    id: &'a str,
    style: &'a MarkdownStyle,
    res: &'a MarkdownResources,
    events: Vec<MarkdownEvent>,
    /// Index of the next prose cell of the block being drawn. Each block starts at its own base (`cell_bases`), so a block that an
    /// override skips does not shift the cells (and selection states) of the blocks after it.
    cell: usize,
    /// Index of the block being drawn.
    block: usize,
    /// The width text wraps to before its first layout.
    width_hint: f32,
}

/// How many prose cells (selectable text blocks) a block draws.
fn cells_in(block: &Block) -> usize {
    match block {
        Block::Paragraph { .. }
        | Block::Heading { .. }
        | Block::ListItem { .. }
        | Block::DefTerm { .. }
        | Block::DefDetail { .. }
        | Block::Footnote { .. }
        | Block::Code { .. }
        | Block::Math { .. } => 1,
        Block::Table { header, rows, .. } => header.len() + rows.iter().map(Vec::len).sum::<usize>(),
        Block::Rule | Block::Image { .. } | Block::AlertTitle { .. } => 0,
    }
}

/// The kind of a block, for [`BlockInfo`].
fn kind_of(block: &Block) -> BlockKind {
    match block {
        Block::Paragraph { .. } => BlockKind::Paragraph,
        Block::Heading { level, .. } => BlockKind::Heading(*level),
        Block::ListItem { .. } => BlockKind::ListItem,
        Block::Code { .. } => BlockKind::Code,
        Block::Rule => BlockKind::Rule,
        Block::Table { .. } => BlockKind::Table,
        Block::Image { .. } => BlockKind::Image,
        Block::AlertTitle { .. } => BlockKind::AlertTitle,
        Block::DefTerm { .. } => BlockKind::DefinitionTerm,
        Block::DefDetail { .. } => BlockKind::DefinitionDetail,
        Block::Math { .. } => BlockKind::Math,
        Block::Footnote { .. } => BlockKind::Footnote,
    }
}

/// `range` widened to whole lines of `source` (start of the first line to the end of the last, without its newline).
pub(super) fn expand_to_lines(source: &str, range: &Range<usize>) -> Range<usize> {
    let start = range.start.min(source.len());
    let end = range.end.clamp(start, source.len());
    let line_start = source.get(..start).and_then(|head| head.rfind('\n')).map_or(0, |at| at + 1);
    // A range that ends just after a newline belongs to the line before it.
    let trimmed_end = source.get(start..end).map_or(end, |body| start + body.trim_end_matches(['\n', '\r']).len()).max(start);
    let line_end = source.get(trimmed_end..).and_then(|tail| tail.find('\n')).map_or(source.len(), |at| trimmed_end + at);
    let line_end = if source.get(..line_end).is_some_and(|head| head.ends_with('\r')) { line_end - 1 } else { line_end };
    line_start..line_end.max(line_start)
}

/// Where in the source (byte offset) the `char_index`-th character of a block's plain text came from: walks both texts in step,
/// skipping source characters (markup) that the plain text does not have. A best guess for placing a caret where a rendered block was clicked.
pub(super) fn approx_source_offset(source: &str, plain: &str, char_index: usize) -> usize {
    let mut plain_chars = plain.chars().enumerate().peekable();
    let mut last = 0usize;
    for (offset, c) in source.char_indices() {
        match plain_chars.peek() {
            Some((index, wanted)) => {
                if c == *wanted {
                    if *index >= char_index {
                        return offset;
                    }
                    plain_chars.next();
                    last = offset + c.len_utf8();
                }
            }
            None => break,
        }
    }
    last.min(source.len())
}

type Content<'c> = &'c mut dyn FnMut(&mut MarkdownView, &mut Ui<'_>, &mut Pass<'_>);

/// A parsed Markdown document plus the per-block widget state that keeps selection working across frames.
pub struct MarkdownView {
    source: String,
    config: ParseConfig,
    parsed: Parsed,
    anchor_set: HashSet<String>,
    /// One text state per prose cell, created lazily in draw order.
    states: Vec<(TextEditState, ScrollAreaState)>,
    /// Layout-independent draw data per prose cell, valid for `cache_style`.
    cells: Vec<Option<Cell>>,
    /// First prose cell of each block.
    cell_bases: Vec<usize>,
    /// What an application can know about each block.
    infos: Vec<BlockInfo>,
    cache_style: Option<MarkdownStyle>,
    cache_dirty: bool,
    marker_widths: HashMap<usize, f32>,
    /// Per code block: the rows each line took when the gutter was built, and the gutter text.
    gutters: HashMap<usize, (Vec<u32>, String)>,
    anchor_cells: HashMap<String, usize>,
    visited: HashSet<String>,
    requested: HashSet<String>,
    checked: Vec<Option<bool>>,
    toggles: Vec<ToggleState>,
    buttons: HashMap<usize, ButtonState>,
    tooltips: HashMap<usize, TooltipState>,
    pending_scroll: Option<String>,
    hover: Option<HoverLink>,
    hover_previous: Option<HoverLink>,
    link_tip: Option<(String, TooltipState)>,
    drag: Option<Drag>,
    cross: Option<CrossSelection>,
    pointer_was_down: bool,
    /// Column width last frame, to notice a resize (heights of skipped blocks are stale then).
    last_width: Option<f32>,
    width_hint: f32,
}

impl MarkdownView {
    pub fn new(source: &str) -> Self {
        Self::with_config(source, ParseConfig::from_style(&MarkdownStyle::default()))
    }

    /// [`MarkdownView::new`] parsed for `style` from the start (smart punctuation, soft breaks, ...), so the first frame does not parse twice.
    pub fn with_style(source: &str, style: &MarkdownStyle) -> Self {
        Self::with_config(source, ParseConfig::from_style(style))
    }

    fn with_config(source: &str, config: ParseConfig) -> Self {
        let mut view = Self {
            source: String::new(),
            config,
            parsed: Parsed::default(),
            anchor_set: HashSet::new(),
            states: Vec::new(),
            cells: Vec::new(),
            cell_bases: Vec::new(),
            infos: Vec::new(),
            cache_style: None,
            cache_dirty: false,
            marker_widths: HashMap::new(),
            gutters: HashMap::new(),
            anchor_cells: HashMap::new(),
            visited: HashSet::new(),
            requested: HashSet::new(),
            checked: Vec::new(),
            toggles: Vec::new(),
            buttons: HashMap::new(),
            tooltips: HashMap::new(),
            pending_scroll: None,
            hover: None,
            hover_previous: None,
            link_tip: None,
            drag: None,
            cross: None,
            pointer_was_down: false,
            last_width: None,
            width_hint: 0.0,
        };
        view.set_source(source);
        view
    }

    /// Replaces the document (selection resets).
    pub fn set_source(&mut self, source: &str) {
        self.source = source.to_owned();
        self.reparse();
    }

    fn reparse(&mut self) {
        self.parsed = parse_with(&self.source, &self.config);
        self.anchor_set = anchors_of(&self.parsed.blocks);
        let mut base = 0usize;
        self.cell_bases = self
            .parsed
            .blocks
            .iter()
            .map(|block| {
                let at = base;
                base += cells_in(block);
                at
            })
            .collect();
        self.infos = self
            .parsed
            .blocks
            .iter()
            .enumerate()
            .map(|(index, block)| {
                let range = self.parsed.ranges.get(index).cloned().unwrap_or(0..0);
                let source = if range.is_empty() { 0..0 } else { expand_to_lines(&self.source, &range) };
                BlockInfo {
                    index,
                    kind: kind_of(block),
                    source,
                    text: plain_text_of(std::slice::from_ref(block)),
                    task: match block {
                        Block::ListItem { task, .. } => task.map(|t| t.index),
                        _ => None,
                    },
                }
            })
            .collect();
        self.states.clear();
        self.cells.clear();
        self.marker_widths.clear();
        self.gutters.clear();
        self.anchor_cells.clear();
        self.checked = vec![None; self.parsed.task_ranges.len()];
        self.pending_scroll = None;
    }

    /// The Markdown source.
    pub fn source(&self) -> &str {
        &self.source
    }

    /// The document without markup, for search indexes, notifications and accessibility: blocks separated by blank lines, list
    /// items and table rows by single newlines, code and math as written, images as their alt text, raw HTML omitted.
    pub fn plain_text(&self) -> String {
        plain_text_of(&self.parsed.blocks)
    }

    /// Every link target in the document in order, without repeats: URLs, `#anchors` and wiki-link targets. Image sources are not links
    /// (an image inside a link counts as that link).
    pub fn links(&self) -> Vec<String> {
        links_of(&self.parsed.blocks)
    }

    /// The headings with their ids (explicit `{#id}` or a GitHub-style slug), for a table of contents.
    pub fn headings(&self) -> Vec<MarkdownHeading> {
        self.parsed
            .blocks
            .iter()
            .filter_map(|block| match block {
                Block::Heading { level, prose, id, .. } => Some(MarkdownHeading { level: *level, text: prose.text.clone(), id: id.clone() }),
                _ => None,
            })
            .collect()
    }

    /// Where the `item`-th task list item's `[ ]`/`[x]` sits in the source, so an app can rewrite it after
    /// [`MarkdownEvent::TaskToggled`].
    pub fn task_source_range(&self, item: usize) -> Option<Range<usize>> {
        self.parsed.task_ranges.get(item).cloned()
    }

    /// Asks for the heading with this id (or footnote `fn:LABEL`) to scroll into view on the next [`MarkdownView::show_with`], inside
    /// [`MarkdownResources::scroll_container`]. `false` when the document has no such anchor.
    pub fn scroll_to(&mut self, anchor: &str) -> bool {
        let anchor = anchor.trim_start_matches('#');
        let known = self.anchor_set.contains(anchor);
        if known {
            self.pending_scroll = Some(anchor.to_owned());
        }
        known
    }

    /// Treats `url` as already visited (drawn in `link_visited`).
    pub fn mark_visited(&mut self, url: &str) {
        if self.visited.insert(url.to_owned()) {
            self.cache_dirty = true;
        }
    }

    pub fn clear_visited(&mut self) {
        if !self.visited.is_empty() {
            self.visited.clear();
            self.cache_dirty = true;
        }
    }

    /// Forgets which images were requested, so [`MarkdownEvent::ImageRequested`] fires again for the ones still missing (a retry).
    pub fn clear_image_requests(&mut self) {
        self.requested.clear();
    }

    /// Draws the document as a column filling the parent's width. `id` must be unique in the frame.
    pub fn show(&mut self, ui: &mut Ui<'_>, id: &str, style: &MarkdownStyle) -> Vec<MarkdownEvent> {
        self.show_with(ui, id, style, &MarkdownResources::default())
    }

    /// [`MarkdownView::show`] with the application's images and scroll container.
    pub fn show_with(&mut self, ui: &mut Ui<'_>, id: &str, style: &MarkdownStyle, resources: &MarkdownResources) -> Vec<MarkdownEvent> {
        self.show_blocks(ui, id, style, resources, &mut |_, _| false)
    }

    /// [`MarkdownView::show_with`] where the application can take over any block: `replace` is called for each block (with its
    /// [`BlockInfo`], in document order) before it is drawn, and returns `true` after drawing something in its place (an editor, a custom
    /// card, nothing at all) to skip the Markdown rendering of that block. This is the hook for editors that show a line's source only
    /// where the cursor is: see [`MarkdownEditor`](super::MarkdownEditor). The blocks around a replaced one keep their selection state.
    ///
    /// Set [`MarkdownResources::report_block_clicks`] to get [`MarkdownEvent::BlockClicked`].
    pub fn show_blocks(
        &mut self,
        ui: &mut Ui<'_>,
        id: &str,
        style: &MarkdownStyle,
        resources: &MarkdownResources,
        replace: &mut dyn FnMut(&mut Ui<'_>, &BlockInfo) -> bool,
    ) -> Vec<MarkdownEvent> {
        self.prepare(style);
        // A pending scroll waits until the block it targets exists (it may have been skipped as off-screen), and nothing is skipped meanwhile.
        let mut scrolling = false;
        if let (Some(anchor), Some(container)) = (self.pending_scroll.clone(), resources.scroll_container.as_deref()) {
            scrolling = true;
            if let Some(cell) = self.anchor_cells.get(&anchor)
                && ui.element_bounds(&format!("{id}::{cell}")).is_some()
            {
                ui.scroll_into_view(container, &format!("{id}::{cell}"));
                self.pending_scroll = None;
            }
        } else {
            self.pending_scroll = None;
        }
        let column_width = ui.element_bounds(id).map(|(_, size)| size.x);
        let resized = column_width.zip(self.last_width).is_none_or(|(now, before)| (now - before).abs() > 0.5);
        self.last_width = column_width;
        let viewport = resources
            .scroll_container
            .as_deref()
            .filter(|_| style.cull_offscreen && !scrolling && !resized)
            .and_then(|container| ui.element_bounds(container));
        let wrapping = resources.scroll_container.is_some() && style.cull_offscreen;
        let mut pass = Pass { id, style, res: resources, events: Vec::new(), cell: 0, block: 0, width_hint: column_width.or(resources.width_hint).unwrap_or(self.width_hint) };
        let _column = ui.element(
            &ElementDesc::new().id(id.to_owned()).width(Sizing::Grow).height(Sizing::Fit).direction(Direction::TopToBottom),
        );
        self.hover_previous = self.hover.take();
        let parsed = std::mem::take(&mut self.parsed);
        let infos = std::mem::take(&mut self.infos);
        for (index, block) in parsed.blocks.iter().enumerate() {
            if let Some(previous) = index.checked_sub(1).and_then(|i| parsed.blocks.get(i)) {
                self.spacer(ui, &mut pass, previous, block);
            }
            if infos.get(index).is_some_and(|info| replace(ui, info)) {
                continue;
            }
            // With a scroll container each block sits in an element of its own, so its height is known next frame and it can be skipped
            // (as empty space of that height) while far from the visible part.
            let block_id = format!("{id}::b{index}");
            if let (Some((view_position, view_size)), Some((position, size))) = (viewport, ui.element_bounds(&block_id))
                && is_culled(view_position, view_size, position, size)
            {
                drop(ui.element(&ElementDesc::new().id(block_id).width(Sizing::Grow).height(Sizing::Fixed(size.y))));
                continue;
            }
            let _wrapper = wrapping.then(|| {
                ui.element(&ElementDesc::new().id(block_id).width(Sizing::Grow).height(Sizing::Fit).direction(Direction::TopToBottom))
            });
            pass.block = index;
            pass.cell = self.cell_bases.get(index).copied().unwrap_or(0);
            self.block(ui, &mut pass, &parsed, index, block);
        }
        self.parsed = parsed;
        self.infos = infos;
        self.after_blocks(ui, &mut pass);
        pass.events
    }

    /// What the application can know about each block: kind, source range, plain text.
    pub fn blocks(&self) -> &[BlockInfo] {
        &self.infos
    }

    /// The block whose source lines contain the byte `offset` of the source (the smallest one when blocks nest, as a quote's paragraph
    /// does inside the quote).
    pub fn block_at_offset(&self, offset: usize) -> Option<usize> {
        self.infos
            .iter()
            .filter(|info| !info.source.is_empty() && info.source.start <= offset && offset <= info.source.end)
            .min_by_key(|info| info.source.len())
            .map(|info| info.index)
    }

    /// A best guess at the byte offset in the source of the `char_index`-th character of block `block`'s plain text (a position from
    /// [`MarkdownEvent::BlockClicked`]), for placing a caret where a rendered block was clicked.
    pub fn source_offset(&self, block: usize, char_index: usize) -> Option<usize> {
        let info = self.infos.get(block)?;
        let slice = self.source.get(info.source.clone())?;
        Some(info.source.start + approx_source_offset(slice, &info.text, char_index))
    }

    /// Re-parses when a parser switch changed and drops the cached draw data when the style changed.
    fn prepare(&mut self, style: &MarkdownStyle) {
        let config = ParseConfig::from_style(style);
        if config != self.config {
            self.config = config;
            self.reparse();
            self.cache_style = None;
        }
        if self.cache_dirty || self.cache_style.as_ref() != Some(style) {
            self.cells.clear();
            self.marker_widths.clear();
            self.gutters.clear();
            self.cache_style = Some(style.clone());
            self.cache_dirty = false;
        }
    }

    fn spacer(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, previous: &Block, next: &Block) {
        let gap = gap_between(previous, next, pass.style);
        if gap == 0 {
            return;
        }
        // Blocks in the same quote keep one unbroken bar through the gap between them.
        let before = quote_of(previous);
        let shared = before.iter().zip(quote_of(next)).take_while(|(a, b)| a == b).count();
        let shared = before.get(..shared).unwrap_or_default().to_vec();
        self.quoted(ui, pass, &shared, &mut |_, ui, _| {
            let _gap = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(f32::from(gap))));
        });
    }

    /// Wraps `content` in one bar-and-indent element per quote level.
    fn quoted(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, quote: &[Option<AlertKind>], content: Content<'_>) {
        let Some((first, rest)) = quote.split_first() else {
            content(self, ui, pass);
            return;
        };
        let color = first.map_or(pass.style.quote_bar, |kind| alert_colour(kind, &pass.style.alert_colours));
        // The bar is an element of its own, as tall as the row, not a border: a border drew hairlines along the other edges of every
        // quote wrapper.
        let _level = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).direction(Direction::LeftToRight));
        drop(ui.element(&ElementDesc::new().width(Sizing::Fixed(f32::from(pass.style.quote_bar_width))).height(Sizing::Grow).background(color)));
        let _inset = ui.element(
            &ElementDesc::new()
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .direction(Direction::TopToBottom)
                .padding(Padding { left: pass.style.quote_padding, ..Padding::default() }),
        );
        self.quoted(ui, pass, rest, content);
    }

    fn block(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, parsed: &Parsed, block_index: usize, block: &Block) {
        let style = pass.style;
        let regular = Look { scale: 1.0, base_font: style.regular_font, force_font: None, color: None, fit_width: false };
        match block {
            Block::Paragraph { prose, quote } => {
                let look = Look { base_font: if quote.is_empty() { style.regular_font } else { style.quote_font.unwrap_or(style.regular_font) }, ..regular };
                self.quoted(ui, pass, quote, &mut |this, ui, pass| this.prose(ui, pass, Source::Prose(prose), look));
            }
            Block::Heading { level, prose, .. } => {
                let scale = style.heading_scale.get(usize::from(level.saturating_sub(1)).min(5)).copied().unwrap_or(1.0);
                let look = Look { scale, force_font: Some(style.heading_font.unwrap_or(style.bold_font)), ..regular };
                self.prose(ui, pass, Source::Prose(prose), look);
            }
            Block::ListItem { depth, ordinal, list, prose, quote, task, .. } => {
                let (depth, ordinal, list, task) = (*depth, *ordinal, *list, *task);
                let look = Look { base_font: if quote.is_empty() { style.regular_font } else { style.quote_font.unwrap_or(style.regular_font) }, ..regular };
                self.quoted(ui, pass, quote, &mut |this, ui, pass| {
                    let _row = ui.element(
                        &ElementDesc::new()
                            .width(Sizing::Grow)
                            .height(Sizing::Fit)
                            .padding(Padding { left: pass.style.list_indent * u16::from(depth), ..Padding::default() })
                            .child_gap(pass.style.list_marker_gap)
                            .direction(Direction::LeftToRight),
                    );
                    let text = marker_text(ordinal, depth, pass.style);
                    this.marker(ui, pass, parsed, list, &text, depth);
                    if let Some(task) = task {
                        this.checkbox(ui, pass, block_index, task);
                    }
                    this.prose(ui, pass, Source::Prose(prose), look);
                });
            }
            Block::Footnote { number, prose, .. } => {
                let _row = ui.element(
                    &ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).child_gap(style.list_marker_gap).direction(Direction::LeftToRight),
                );
                self.marker_plain(ui, style, &format!("{number}."));
                self.prose(ui, pass, Source::Prose(prose), regular);
            }
            Block::DefTerm { prose } => {
                self.prose(ui, pass, Source::Prose(prose), Look { force_font: Some(style.bold_font), ..regular });
            }
            Block::DefDetail { prose } => {
                let _indent = ui.element(
                    &ElementDesc::new()
                        .width(Sizing::Grow)
                        .height(Sizing::Fit)
                        .padding(Padding { left: style.definition_indent, ..Padding::default() }),
                );
                self.prose(ui, pass, Source::Prose(prose), regular);
            }
            Block::AlertTitle { kind, quote } => {
                let kind = *kind;
                self.quoted(ui, pass, quote, &mut |_, ui, pass| {
                    let style = pass.style;
                    ui.text(
                        kind.title(),
                        &TextStyle {
                            color: alert_colour(kind, &style.alert_colours),
                            font_id: style.bold_font,
                            font_size: style.font_size,
                            ..TextStyle::default()
                        },
                    );
                });
            }
            Block::Code { language, text } => self.code(ui, pass, language, text),
            Block::Math { tex, render, expr } => match render {
                Some(MathRender::Image { texture, size }) => {
                    ui.image(&ElementDesc::new().width(Sizing::Fixed(size.x)).height(Sizing::Fixed(size.y)), *texture);
                }
                None if expr.is_some() => {
                    if let Some(expr) = expr {
                        let _centred = ui.element(
                            &ElementDesc::new()
                                .width(Sizing::Grow)
                                .height(Sizing::Fit)
                                .padding(Padding::symmetric(0, 4))
                                .align(ChildAlignment::new(AlignX::Center, AlignY::Center)),
                        );
                        let math = TexStyle {
                            font_size: style.font_size + 3,
                            color: style.math_colour.unwrap_or(style.text),
                            italic_font: style.italic_font,
                            roman_font: style.regular_font,
                            bold_font: style.bold_font,
                            mono_font: style.mono_font,
                            rule_thickness: style.rule_thickness.max(1.0),
                            script_scale: style.script_scale,
                        };
                        tex::draw(ui, expr, &math);
                    }
                }
                other => {
                    let text = match other {
                        Some(MathRender::Text(text)) => text.as_str(),
                        _ => tex.as_str(),
                    };
                    let _box = ui.element(
                        &ElementDesc::new()
                            .width(Sizing::Grow)
                            .height(Sizing::Fit)
                            .padding(Padding::all(style.code_padding))
                            .background(style.code_background)
                            .corner_radius(CornerRadius::all(style.code_corner_radius)),
                    );
                    let look = Look { force_font: Some(style.mono_font), color: style.math_colour, ..regular };
                    self.prose(ui, pass, Source::Math(text), look);
                }
            },
            Block::Rule => {
                let _rule = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(style.rule_thickness)).background(style.rule));
            }
            Block::Table { header, rows, aligns } => self.table(ui, pass, header, rows, aligns),
            Block::Image { url, alt, title, link, quote } => {
                self.image(ui, pass, url, alt, title, link.as_deref(), quote);
            }
        }
    }

    /// The bullet or number of a list item, right-aligned in a column as wide as the list's widest marker.
    fn marker(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, parsed: &Parsed, list: usize, text: &str, depth: u8) {
        let style = pass.style;
        let text_style = marker_text_style(style);
        let width = match self.marker_widths.get(&list) {
            Some(width) => *width,
            None => {
                let mut widest = ui.measure_text(&text_style, text).x;
                if let Some(info) = parsed.lists.get(list)
                    && let Some(start) = info.start
                {
                    for number in start..start.saturating_add(info.count.min(300)) {
                        widest = widest.max(ui.measure_text(&text_style, &marker_text(Some(number), depth, style)).x);
                    }
                }
                self.marker_widths.insert(list, widest);
                widest
            }
        };
        if width <= 0.0 {
            ui.text(text, &text_style);
            return;
        }
        let _column = ui.element(
            &ElementDesc::new()
                .width(Sizing::Fixed(width.ceil()))
                .height(Sizing::Fit)
                .align(ChildAlignment::new(AlignX::Right, AlignY::Top)),
        );
        ui.text(text, &text_style);
    }

    fn marker_plain(&mut self, ui: &mut Ui<'_>, style: &MarkdownStyle, text: &str) {
        ui.text(text, &marker_text_style(style));
    }

    fn checkbox(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, block_index: usize, task: TaskItem) {
        let style = pass.style;
        while self.toggles.len() <= task.index {
            self.toggles.push(ToggleState::new());
        }
        let checked = self.checked.get(task.index).copied().flatten().unwrap_or(task.checked);
        let size = style.task_box_size;
        let desc = ElementDesc::new().id(format!("{}::task{}", pass.id, task.index)).width(Sizing::Fixed(size)).height(Sizing::Fixed(size));
        let toggle_style = ToggleStyle { checked: style.task_accent, ..ToggleStyle::default() };
        let Some(state) = self.toggles.get_mut(task.index) else { return };
        let result = ui.checkbox(&desc, state, &toggle_style, checked, style.regular_font, !style.task_read_only);
        if result.clicked && !style.task_read_only {
            if let Some(slot) = self.checked.get_mut(task.index) {
                *slot = Some(!checked);
            }
            pass.events.push(MarkdownEvent::TaskToggled { block: block_index, item: task.index, checked: !checked });
        }
    }

    fn code(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, language: &str, text: &str) {
        let style = pass.style;
        let index = pass.block;
        let _code = ui.element(
            &ElementDesc::new()
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .padding(Padding::all(style.code_padding))
                .background(style.code_background)
                .corner_radius(CornerRadius::all(style.code_corner_radius))
                .direction(Direction::TopToBottom)
                .child_gap(6),
        );
        let label = style.code_language_label && !language.trim().is_empty();
        if label || style.code_copy_button {
            let _header = ui.element(
                &ElementDesc::new()
                    .width(Sizing::Grow)
                    .height(Sizing::Fit)
                    .direction(Direction::LeftToRight)
                    .child_gap(8)
                    .align(ChildAlignment::new(AlignX::Left, AlignY::Center)),
            );
            if label {
                ui.text(language.split(|c: char| c.is_whitespace() || c == ',').next().unwrap_or_default(), &TextStyle {
                    color: style.muted,
                    font_id: style.regular_font,
                    font_size: style.font_size.saturating_sub(2).max(8),
                    ..TextStyle::default()
                });
            }
            let _push = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(1.0)));
            drop(_push);
            if style.code_copy_button {
                {
                    let state = self.buttons.entry(index).or_default();
                    let desc = ElementDesc::new()
                        .id(format!("{}::copy{index}", pass.id))
                        .width(Sizing::Fit)
                        .height(Sizing::Fit)
                        .padding(Padding::symmetric(8, 2))
                        .cursor(Cursor::Pointer);
                    let (result, scope) = ui.button(&desc, state, &ButtonStyle { corner_radius: 4.0, ..ButtonStyle::default() }, true);
                    if scope.is_some() {
                        ui.text("Copy", &TextStyle { color: style.text, font_id: style.regular_font, font_size: style.font_size.saturating_sub(2).max(8), ..TextStyle::default() });
                    }
                    drop(scope);
                    if result.clicked {
                        pass.events.push(MarkdownEvent::CopyCode(text.to_owned()));
                    }
                }
            }
        }
        let mut body = ElementDesc::new().id(format!("{}::codebody{index}", pass.id)).width(Sizing::Grow).height(Sizing::Fit).direction(Direction::LeftToRight).child_gap(10);
        if let Some(max) = style.code_max_height.filter(|m| *m > 0.0) {
            body = body.height_max(max).clip(Clip { vertical: true, ..Clip::default() });
        }
        let _body = ui.element(&body);
        if style.code_line_numbers {
            // A line that wraps takes several rows: the number goes on the first and the rest stay blank, so the gutter stays in step.
            let rows: Vec<u32> = if style.code_wrap {
                self.states.get(pass.cell).map(|(state, _)| ui.rich_text_row_counts(state)).unwrap_or_default()
            } else {
                Vec::new()
            };
            let stale = self.gutters.get(&index).is_none_or(|(known, _)| *known != rows);
            if stale {
                let lines = text.split('\n').count();
                let numbers = (1..=lines)
                    .map(|n| {
                        let extra = rows.get(n - 1).copied().unwrap_or(1).saturating_sub(1) as usize;
                        format!("{n}{}", "\n".repeat(extra))
                    })
                    .collect::<Vec<_>>()
                    .join("\n");
                self.gutters.insert(index, (rows, numbers));
            }
            let Some((_, gutter)) = self.gutters.get(&index) else { return };
            let gutter_style = TextStyle {
                color: style.muted,
                font_id: style.mono_font,
                font_size: style.font_size,
                line_height: style.code_line_height,
                ..TextStyle::default()
            };
            let width = ui.measure_text(&gutter_style, &lines_widest(gutter)).x;
            let _gutter = ui.element(
                &ElementDesc::new().width(Sizing::Fixed(width.ceil())).height(Sizing::Fit).align(ChildAlignment::new(AlignX::Right, AlignY::Top)),
            );
            ui.text(gutter, &gutter_style);
        }
        let look = Look { scale: 1.0, base_font: style.regular_font, force_font: Some(style.mono_font), color: None, fit_width: false };
        // The text block lives in its own column so it takes the width left of the gutter.
        self.prose(ui, pass, Source::Code { language, text }, look);
    }

    fn table(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, header: &[Prose], rows: &[Vec<Prose>], aligns: &[ColumnAlign]) {
        let style = pass.style;
        let border = style.table_border.unwrap_or(style.rule);
        let _table = ui.element(
            &ElementDesc::new()
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .direction(Direction::TopToBottom)
                .border(Border { color: border, left: 1, right: 1, top: 1, bottom: 1, between_children: 1 }),
        );
        for (row_index, row) in std::iter::once(header).chain(rows.iter().map(Vec::as_slice)).enumerate() {
            let background = if row_index == 0 {
                style.table_header_background.unwrap_or(style.code_background)
            } else if row_index % 2 == 0 {
                style.table_zebra
            } else {
                Vec4::ZERO
            };
            let _row = ui.element(
                &ElementDesc::new()
                    .width(Sizing::Grow)
                    .height(Sizing::Fit)
                    .direction(Direction::LeftToRight)
                    .background(background)
                    .border(Border { color: border, between_children: 1, ..Border::default() }),
            );
            for (column, prose) in row.iter().enumerate() {
                let align = aligns.get(column).copied().unwrap_or_default();
                let x = match align {
                    ColumnAlign::Left => AlignX::Left,
                    ColumnAlign::Center => AlignX::Center,
                    ColumnAlign::Right => AlignX::Right,
                };
                let _cell = ui.element(
                    &ElementDesc::new()
                        .width(Sizing::Grow)
                        .height(Sizing::Fit)
                        .padding(Padding::symmetric(style.table_cell_padding_x, style.table_cell_padding_y))
                        .align(ChildAlignment::new(x, AlignY::Top)),
                );
                let look = if row_index == 0 {
                    Look { scale: 1.0, base_font: style.regular_font, force_font: Some(style.table_header_font.unwrap_or(style.bold_font)), color: None, fit_width: align != ColumnAlign::Left }
                } else {
                    Look { scale: 1.0, base_font: style.table_font.unwrap_or(style.regular_font), force_font: None, color: None, fit_width: align != ColumnAlign::Left }
                };
                self.prose(ui, pass, Source::Prose(prose), look);
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn image(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, url: &str, alt: &str, title: &str, link: Option<&str>, quote: &[Option<AlertKind>]) {
        let style = pass.style;
        let index = pass.block;
        let element_id = format!("{}::img{index}", pass.id);
        let ready = match pass.res.images.get(url) {
            Some(MarkdownImage::Ready { texture, size }) => Some((*texture, *size)),
            Some(MarkdownImage::Failed) => None,
            None => {
                if self.requested.insert(url.to_owned()) {
                    pass.events.push(MarkdownEvent::ImageRequested { url: url.to_owned(), id: element_id.clone() });
                }
                None
            }
        };
        let column_width = ui.element_bounds(pass.id).map(|(_, size)| size.x);
        let owned_link = link.map(str::to_owned);
        let (url_owned, title_owned, alt_owned) = (url.to_owned(), title.to_owned(), alt.to_owned());
        self.tooltips.entry(index).or_default();
        self.quoted(ui, pass, quote, &mut |this, ui, pass| {
            let style = pass.style;
            match ready {
                Some((texture, size)) if fit_image(size, column_width, style.image_max_height, style.image_upscale) != Vec2::ZERO => {
                    let shown = fit_image(size, column_width, style.image_max_height, style.image_upscale);
                    ui.image(
                        &ElementDesc::new().id(element_id.clone()).width(Sizing::Fixed(shown.x)).height(Sizing::Fixed(shown.y)).cursor(Cursor::Pointer),
                        texture,
                    );
                    if ui.clicked(&element_id) {
                        pass.events.push(match &owned_link {
                            Some(link) => MarkdownEvent::LinkClicked(link.clone()),
                            None => MarkdownEvent::ImageClicked(url_owned.clone()),
                        });
                    }
                    if !title_owned.is_empty()
                        && let Some(state) = this.tooltips.get_mut(&index)
                    {
                        text_tooltip(ui, &element_id, &title_owned, state, &TooltipStyle::default(), pass.res.delta_seconds);
                    }
                }
                _ => {
                    let shown = if alt_owned.is_empty() { url_owned.as_str() } else { alt_owned.as_str() };
                    ui.text(shown, &TextStyle { color: style.muted, font_id: style.italic_font, font_size: style.font_size, ..TextStyle::default() });
                }
            }
        });
        let _ = style;
    }

    /// Draws one block of selectable prose and reports link clicks and hovers.
    fn prose(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>, source: Source<'_>, look: Look) {
        let index = pass.cell;
        pass.cell += 1;
        while self.states.len() <= index {
            self.states.push((TextEditState::new(), ScrollAreaState::new()));
        }
        if self.cells.len() <= index {
            self.cells.resize_with(index + 1, || None);
        }
        if self.cells.get(index).is_some_and(Option::is_none) {
            let owned;
            let prose = match source {
                Source::Prose(prose) => prose,
                Source::Code { language, text } => {
                    owned = code_prose(language, text, pass.style);
                    &owned
                }
                Source::Math(text) => {
                    owned = math_prose(text);
                    &owned
                }
            };
            if let Some((state, _)) = self.states.get_mut(index)
                && state.text() != prose.text
            {
                state.set_text(&prose.text);
            }
            for anchor in &prose.anchors {
                self.anchor_cells.insert(anchor.clone(), index);
            }
            let is_code = matches!(source, Source::Code { .. });
            let cell = build_cell(pass.id, index, prose, look, pass.style, &self.visited, is_code);
            if let Some(slot) = self.cells.get_mut(index) {
                *slot = Some(cell);
            }
        }
        // The link under the pointer last frame decides how this frame draws it (colour, underline, cursor).
        let hover = self
            .hover_previous
            .as_ref()
            .filter(|h| h.cell == index)
            .map(|h| Hover { link: h.link, pressed: ui.pointer_down() });
        let (Some(Some(cell)), Some((state, scroll))) = (self.cells.get(index), self.states.get_mut(index)) else { return };
        let hovered_spans;
        let hovered_desc;
        let (desc, spans) = match (hover, source) {
            (Some(hover), Source::Prose(prose)) => {
                hovered_spans = spans_for(prose, look, pass.style, &self.visited, Some(hover));
                hovered_desc = cell.desc.clone().cursor(Cursor::Pointer);
                (&hovered_desc, &hovered_spans)
            }
            _ => (&cell.desc, &cell.spans),
        };
        let scrollbar = if cell.scrolls_sideways {
            ScrollbarStyle::default()
        } else {
            ScrollbarStyle { visibility: ScrollbarVisibility::AlwaysHidden, ..ScrollbarStyle::default() }
        };
        let text_style = TextInputStyle { wrap_width_hint: pass.width_hint, ..cell.text_style };
        let result = ui.rich_text_wrapping(desc, &text_style, &scrollbar, state, scroll, spans);
        if let (Some(char_index), Source::Prose(prose)) = (result.hovered_char, source)
            && let Some(link) = prose.link_index_at(char_index)
            && let Some(target) = prose.links.get(link)
        {
            self.hover = Some(HoverLink { cell: index, link, url: target.url.clone(), title: target.title.clone() });
        }
        let Some(char_index) = result.clicked_char else { return };
        if pass.res.report_block_clicks {
            pass.events.push(MarkdownEvent::BlockClicked { block: pass.block, char_index });
        }
        let target = match source {
            Source::Prose(prose) => prose.link_target_at(char_index).cloned(),
            _ => None,
        };
        if let Some(target) = target {
            self.link_clicked(&target, pass);
        }
    }

    /// What happens after every block is drawn: link hover events, right-click, link tooltips and selection across blocks.
    fn after_blocks(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>) {
        if self.hover != self.hover_previous {
            pass.events.push(MarkdownEvent::LinkHovered(self.hover.as_ref().map(|h| h.url.clone())));
        }
        if let Some(hover) = &self.hover {
            if ui.engine().input().mouse_just_pressed(MouseButton::Right) {
                let at = ui.pointer_position();
                pass.events.push(MarkdownEvent::LinkContext { url: hover.url.clone(), x: at.x.round() as i32, y: at.y.round() as i32 });
            }
            if pass.style.link_tooltips && !hover.title.is_empty() {
                let key = format!("{}:{}", hover.cell, hover.link);
                if self.link_tip.as_ref().is_none_or(|(known, _)| *known != key) {
                    self.link_tip = Some((key, TooltipState::default()));
                }
                if let Some((_, tip)) = self.link_tip.as_mut() {
                    text_tooltip(ui, &format!("{}::{}", pass.id, hover.cell), &hover.title, tip, &TooltipStyle::default(), pass.res.delta_seconds);
                }
            } else {
                self.link_tip = None;
            }
        } else {
            self.link_tip = None;
        }
        self.cross_selection(ui, pass);
    }

    /// The text cell under `at` (or, outside every cell, the nearest one vertically) with its drawn bounds.
    fn cell_near(&self, ui: &Ui<'_>, at: Vec2) -> Option<(usize, bool)> {
        let mut best: Option<(usize, f32)> = None;
        for (index, cell) in self.cells.iter().enumerate() {
            let Some(cell) = cell else { continue };
            let Some((position, size)) = ui.element_bounds(&cell.desc.id) else { continue };
            if at.x >= position.x && at.x <= position.x + size.x && at.y >= position.y && at.y <= position.y + size.y {
                return Some((index, true));
            }
            let distance = if at.y < position.y { position.y - at.y } else { (at.y - position.y - size.y).max(0.0) };
            if best.is_none_or(|(_, nearest)| distance < nearest) {
                best = Some((index, distance));
            }
        }
        best.map(|(index, _)| (index, false))
    }

    fn boundary_in(&self, ui: &Ui<'_>, cell: usize, at: Vec2) -> Option<usize> {
        let (Some(Some(drawn)), Some((state, _))) = (self.cells.get(cell), self.states.get(cell)) else { return None };
        ui.rich_text_char_at(&drawn.desc.id, &drawn.text_style, state, &drawn.spans, at, true)
    }

    /// One drag that starts in a text block and ends in another selects everything between: the end of the first, whole blocks in
    /// between, the start of the last. Each block keeps its own selection state, so this sets those states, and
    /// [`MarkdownView::selected_text`] reads them back.
    fn cross_selection(&mut self, ui: &mut Ui<'_>, pass: &mut Pass<'_>) {
        let pointer = ui.pointer_state();
        // The press is the first frame the pointer is down (the engine's own `pressed` flag is not reliably visible here).
        let pressed = pointer.down && !self.pointer_was_down;
        self.pointer_was_down = pointer.down;
        let press_at = pointer.press_position.unwrap_or(pointer.position);
        if pressed {
            self.cross = None;
            self.drag = match self.cell_near(ui, press_at) {
                Some((cell, true)) => self.boundary_in(ui, cell, press_at).map(|at| Drag { cell, at }),
                _ => None,
            };
            for (index, (state, _)) in self.states.iter_mut().enumerate() {
                if self.drag.is_none_or(|drag| drag.cell != index) {
                    state.select_range(0, 0);
                }
            }
        } else if pointer.down {
            if let Some(drag) = self.drag
                && let Some((head, _)) = self.cell_near(ui, pointer.position)
            {
                if head == drag.cell {
                    if self.cross.take().is_some() {
                        for (index, (state, _)) in self.states.iter_mut().enumerate() {
                            if index != drag.cell {
                                state.select_range(0, 0);
                            }
                        }
                    }
                } else if let Some(at) = self.boundary_in(ui, head, pointer.position) {
                    let (first, last) = if drag.cell < head { ((drag.cell, drag.at), (head, at)) } else { ((head, at), (drag.cell, drag.at)) };
                    self.cross = Some(CrossSelection { first, last });
                    self.apply_cross_selection();
                }
            }
        } else {
            self.drag = None;
        }
        // Dragging near the top or bottom of the scroll container scrolls it, so a selection can reach beyond what is on screen.
        if pointer.down
            && self.drag.is_some()
            && let Some(container) = pass.res.scroll_container.as_deref()
            && let Some((position, size)) = ui.element_bounds(container)
            && let Some(metrics) = ui.scroll_metrics(container)
        {
            let step = auto_scroll_step(position.y, size.y, pointer.position.y);
            if step != 0.0 {
                ui.set_scroll_offset(container, metrics.offset + Vec2::new(0.0, step));
                ui.engine().request_redraw_after(std::time::Duration::from_millis(16));
            }
        }
        if let Some(text) = self.cross.map(|_| self.selected_text()).filter(|text| !text.is_empty())
            && Shortcut::ctrl(Key::C).triggered(&ui.engine())
        {
            pass.events.push(MarkdownEvent::CopySelection(text));
        }
    }

    fn apply_cross_selection(&mut self) {
        let Some(cross) = self.cross else { return };
        for index in 0..self.states.len() {
            let chars = self.cells.get(index).and_then(Option::as_ref).map_or(0, |cell| cell.chars);
            let Some((state, _)) = self.states.get_mut(index) else { continue };
            let (from, to) = cell_selection(index, chars, cross.first, cross.last);
            state.select_range(from, to);
        }
    }

    /// The selected text of the whole view: what the reader selected inside one block, or across several (blocks separated by a
    /// blank line, table cells and list items by a newline). Empty when nothing is selected.
    pub fn selected_text(&self) -> String {
        let block_of = |cell: usize| self.cell_bases.partition_point(|base| *base <= cell).saturating_sub(1);
        let mut out = String::new();
        let mut previous: Option<usize> = None;
        for (index, (state, _)) in self.states.iter().enumerate() {
            if !state.has_selection() {
                continue;
            }
            let piece = state.selected_text();
            if piece.is_empty() {
                continue;
            }
            if let Some(previous) = previous {
                out.push_str(if block_of(previous) == block_of(index) { "\n" } else { "\n\n" });
            }
            out.push_str(&piece);
            previous = Some(index);
        }
        out
    }

    /// The width the view's column had last frame (`None` before the first layout): [`MarkdownViews`](super::MarkdownViews) passes it to
    /// views it creates so their first frame wraps right.
    pub fn last_width(&self) -> Option<f32> {
        self.last_width
    }

    /// The width to wrap to before this view has been laid out.
    pub fn set_width_hint(&mut self, width: f32) {
        self.width_hint = width;
    }

    /// Clears the selection in every block.
    pub fn clear_selection(&mut self) {
        self.cross = None;
        self.drag = None;
        for (state, _) in &mut self.states {
            state.select_range(0, 0);
        }
    }

    fn link_clicked(&mut self, target: &LinkTarget, pass: &mut Pass<'_>) {
        match target.kind {
            LinkKind::External => {
                pass.events.push(MarkdownEvent::LinkClicked(target.url.clone()));
                if self.visited.insert(target.url.clone()) && pass.style.link_visited.is_some() {
                    self.cache_dirty = true;
                }
            }
            LinkKind::Anchor => {
                pass.events.push(MarkdownEvent::LinkClicked(target.url.clone()));
                let name = target.url.trim_start_matches('#');
                if self.anchor_set.contains(name) {
                    pass.events.push(MarkdownEvent::AnchorClicked(name.to_owned()));
                    self.pending_scroll = Some(name.to_owned());
                }
            }
            LinkKind::Wiki => pass.events.push(MarkdownEvent::WikiLinkClicked(target.url.clone())),
            LinkKind::Footnote | LinkKind::FootnoteBack => {
                pass.events.push(MarkdownEvent::AnchorClicked(target.url.clone()));
                self.pending_scroll = Some(target.url.clone());
            }
        }
    }
}

fn lines_widest(gutter: &str) -> String {
    gutter.lines().max_by_key(|line| line.len()).unwrap_or_default().to_owned()
}

fn marker_text_style(style: &MarkdownStyle) -> TextStyle {
    TextStyle {
        color: style.muted,
        font_id: style.list_marker_font.unwrap_or(style.regular_font),
        font_size: style.font_size,
        ..TextStyle::default()
    }
}

/// Joins neighbouring spans that look the same. The engine only word-wraps a text block with a single styled run, so every needless
/// boundary (two links in a row, a link index that differs but draws alike) would stop a paragraph from wrapping.
pub(super) fn merge_spans(spans: Vec<RichSpan>) -> Vec<RichSpan> {
    let mut merged: Vec<RichSpan> = Vec::with_capacity(spans.len());
    for span in spans {
        match merged.last_mut() {
            Some(last) if last.start + last.len == span.start && (RichSpan { start: span.start, len: span.len, ..*last }) == span => {
                last.len += span.len;
            }
            _ => merged.push(span),
        }
    }
    merged
}

/// The spans of `prose` for `style`; `hover` is the link under the pointer, if it is in this prose.
fn spans_for(prose: &Prose, look: Look, style: &MarkdownStyle, visited: &HashSet<String>, hover: Option<Hover>) -> Vec<RichSpan> {
    let spans = prose
        .runs
        .iter()
        .map(|(start, len, format)| {
            let target = format.link.and_then(|link| prose.links.get(link));
            let hovered = hover.is_some_and(|h| format.link == Some(h.link));
            let font = look.force_font.or_else(|| {
                if target.is_some() && style.link_font.is_some() {
                    return style.link_font;
                }
                if format.code || format.math {
                    return Some(style.mono_font);
                }
                match (format.bold, format.italic) {
                    (true, true) => Some(style.bold_italic_font),
                    (true, false) => Some(style.bold_font),
                    (false, true) => Some(style.italic_font),
                    _ => None,
                }
            });
            let color = match target {
                Some(target) => {
                    let seen = visited.contains(&target.url);
                    let normal = style.link_visited.filter(|_| seen && target.kind == LinkKind::External).unwrap_or(style.link);
                    Some(match hover {
                        Some(h) if hovered && h.pressed => style.link_pressed.or(style.link_hover).unwrap_or(normal),
                        Some(_) if hovered => style.link_hover.unwrap_or(normal),
                        _ => normal,
                    })
                }
                None if format.math => style.math_colour.or(format.color.map(unpack_color)),
                None => format.color.map(unpack_color),
            };
            let is_link = target.is_some_and(|t| matches!(t.kind, LinkKind::External | LinkKind::Anchor | LinkKind::Wiki));
            let underlined_link = is_link
                && match style.link_underline {
                    LinkUnderline::Always => true,
                    LinkUnderline::Never => false,
                    LinkUnderline::OnHover => hovered,
                };
            RichSpan {
                start: *start,
                len: *len,
                color,
                font_id: font,
                // The block's font size is already scaled for headings; scaling the spans too would square it. Only scripts scale.
                size_scale: if format.script == Script::None { 1.0 } else { style.script_scale },
                underline: underlined_link || format.underline,
                strikethrough: format.strikethrough,
                baseline_shift: match format.script {
                    Script::None => 0.0,
                    Script::Sup => style.superscript_shift,
                    Script::Sub => style.subscript_shift,
                },
                line_style: if is_link { style.link_underline_style } else { LineStyle::Solid },
                line_thickness: if is_link { style.link_underline_thickness } else { 0.0 },
                line_color: if is_link { style.link_underline_color } else { None },
                hidden: false,
            }
        })
        .collect::<Vec<RichSpan>>();
    merge_spans(spans)
}

/// The spans and text style of one prose block for `style`.
fn build_cell(id: &str, index: usize, prose: &Prose, look: Look, style: &MarkdownStyle, visited: &HashSet<String>, is_code: bool) -> Cell {
    let spans = spans_for(prose, look, style, visited, None);
    let wraps = !is_code || style.code_wrap;
    let text_style = TextInputStyle {
        idle: Vec4::ZERO,
        hovered: Vec4::ZERO,
        focused: Vec4::ZERO,
        disabled: Vec4::ZERO,
        border_idle: Vec4::ZERO,
        border_focused: Vec4::ZERO,
        text_color: look.color.unwrap_or(style.text),
        caret_color: Vec4::ZERO,
        corner_radius: 0.0,
        font_id: look.base_font,
        font_size: (f32::from(style.font_size) * look.scale.max(1.0)).round() as u16,
        wrap_lines: wraps,
        letter_spacing: style.letter_spacing,
        // A heading's line is as much taller than a paragraph's as its text is.
        line_height: if is_code { style.code_line_height } else { (f32::from(style.line_height) * look.scale.max(1.0)).round() as u16 },
        read_only: true,
        ..TextInputStyle::default()
    };
    let width = if look.fit_width { Sizing::Fit } else { Sizing::Grow };
    Cell {
        desc: ElementDesc::new().id(format!("{id}::{index}")).width(width).height(Sizing::Fit),
        text_style,
        spans,
        chars: prose.chars,
        scrolls_sideways: !wraps,
    }
}

/// The spans of `prose` drawn with `style` in the default text look: what [`inline_text`](super::inline_text) hands to other widgets.
pub(super) fn spans_of(prose: &Prose, style: &MarkdownStyle) -> Vec<RichSpan> {
    let look = Look { scale: 1.0, base_font: style.regular_font, force_font: None, color: None, fit_width: false };
    build_cell("inline", 0, prose, look, style, &HashSet::new(), false).spans
}

#[cfg(test)]
pub(super) fn test_look(base_font: u16) -> Look {
    Look { scale: 1.0, base_font, force_font: None, color: None, fit_width: false }
}

#[cfg(test)]
pub(super) fn test_spans(prose: &Prose, look: Look, style: &MarkdownStyle, visited: &HashSet<String>) -> Vec<RichSpan> {
    build_cell("t", 0, prose, look, style, visited, false).spans
}

#[cfg(test)]
pub(super) fn test_hover_spans(prose: &Prose, style: &MarkdownStyle, link: usize, pressed: bool) -> Vec<RichSpan> {
    let look = Look { scale: 1.0, base_font: style.regular_font, force_font: None, color: None, fit_width: false };
    spans_for(prose, look, style, &HashSet::new(), Some(Hover { link, pressed }))
}

#[cfg(test)]
pub(super) fn test_cell_style(prose: &Prose, style: &MarkdownStyle, scale: f32, is_code: bool) -> TextInputStyle {
    let look = Look { scale, base_font: style.regular_font, force_font: None, color: None, fit_width: false };
    build_cell("t", 0, prose, look, style, &HashSet::new(), is_code).text_style
}

#[cfg(test)]
impl MarkdownView {
    /// Gives every prose cell its text and selects `ranges[cell]` (char ranges), as a drag would.
    pub(super) fn test_select(&mut self, ranges: &[(usize, usize)]) {
        let texts: Vec<String> = self
            .parsed
            .blocks
            .iter()
            .flat_map(|block| match block {
                Block::Paragraph { prose, .. } | Block::Heading { prose, .. } | Block::ListItem { prose, .. } => vec![prose.text.clone()],
                Block::Table { header, rows, .. } => header.iter().chain(rows.iter().flatten()).map(|c| c.text.clone()).collect(),
                _ => Vec::new(),
            })
            .collect();
        for (index, text) in texts.iter().enumerate() {
            let mut state = TextEditState::new();
            state.set_text(text);
            let (from, to) = ranges.get(index).copied().unwrap_or((0, 0));
            state.select_range(from, to);
            self.states.push((state, ScrollAreaState::new()));
        }
    }
}
