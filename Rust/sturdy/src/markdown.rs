//! Markdown rendered with the engine's UI: changelogs, mod descriptions, README-style text.
//!
//! [`MarkdownView`] parses once (CommonMark plus tables, strikethrough and task lists) and draws the document each
//! frame as a column of blocks. Every block of prose is a [`Ui::rich_text`] block, so text wraps with the layout, is
//! selectable and copyable, and links report clicks:
//!
//! ```ignore
//! // once, or whenever the text changes:
//! let mut view = MarkdownView::new(&changelog);
//! // each frame, inside the UI hook:
//! for event in view.show(ui, "changelog", &style) {
//!     if let MarkdownEvent::LinkClicked(url) = event { open_in_browser(&url); }
//! }
//! ```
//!
//! Bold, italic and monospace runs use the font ids in [`MarkdownStyle`]; register those faces (for example with
//! [`crate::Engine::register_ui_font_stack`]) or leave them at 0 to draw everything in the regular face. Images
//! are shown as their alt text and HTML is dropped. Code blocks are monospace without syntax colouring.

use glam::Vec4;
use pulldown_cmark::{CodeBlockKind, Event, HeadingLevel, Options, Parser, Tag, TagEnd};

use crate::ui::{CornerRadius, Direction, ElementDesc, Padding, Sizing, Ui};
use crate::widgets::{RichSpan, ScrollAreaState, ScrollbarStyle, ScrollbarVisibility, TextEditState, TextInputStyle};

/// One coloured range of a code block (character offsets into the block's text).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CodeSpan {
    pub start: usize,
    pub len: usize,
    pub color: Vec4,
}

/// Colours the code in fenced code blocks: called with the fence's language (may be empty) and the code, returns the
/// coloured ranges. The engine ships no highlighter; plug in `syntect` (or anything) here:
///
/// ```ignore
/// style.code_colorizer = Some(CodeColorizer::new(|language, code| my_syntect_spans(language, code)));
/// ```
#[derive(Clone)]
pub struct CodeColorizer(std::sync::Arc<dyn Fn(&str, &str) -> Vec<CodeSpan> + Send + Sync>);

impl CodeColorizer {
    pub fn new(colorize: impl Fn(&str, &str) -> Vec<CodeSpan> + Send + Sync + 'static) -> Self {
        Self(std::sync::Arc::new(colorize))
    }
}

impl std::fmt::Debug for CodeColorizer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("CodeColorizer")
    }
}

impl PartialEq for CodeColorizer {
    fn eq(&self, other: &Self) -> bool {
        std::sync::Arc::ptr_eq(&self.0, &other.0)
    }
}

/// Fonts and colours of a [`MarkdownView`].
#[derive(Debug, Clone, PartialEq)]
pub struct MarkdownStyle {
    /// Colours fenced code blocks; `None` draws them in the plain text colour.
    pub code_colorizer: Option<CodeColorizer>,
    pub text: Vec4,
    pub muted: Vec4,
    pub link: Vec4,
    pub code_background: Vec4,
    pub quote_bar: Vec4,
    pub rule: Vec4,
    pub font_size: u16,
    pub regular_font: u16,
    pub bold_font: u16,
    pub italic_font: u16,
    pub bold_italic_font: u16,
    pub mono_font: u16,
    /// Size multipliers for heading levels 1 to 6.
    pub heading_scale: [f32; 6],
    pub block_gap: u16,
    /// Indent per list level, logical pixels.
    pub list_indent: u16,
}

impl Default for MarkdownStyle {
    fn default() -> Self {
        Self {
            text: Vec4::new(0.92, 0.93, 0.95, 1.0),
            muted: Vec4::new(0.62, 0.64, 0.68, 1.0),
            link: Vec4::new(0.45, 0.65, 1.0, 1.0),
            code_background: Vec4::new(1.0, 1.0, 1.0, 0.06),
            quote_bar: Vec4::new(0.35, 0.55, 0.85, 0.7),
            rule: Vec4::new(1.0, 1.0, 1.0, 0.12),
            font_size: 15,
            regular_font: 0,
            bold_font: 0,
            italic_font: 0,
            bold_italic_font: 0,
            mono_font: 0,
            heading_scale: [1.8, 1.5, 1.3, 1.15, 1.05, 1.0],
            block_gap: 10,
            list_indent: 20,
            code_colorizer: None,
        }
    }
}

