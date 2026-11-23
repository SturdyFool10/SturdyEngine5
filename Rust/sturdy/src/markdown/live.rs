//! Live preview: the styling of Markdown *source* text, for an editor that shows the document rendered while it stays one editable text.
//!
//! [`live_spans`] reads the source and returns [`RichSpan`]s over its characters: headings large, `**bold**` bold, links coloured, code in
//! the mono font (with the built-in highlighter's colours), quotes muted. The markup characters (`#`, `**`, `[`, `](url)`, fences, `>`, `$`)
//! are hidden ([`RichSpan::hidden`]: no room, no drawing, still in the text) except where the app says to show them, in which case they are
//! drawn dimmed. Which markup shows is the caller's decision: pass the byte ranges of the source it wants revealed (the block under the
//! caret, the line, a selection, everything). Draw the result with [`Ui::rich_text_edit`](crate::ui::Ui::rich_text_edit).

use std::ops::Range;

use glam::Vec4;
use pulldown_cmark::{CodeBlockKind, Event, Options, Parser, Tag};

use super::highlight::MarkdownHighlighter;
use super::style::MarkdownStyle;
use crate::widgets::RichSpan;

/// What is drawn for one character.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
struct Attr {
    bold: bool,
    italic: bool,
    mono: bool,
    heading: u8,
    strike: bool,
    underline: bool,
    link: bool,
    muted: bool,
    hidden: bool,
    script: i8,
    color: Option<[u8; 4]>,
}

fn pack(color: Vec4) -> [u8; 4] {
    let channel = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    [channel(color.x), channel(color.y), channel(color.z), channel(color.w)]
}

fn unpack(color: [u8; 4]) -> Vec4 {
    Vec4::new(f32::from(color[0]) / 255.0, f32::from(color[1]) / 255.0, f32::from(color[2]) / 255.0, f32::from(color[3]) / 255.0)
}

struct Painter<'a> {
    source: &'a str,
    /// Char index of every byte offset (and of the end).
    chars_before: Vec<usize>,
    attrs: Vec<Attr>,
    reveal: &'a [Range<usize>],
    reveal_all: bool,
}

impl Painter<'_> {
    fn char_range(&self, bytes: &Range<usize>) -> Range<usize> {
        let at = |byte: usize| self.chars_before.get(byte.min(self.source.len())).copied().unwrap_or(self.attrs.len());
        at(bytes.start)..at(bytes.end).max(at(bytes.start))
    }

    fn paint(&mut self, bytes: &Range<usize>, change: impl Fn(&mut Attr)) {
        let range = self.char_range(bytes);
        for attr in self.attrs.get_mut(range).unwrap_or_default() {
            change(attr);
        }
    }

    fn revealed(&self, bytes: &Range<usize>) -> bool {
        self.reveal_all || self.reveal.iter().any(|r| bytes.start >= r.start && bytes.start <= r.end)
    }

    /// Paints markup: hidden unless the app wants it shown, then dimmed.
    fn marker(&mut self, bytes: Range<usize>) {
        if bytes.is_empty() {
            return;
        }
        let show = self.revealed(&bytes);
        self.paint(&bytes, |a| {
            a.muted = true;
            a.hidden = !show;
        });
    }
}

/// Splits `bytes` of the source in two: the first `lead` bytes and the last `trail` bytes are markup.
fn ends(bytes: &Range<usize>, lead: usize, trail: usize) -> (Range<usize>, Range<usize>, Range<usize>) {
    let lead_end = (bytes.start + lead).min(bytes.end);
    let trail_start = bytes.end.saturating_sub(trail).max(lead_end);
    (bytes.start..lead_end, lead_end..trail_start, trail_start..bytes.end)
}

fn run_len(source: &str, from: usize, c: char) -> usize {
    source.get(from..).map_or(0, |rest| rest.chars().take_while(|x| *x == c).count())
}