/// Something the reader did in a [`MarkdownView`] this frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MarkdownEvent {
    /// A link was clicked; open it yourself.
    LinkClicked(String),
}

/// Inline formatting active over a range of a run.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct InlineFormat {
    /// Packed 0xRRGGBBAA colour override (code colouring).
    color: Option<u32>,
    bold: bool,
    italic: bool,
    code: bool,
    strikethrough: bool,
    link: Option<usize>,
}

/// One block of prose: its plain text, styled ranges, and link targets.
#[derive(Debug, Clone, Default, PartialEq)]
struct Prose {
    text: String,
    /// (start char, char length, format) in order.
    runs: Vec<(usize, usize, InlineFormat)>,
    links: Vec<String>,
    chars: usize,
}

impl Prose {
    fn push(&mut self, text: &str, format: InlineFormat) {
        let count = text.chars().count();
        if count == 0 {
            return;
        }
        self.text.push_str(text);
        self.runs.push((self.chars, count, format));
        self.chars += count;
    }

    fn is_empty(&self) -> bool {
        self.text.trim().is_empty()
    }

    fn link_at(&self, char_index: usize) -> Option<&str> {
        self.runs
            .iter()
            .find(|(start, len, _)| char_index >= *start && char_index < start + len)
            .and_then(|(_, _, format)| format.link)
            .and_then(|link| self.links.get(link))
            .map(String::as_str)
    }
}

#[derive(Debug, Clone, PartialEq)]
enum Block {
    Paragraph { prose: Prose, quote_depth: u8 },
    Heading { level: u8, prose: Prose },
    ListItem { depth: u8, marker: String, prose: Prose, quote_depth: u8 },
    Code { language: String, text: String },
    Rule,
    Table { header: Vec<Prose>, rows: Vec<Vec<Prose>> },
}

/// A parsed Markdown document plus the per-block widget state that keeps selection working across frames.
pub struct MarkdownView {
    blocks: Vec<Block>,
    /// One text state per prose cell, created lazily in draw order.
    states: Vec<(TextEditState, ScrollAreaState)>,
}

impl MarkdownView {
    pub fn new(source: &str) -> Self {
        Self { blocks: parse(source), states: Vec::new() }
    }

    /// Replaces the document (selection resets).
    pub fn set_source(&mut self, source: &str) {
        self.blocks = parse(source);
        self.states.clear();
    }

    /// Draws the document as a column filling the parent's width. `id` must be unique in the frame.
    pub fn show(&mut self, ui: &mut Ui<'_>, id: &str, style: &MarkdownStyle) -> Vec<MarkdownEvent> {
        let mut events = Vec::new();
        let _column = ui.element(
            &ElementDesc::new()
                .id(id.to_owned())
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .direction(Direction::TopToBottom)
                .child_gap(style.block_gap),
        );
        let mut cell = 0usize;
        let blocks = std::mem::take(&mut self.blocks);
        for block in &blocks {
            match block {
                Block::Paragraph { prose, quote_depth } => {
                    self.quoted(ui, *quote_depth, style, |this, ui| {
                        this.prose(ui, id, &mut cell, prose, style, 1.0, None, &mut events);
                    });
                }
                Block::Heading { level, prose } => {
                    let scale = style.heading_scale[usize::from(level.saturating_sub(1)).min(5)];
                    self.prose(ui, id, &mut cell, prose, style, scale, Some(style.bold_font), &mut events);
                }
                Block::ListItem { depth, marker, prose, quote_depth } => {
                    self.quoted(ui, *quote_depth, style, |this, ui| {
                        let _row = ui.element(
                            &ElementDesc::new()
                                .width(Sizing::Grow)
                                .height(Sizing::Fit)
                                .padding(Padding { left: style.list_indent * u16::from(*depth), ..Padding::default() })
                                .child_gap(6)
                                .direction(Direction::LeftToRight),
                        );
                        ui.text(
                            marker,
                            &crate::ui::TextStyle {
                                color: style.muted,
                                font_id: style.regular_font,
                                font_size: style.font_size,
                                ..crate::ui::TextStyle::default()
                            },
                        );
                        this.prose(ui, id, &mut cell, prose, style, 1.0, None, &mut events);
                    });
                }
                Block::Code { language, text } => {
                    let _code = ui.element(
                        &ElementDesc::new()
                            .width(Sizing::Grow)
                            .height(Sizing::Fit)
                            .padding(Padding::all(10))
                            .background(style.code_background)
                            .corner_radius(CornerRadius::all(6.0)),
                    );
                    let total = text.chars().count();
                    let plain = InlineFormat { code: true, ..InlineFormat::default() };
                    let mut runs = Vec::new();
                    match &style.code_colorizer {
                        Some(colorizer) => {
                            // Fill the gaps between coloured ranges with plain runs so every char is covered.
                            let mut spans = (colorizer.0)(language, text);
                            spans.sort_by_key(|span| span.start);
                            let mut at = 0usize;
                            for span in spans {
                                let start = span.start.max(at).min(total);
                                let end = (span.start + span.len).min(total);
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
                        }
                        None => runs.push((0, total, plain)),
                    }
                    let prose = Prose { text: text.clone(), runs, links: Vec::new(), chars: total };
                    self.prose(ui, id, &mut cell, &prose, style, 1.0, Some(style.mono_font), &mut events);
                }
                Block::Rule => {
                    let _rule = ui.element(
                        &ElementDesc::new().width(Sizing::Grow).height(Sizing::Fixed(1.0)).background(style.rule),
                    );
                }
                Block::Table { header, rows } => {
                    let _table = ui.element(
                        &ElementDesc::new()
                            .width(Sizing::Grow)
                            .height(Sizing::Fit)
                            .direction(Direction::TopToBottom)
                            .border(crate::ui::Border {
                                color: style.rule,
                                left: 1,
                                right: 1,
                                top: 1,
                                bottom: 1,
                                between_children: 1,
                            }),
                    );
                    for (row_index, row) in std::iter::once(header).chain(rows.iter()).enumerate() {
                        let _row = ui.element(
                            &ElementDesc::new()
                                .width(Sizing::Grow)
                                .height(Sizing::Fit)
                                .direction(Direction::LeftToRight)
                                .background(if row_index == 0 { style.code_background } else { Vec4::ZERO })
                                .border(crate::ui::Border { color: style.rule, between_children: 1, ..Default::default() }),
                        );
                        for prose in row {
                            let _cell = ui.element(
                                &ElementDesc::new().width(Sizing::Grow).height(Sizing::Fit).padding(Padding::symmetric(8, 4)),
                            );
                            let font = (row_index == 0).then_some(style.bold_font);
                            self.prose(ui, id, &mut cell, prose, style, 1.0, font, &mut events);
                        }
                    }
                }
            }
        }
        self.blocks = blocks;
        events
    }

    fn quoted(&mut self, ui: &mut Ui<'_>, depth: u8, style: &MarkdownStyle, content: impl FnOnce(&mut Self, &mut Ui<'_>)) {
        if depth == 0 {
            content(self, ui);
            return;
        }
        let _quote = ui.element(
            &ElementDesc::new()
                .width(Sizing::Grow)
                .height(Sizing::Fit)
                .padding(Padding { left: 12 * u16::from(depth), ..Padding::default() })
                .border(crate::ui::Border { color: style.quote_bar, left: 3, ..Default::default() }),
        );
        content(self, ui);
    }