/// The spans that style `source` as a live-preview document. `reveal` lists byte ranges of the source whose markup stays visible (dimmed);
/// `reveal_all` shows all of it. Ranges and spans are in bytes and characters respectively; spans cover the whole text, in order.
pub fn live_spans(source: &str, style: &MarkdownStyle, reveal: &[Range<usize>], reveal_all: bool) -> Vec<RichSpan> {
    let total = source.chars().count();
    let mut chars_before = vec![0usize; source.len() + 1];
    for (count, (byte, c)) in source.char_indices().enumerate() {
        for slot in chars_before.iter_mut().skip(byte).take(c.len_utf8()) {
            *slot = count;
        }
    }
    if let Some(last) = chars_before.last_mut() {
        *last = total;
    }
    let mut painter = Painter { source, chars_before, attrs: vec![Attr::default(); total], reveal, reveal_all };

    let f = &style.features;
    let mut options = Options::empty();
    for (on, option) in [
        (f.tables, Options::ENABLE_TABLES),
        (f.strikethrough, Options::ENABLE_STRIKETHROUGH),
        (f.task_lists, Options::ENABLE_TASKLISTS),
        (f.footnotes, Options::ENABLE_FOOTNOTES),
        (f.math, Options::ENABLE_MATH),
        (f.superscript, Options::ENABLE_SUPERSCRIPT),
        (f.subscript, Options::ENABLE_SUBSCRIPT),
        (f.heading_attributes, Options::ENABLE_HEADING_ATTRIBUTES),
        (f.wikilinks, Options::ENABLE_WIKILINKS),
    ] {
        if on {
            options |= option;
        }
    }
    let highlighter = MarkdownHighlighter::new(style.code_colours);
    for (event, range) in Parser::new_ext(source, options).into_offset_iter() {
        match event {
            Event::Start(tag) => match tag {
                Tag::Heading { level, .. } => {
                    let level = level as u8;
                    // The line break after a heading is not part of it (it would be drawn at the heading's size).
                    let range = range.start..range.start + source.get(range.clone()).unwrap_or_default().trim_end_matches(['\n', '\r']).len();
                    let slice = source.get(range.clone()).unwrap_or_default();
                    if slice.starts_with('#') {
                        let hashes = run_len(source, range.start, '#');
                        let spaces = source.get(range.start + hashes..).map_or(0, |r| r.chars().take_while(|c| *c == ' ').count());
                        let mut trail = 0;
                        if slice.trim_end().ends_with('}') && let Some(open) = slice.rfind(" {") {
                            trail = slice.len() - open;
                        }
                        let (lead_range, content, trail_range) = ends(&range, hashes + spaces, trail);
                        painter.paint(&content, |a| a.heading = level);
                        painter.paint(&lead_range, |a| a.heading = level);
                        painter.marker(lead_range);
                        painter.marker(trail_range);
                    } else {
                        painter.paint(&range, |a| a.heading = level);
                    }
                }
                Tag::Strong => {
                    let (open, content, close) = ends(&range, 2, 2);
                    painter.paint(&content, |a| a.bold = true);
                    painter.marker(open);
                    painter.marker(close);
                }
                Tag::Emphasis => {
                    let (open, content, close) = ends(&range, 1, 1);
                    painter.paint(&content, |a| a.italic = true);
                    painter.marker(open);
                    painter.marker(close);
                }
                Tag::Strikethrough => {
                    let width = run_len(source, range.start, '~').clamp(1, 2);
                    let (open, content, close) = ends(&range, width, width);
                    painter.paint(&content, |a| a.strike = true);
                    painter.marker(open);
                    painter.marker(close);
                }
                Tag::Superscript | Tag::Subscript => {
                    let up = matches!(tag, Tag::Superscript);
                    let (open, content, close) = ends(&range, 1, 1);
                    painter.paint(&content, |a| a.script = if up { 1 } else { -1 });
                    painter.marker(open);
                    painter.marker(close);
                }
                Tag::Link { .. } => {
                    let slice = source.get(range.clone()).unwrap_or_default();
                    let wiki = slice.starts_with("[[");
                    let tail = if wiki {
                        2
                    } else if let Some(at) = slice.rfind("](") {
                        slice.len() - at
                    } else if let Some(at) = slice.rfind("][") {
                        slice.len() - at
                    } else if slice.starts_with('<') {
                        1
                    } else {
                        0
                    };
                    let lead = if wiki { 2 } else { usize::from(slice.starts_with('[') || slice.starts_with('<')) };
                    if lead == 0 && tail == 0 {
                        painter.paint(&range, |a| {
                            a.link = true;
                            a.underline = true;
                        });
                    } else {
                        let (open, content, close) = ends(&range, lead, tail);
                        painter.paint(&content, |a| {
                            a.link = true;
                            a.underline = true;
                        });
                        painter.marker(open);
                        painter.marker(close);
                    }
                }
                Tag::Image { .. } => {
                    let slice = source.get(range.clone()).unwrap_or_default();
                    let tail = slice.rfind("](").map_or(0, |at| slice.len() - at);
                    let (open, content, close) = ends(&range, 2, tail);
                    painter.paint(&content, |a| {
                        a.italic = true;
                        a.muted = true;
                    });
                    painter.marker(open);
                    painter.marker(close);
                }
                Tag::BlockQuote(_) => {
                    painter.paint(&range, |a| {
                        a.italic = true;
                        a.muted = true;
                    });
                    let mut at = range.start;
                    for line in source.get(range.clone()).unwrap_or_default().split_inclusive('\n') {
                        let indent = line.len() - line.trim_start_matches([' ', '\t']).len();
                        if line.get(indent..).is_some_and(|l| l.starts_with('>')) {
                            let after = line.get(indent + 1..).map_or(0, |l| usize::from(l.starts_with(' ')));
                            painter.marker(at + indent..at + indent + 1 + after);
                        }
                        at += line.len();
                    }
                }
                Tag::CodeBlock(kind) => {
                    let code_language = match kind {
                        CodeBlockKind::Fenced(language) => language.into_string(),
                        CodeBlockKind::Indented => String::new(),
                    };
                    painter.paint(&range, |a| a.mono = true);
                    let slice = source.get(range.clone()).unwrap_or_default();
                    if matches!(slice.chars().next(), Some('`' | '~')) {
                        let first_end = slice.find('\n').map_or(slice.len(), |at| at + 1);
                        painter.marker(range.start..range.start + first_end);
                        let body = slice.get(first_end..).unwrap_or_default();
                        let last_start = body.trim_end_matches(['\n', '\r']).rfind('\n').map_or(0, |at| at + 1);
                        let last = body.get(last_start..).unwrap_or_default();
                        if last.trim().starts_with(['`', '~']) {
                            painter.marker(range.start + first_end + last_start..range.end);
                        }
                        let code = range.start + first_end..(range.start + first_end + last_start).max(range.start + first_end);
                        let text = source.get(code.clone()).unwrap_or_default();
                        let start_char = painter.char_range(&code).start;
                        for span in highlighter.spans(&code_language, text) {
                            for attr in painter.attrs.iter_mut().skip(start_char + span.start).take(span.len) {
                                attr.color = Some(pack(span.color));
                            }
                        }
                    }
                }
                Tag::Item => {
                    // The list marker stays, dimmed.
                    let slice = source.get(range.clone()).unwrap_or_default();
                    let width = slice.chars().take_while(|c| matches!(c, '-' | '*' | '+' | '0'..='9' | '.' | ')')).count();
                    painter.paint(&(range.start..range.start + width), |a| a.muted = true);
                }
                _ => {}
            },
            Event::Code(_) => {
                let width = run_len(source, range.start, '`').max(1);
                let (open, content, close) = ends(&range, width, width);
                painter.paint(&content, |a| a.mono = true);
                painter.marker(open);
                painter.marker(close);
            }
            Event::InlineMath(_) | Event::DisplayMath(_) => {
                let width = run_len(source, range.start, '$').max(1);
                let (open, content, close) = ends(&range, width, width);
                painter.paint(&content, |a| {
                    a.mono = true;
                    a.color = style.math_colour.map(pack);
                });
                painter.marker(open);
                painter.marker(close);
            }
            Event::Rule => painter.paint(&range, |a| a.muted = true),
            _ => {}
        }
    }

    // Compress per-char attributes into spans.
    let mut spans: Vec<RichSpan> = Vec::new();
    let mut previous: Option<Attr> = None;
    for (index, attr) in painter.attrs.iter().enumerate() {
        if previous == Some(*attr) {
            if let Some(last) = spans.last_mut() {
                last.len += 1;
            }
            continue;
        }
        previous = Some(*attr);
        let heading_scale = (attr.heading > 0).then(|| style.heading_scale.get(usize::from(attr.heading - 1)).copied().unwrap_or(1.0));
        let font = if attr.mono {
            Some(style.mono_font)
        } else if attr.heading > 0 {
            Some(style.heading_font.unwrap_or(style.bold_font))
        } else {
            match (attr.bold, attr.italic) {
                (true, true) => Some(style.bold_italic_font),
                (true, false) => Some(style.bold_font),
                (false, true) => Some(style.italic_font),
                _ => None,
            }
        };
        let color = if attr.link {
            Some(style.link)
        } else if let Some(color) = attr.color {
            Some(unpack(color))
        } else if attr.muted {
            Some(style.muted)
        } else {
            None
        };
        let script_scale = if attr.script != 0 { style.script_scale } else { 1.0 };
        spans.push(RichSpan {
            start: index,
            len: 1,
            color,
            font_id: font,
            size_scale: heading_scale.unwrap_or(1.0) * script_scale,
            underline: attr.underline,
            strikethrough: attr.strike,
            baseline_shift: match attr.script {
                1 => style.superscript_shift,
                -1 => style.subscript_shift,
                _ => 0.0,
            },
            hidden: attr.hidden,
            ..RichSpan::default()
        });
    }
    spans
}

/// `spans` adjusted for `inserted` characters typed at char offset `at`: later spans move right and the span the text lands in grows. Used
/// to draw the frame of a keystroke with spans that still line up, before the styling is recomputed from the new text.
pub fn shift_spans_for_insertion(spans: &[RichSpan], at: usize, inserted: usize) -> Vec<RichSpan> {
    spans
        .iter()
        .map(|span| {
            let mut span = *span;
            if span.start >= at {
                span.start += inserted;
            } else if at <= span.start + span.len {
                // Typed inside the span or at its end: it takes the text's style.
                span.len += inserted;
            }
            span
        })
        .collect()
}