    #[allow(clippy::too_many_arguments)]
    fn prose(
        &mut self,
        ui: &mut Ui<'_>,
        id: &str,
        cell: &mut usize,
        prose: &Prose,
        style: &MarkdownStyle,
        size_scale: f32,
        font_override: Option<u16>,
        events: &mut Vec<MarkdownEvent>,
    ) {
        let index = *cell;
        *cell += 1;
        while self.states.len() <= index {
            self.states.push((TextEditState::new(), ScrollAreaState::new()));
        }
        let (state, scroll) = &mut self.states[index];
        if state.text() != prose.text {
            state.set_text(&prose.text);
        }
        let spans: Vec<RichSpan> = prose
            .runs
            .iter()
            .map(|(start, len, format)| {
                let font = font_override.or(match (format.bold, format.italic, format.code) {
                    (_, _, true) => Some(style.mono_font),
                    (true, true, _) => Some(style.bold_italic_font),
                    (true, false, _) => Some(style.bold_font),
                    (false, true, _) => Some(style.italic_font),
                    _ => None,
                });
                RichSpan {
                    start: *start,
                    len: *len,
                    color: format.link.map(|_| style.link).or(format.color.map(unpack_color)),
                    font_id: font,
                    size_scale: 1.0, // the block's font size is already scaled; scaling the spans too would square the heading size
                    underline: format.link.is_some(),
                    strikethrough: format.strikethrough,
                }
            })
            .collect();
        let text_style = TextInputStyle {
            idle: Vec4::ZERO,
            hovered: Vec4::ZERO,
            focused: Vec4::ZERO,
            disabled: Vec4::ZERO,
            border_idle: Vec4::ZERO,
            border_focused: Vec4::ZERO,
            text_color: style.text,
            caret_color: Vec4::ZERO,
            corner_radius: 0.0,
            font_id: style.regular_font,
            font_size: (f32::from(style.font_size) * size_scale.max(1.0)).round() as u16,
            wrap_lines: true,
            read_only: true,
            ..TextInputStyle::default()
        };
        let result = ui.rich_text(
            &ElementDesc::new().id(format!("{id}::{index}")).width(Sizing::Grow).height(Sizing::Fit),
            &text_style,
            &ScrollbarStyle { visibility: ScrollbarVisibility::AlwaysHidden, ..ScrollbarStyle::default() },
            state,
            scroll,
            &spans,
        );
        if let Some(url) = result.clicked_char.and_then(|c| prose.link_at(c)) {
            events.push(MarkdownEvent::LinkClicked(url.to_owned()));
        }
    }
}

fn pack_color(color: Vec4) -> u32 {
    let channel = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u32;
    (channel(color.x) << 24) | (channel(color.y) << 16) | (channel(color.z) << 8) | channel(color.w)
}

fn unpack_color(packed: u32) -> Vec4 {
    let channel = |shift: u32| ((packed >> shift) & 0xFF) as f32 / 255.0;
    Vec4::new(channel(24), channel(16), channel(8), channel(0))
}

/// Parses Markdown into blocks. Pure (no engine calls), so it is unit tested below.
fn parse(source: &str) -> Vec<Block> {
    let options = Options::ENABLE_TABLES | Options::ENABLE_STRIKETHROUGH | Options::ENABLE_TASKLISTS;
    let mut blocks = Vec::new();
    let mut prose = Prose::default();
    let mut format = InlineFormat::default();
    let mut heading: Option<u8> = None;
    let mut quote_depth: u8 = 0;
    // One entry per open list: Some(next number) for ordered lists, None for bullets.
    let mut lists: Vec<Option<u64>> = Vec::new();
    let mut item_marker: Option<String> = None;
    let mut code: Option<String> = None;
    let mut code_language = String::new();
    let mut table: Option<(Vec<Prose>, Vec<Vec<Prose>>)> = None;
    let mut row: Vec<Prose> = Vec::new();
    let mut in_head = false;

    let flush = |blocks: &mut Vec<Block>, prose: &mut Prose, heading: Option<u8>, marker: &mut Option<String>, depth: usize, quote: u8| {
        if prose.is_empty() && marker.is_none() {
            *prose = Prose::default();
            return;
        }
        let taken = std::mem::take(prose);
        match (heading, marker.take()) {
            (Some(level), _) => blocks.push(Block::Heading { level, prose: taken }),
            (None, Some(marker)) => blocks.push(Block::ListItem {
                depth: depth.saturating_sub(1) as u8,
                marker,
                prose: taken,
                quote_depth: quote,
            }),
            (None, None) => blocks.push(Block::Paragraph { prose: taken, quote_depth: quote }),
        }
    };

    for event in Parser::new_ext(source, options) {
        match event {
            Event::Start(tag) => match tag {
                Tag::Heading { level, .. } => {
                    heading = Some(match level {
                        HeadingLevel::H1 => 1,
                        HeadingLevel::H2 => 2,
                        HeadingLevel::H3 => 3,
                        HeadingLevel::H4 => 4,
                        HeadingLevel::H5 => 5,
                        HeadingLevel::H6 => 6,
                    });
                }
                Tag::BlockQuote(..) => quote_depth = quote_depth.saturating_add(1),
                Tag::CodeBlock(kind) => {
                    code_language = match kind {
                        CodeBlockKind::Fenced(language) => language.into_string(),
                        CodeBlockKind::Indented => String::new(),
                    };
                    code = Some(String::new());
                }
                Tag::List(start) => {
                    // A nested list ends the parent item's own text.
                    flush(&mut blocks, &mut prose, None, &mut item_marker, lists.len(), quote_depth);
                    lists.push(start);
                }
                Tag::Item => {
                    flush(&mut blocks, &mut prose, None, &mut item_marker, lists.len(), quote_depth);
                    item_marker = Some(match lists.last_mut() {
                        Some(Some(number)) => {
                            let marker = format!("{number}.");
                            *number += 1;
                            marker
                        }
                        _ => "\u{2022}".to_owned(),
                    });
                }
                Tag::Emphasis => format.italic = true,
                Tag::Strong => format.bold = true,
                Tag::Strikethrough => format.strikethrough = true,
                Tag::Link { dest_url, .. } => {
                    prose.links.push(dest_url.into_string());
                    format.link = Some(prose.links.len() - 1);
                }
                Tag::Table(_) => table = Some((Vec::new(), Vec::new())),
                Tag::TableHead => in_head = true,
                Tag::TableRow => row.clear(),
                Tag::TableCell => prose = Prose::default(),
                _ => {}
            },
            Event::End(end) => match end {
                TagEnd::Paragraph => {
                    if table.is_none() {
                        flush(&mut blocks, &mut prose, None, &mut item_marker, lists.len(), quote_depth);
                    }
                }
                TagEnd::Heading { .. } => {
                    flush(&mut blocks, &mut prose, heading, &mut item_marker, lists.len(), quote_depth);
                    heading = None;
                }
                TagEnd::BlockQuote { .. } => quote_depth = quote_depth.saturating_sub(1),
                TagEnd::CodeBlock => {
                    if let Some(text) = code.take() {
                        blocks.push(Block::Code {
                            language: std::mem::take(&mut code_language),
                            text: text.trim_end_matches('\n').to_owned(),
                        });
                    }
                }
                TagEnd::List { .. } => {
                    flush(&mut blocks, &mut prose, None, &mut item_marker, lists.len(), quote_depth);
                    lists.pop();
                }
                TagEnd::Item => flush(&mut blocks, &mut prose, None, &mut item_marker, lists.len(), quote_depth),
                TagEnd::Emphasis => format.italic = false,
                TagEnd::Strong => format.bold = false,
                TagEnd::Strikethrough => format.strikethrough = false,
                TagEnd::Link => format.link = None,
                TagEnd::TableCell => row.push(std::mem::take(&mut prose)),
                TagEnd::TableHead => {
                    if let Some((header, _)) = table.as_mut() {
                        *header = std::mem::take(&mut row);
                    }
                    in_head = false;
                }
                TagEnd::TableRow => {
                    if let Some((_, rows)) = table.as_mut() {
                        if !in_head {
                            rows.push(std::mem::take(&mut row));
                        }
                    }
                }
                TagEnd::Table => {
                    if let Some((header, rows)) = table.take() {
                        blocks.push(Block::Table { header, rows });
                    }
                }
                _ => {}
            },
            Event::Text(text) => match code.as_mut() {
                Some(code) => code.push_str(&text),
                None => prose.push(&text, format),
            },
            Event::Code(text) => prose.push(&text, InlineFormat { code: true, ..format }),
            Event::SoftBreak => prose.push(" ", format),
            Event::HardBreak => prose.push("\n", format),
            Event::Rule => blocks.push(Block::Rule),
            Event::TaskListMarker(done) => prose.push(if done { "\u{2611} " } else { "\u{2610} " }, format),
            _ => {}
        }
    }
    flush(&mut blocks, &mut prose, heading, &mut item_marker, lists.len(), quote_depth);
    blocks
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_headings_paragraphs_and_inline_formatting() {
        let blocks = parse("# Title\n\nSome **bold** and *italic* and `code` and [a link](https://example.com).");
        assert_eq!(blocks.len(), 2);
        let Block::Heading { level, prose } = &blocks[0] else { panic!("{blocks:?}") };
        assert_eq!((*level, prose.text.as_str()), (1, "Title"));
        let Block::Paragraph { prose, .. } = &blocks[1] else { panic!("{blocks:?}") };
        assert_eq!(prose.text, "Some bold and italic and code and a link.");
        assert!(prose.runs.iter().any(|(_, _, f)| f.bold));
        assert!(prose.runs.iter().any(|(_, _, f)| f.italic));
        assert!(prose.runs.iter().any(|(_, _, f)| f.code));
        let link_start = prose.text.find("a link").map(|b| prose.text[..b].chars().count()).unwrap();
        assert_eq!(prose.link_at(link_start), Some("https://example.com"));
        assert_eq!(prose.link_at(0), None);
    }

    #[test]
    fn parses_lists_code_rules_and_tables() {
        let source = "- one\n- two\n  1. nested\n\n---\n\n```rust\nfn main() {}\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n";
        let blocks = parse(source);
        let items: Vec<_> = blocks
            .iter()
            .filter_map(|b| match b {
                Block::ListItem { depth, marker, prose, .. } => Some((*depth, marker.clone(), prose.text.clone())),
                _ => None,
            })
            .collect();
        assert_eq!(items, vec![(0, "\u{2022}".into(), "one".into()), (0, "\u{2022}".into(), "two".into()), (1, "1.".into(), "nested".into())]);
        assert!(blocks.contains(&Block::Rule));
        assert!(blocks
            .iter()
            .any(|b| matches!(b, Block::Code { language, text } if language == "rust" && text == "fn main() {}")));
        let table = blocks.iter().find_map(|b| match b {
            Block::Table { header, rows } => Some((header.len(), rows.len(), rows[0][1].text.clone())),
            _ => None,
        });
        assert_eq!(table, Some((2, 1, "2".into())));
    }

    #[test]
    fn colour_packing_round_trips() {
        let color = Vec4::new(1.0, 0.5, 0.0, 1.0);
        let back = unpack_color(pack_color(color));
        assert!((back - color).abs().max_element() < 0.01);
    }

    #[test]
    fn link_hit_testing_counts_chars_not_bytes() {
        let blocks = parse("ünïcödé [link](u)");
        let Block::Paragraph { prose, .. } = &blocks[0] else { panic!() };
        assert_eq!(prose.link_at(8), Some("u"));
        assert_eq!(prose.link_at(7), None);
    }
}
