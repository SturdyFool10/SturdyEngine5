//! Markdown source to blocks. Pure (no engine calls), so it is unit tested without a UI.

use std::collections::{HashMap, HashSet};
use std::ops::Range;

use pulldown_cmark::{Alignment, BlockQuoteKind, CodeBlockKind, Event, HeadingLevel, LinkType, Options, Parser, Tag, TagEnd};

use super::style::{MarkdownFeatures, MathHook, MathMode, MathRender, SoftBreak};
use super::tex::{self, TexNode};

/// Vertical placement of a run drawn smaller (superscript, subscript).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(super) enum Script {
    #[default]
    None,
    Sup,
    Sub,
}

/// Inline formatting active over a range of a run.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(super) struct InlineFormat {
    /// Packed 0xRRGGBBAA colour override (code colouring).
    pub color: Option<u32>,
    pub bold: bool,
    pub italic: bool,
    pub code: bool,
    pub strikethrough: bool,
    pub underline: bool,
    pub math: bool,
    pub script: Script,
    pub link: Option<usize>,
}

/// What clicking a link does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum LinkKind {
    /// A URL the application opens.
    External,
    /// `[[Page]]`.
    Wiki,
    /// `[text](#heading)`.
    Anchor,
    /// A footnote reference: jumps to the footnote.
    Footnote,
    /// The arrow after a footnote: jumps back to the reference.
    FootnoteBack,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct LinkTarget {
    pub url: String,
    pub title: String,
    pub kind: LinkKind,
}

/// One block of prose: its plain text, styled ranges, and link targets.
#[derive(Debug, Clone, Default, PartialEq)]
pub(super) struct Prose {
    pub text: String,
    /// (start char, char length, format) in order.
    pub runs: Vec<(usize, usize, InlineFormat)>,
    pub links: Vec<LinkTarget>,
    pub chars: usize,
    /// Names [`MarkdownView::scroll_to`](super::MarkdownView::scroll_to) can jump to this block by.
    pub anchors: Vec<String>,
}

impl Prose {
    pub fn push(&mut self, text: &str, format: InlineFormat) {
        let count = text.chars().count();
        if count == 0 {
            return;
        }
        self.text.push_str(text);
        match self.runs.last_mut() {
            Some((_, len, last)) if *last == format => *len += count,
            _ => self.runs.push((self.chars, count, format)),
        }
        self.chars += count;
    }

    pub fn is_empty(&self) -> bool {
        self.text.trim().is_empty()
    }

    /// Appends `other` (its runs, links and anchors) after this prose's text.
    pub fn append(&mut self, other: &Prose) {
        let link_offset = self.links.len();
        let char_offset = self.chars;
        self.text.push_str(&other.text);
        self.chars += other.chars;
        self.links.extend(other.links.iter().cloned());
        self.anchors.extend(other.anchors.iter().cloned());
        for (start, len, format) in &other.runs {
            let format = InlineFormat { link: format.link.map(|link| link + link_offset), ..*format };
            self.runs.push((start + char_offset, *len, format));
        }
    }

    pub fn link_target_at(&self, char_index: usize) -> Option<&LinkTarget> {
        self.runs
            .iter()
            .find(|(start, len, _)| char_index >= *start && char_index < start + len)
            .and_then(|(_, _, format)| format.link)
            .and_then(|link| self.links.get(link))
    }

    /// Index into `links` of the link at char `char_index`.
    pub fn link_index_at(&self, char_index: usize) -> Option<usize> {
        self.runs.iter().find(|(start, len, _)| char_index >= *start && char_index < start + len).and_then(|(_, _, format)| format.link)
    }

    #[cfg(test)]
    pub fn link_at(&self, char_index: usize) -> Option<&str> {
        self.link_target_at(char_index).map(|target| target.url.as_str())
    }

    /// Turns bare URLs in plain runs (not code, math or existing links) into links.
    pub fn linkify(self) -> Prose {
        let mut out = Prose { links: self.links.clone(), anchors: self.anchors.clone(), ..Prose::default() };
        let mut byte = 0usize;
        for (_, len, format) in &self.runs {
            let rest = self.text.get(byte..).unwrap_or_default();
            let end = rest.char_indices().nth(*len).map_or(rest.len(), |(index, _)| index);
            let piece = rest.get(..end).unwrap_or_default();
            byte += end;
            if format.link.is_some() || format.code || format.math {
                out.push(piece, *format);
                continue;
            }
            let mut at = 0usize;
            for (start, stop) in find_urls(piece) {
                out.push(piece.get(at..start).unwrap_or_default(), *format);
                let shown = piece.get(start..stop).unwrap_or_default();
                let url = if shown.len() >= 4 && shown.get(..4).is_some_and(|p| p.eq_ignore_ascii_case("www.")) {
                    format!("http://{shown}")
                } else {
                    shown.to_owned()
                };
                out.links.push(LinkTarget { url, title: String::new(), kind: LinkKind::External });
                out.push(shown, InlineFormat { link: Some(out.links.len() - 1), ..*format });
                at = stop;
            }
            out.push(piece.get(at..).unwrap_or_default(), *format);
        }
        out
    }
}

fn starts_with_ci(text: &str, prefix: &str) -> bool {
    text.get(..prefix.len()).is_some_and(|head| head.eq_ignore_ascii_case(prefix))
}

/// Byte ranges of the bare URLs in `text`, the way GitHub finds them: `http://`, `https://` or `www.` after whitespace or an opening
/// `* _ ~ (`, up to whitespace, minus trailing punctuation and an unbalanced `)`.
pub(super) fn find_urls(text: &str) -> Vec<(usize, usize)> {
    let mut found = Vec::new();
    let mut at = 0usize;
    while at < text.len() {
        let Some(rest) = text.get(at..) else { break };
        let scheme = ["https://", "http://", "www."].into_iter().find(|prefix| starts_with_ci(rest, prefix));
        let boundary_ok = text
            .get(..at)
            .and_then(|before| before.chars().next_back())
            .is_none_or(|c| c.is_whitespace() || matches!(c, '*' | '_' | '~' | '('));
        match scheme {
            Some(prefix) if boundary_ok => {
                let stop = rest.find(|c: char| c.is_whitespace() || c == '<').unwrap_or(rest.len());
                let mut url = rest.get(..stop).unwrap_or_default();
                while let Some(last) = url.chars().next_back() {
                    let unbalanced_paren = last == ')' && url.matches('(').count() < url.matches(')').count();
                    if !(matches!(last, '?' | '!' | '.' | ',' | ':' | '*' | '_' | '~' | '\'' | '"' | ';') || unbalanced_paren) {
                        break;
                    }
                    url = url.get(..url.len() - last.len_utf8()).unwrap_or_default();
                }
                let host = url.get(prefix.len()..).unwrap_or_default();
                if host.chars().next().is_some_and(char::is_alphanumeric) {
                    found.push((at, at + url.len()));
                    at += url.len();
                    continue;
                }
            }
            _ => {}
        }
        at += rest.chars().next().map_or(1, char::len_utf8);
    }
    found
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum AlertKind {
    Note,
    Tip,
    Important,
    Warning,
    Caution,
}

impl AlertKind {
    pub fn title(self) -> &'static str {
        match self {
            Self::Note => "Note",
            Self::Tip => "Tip",
            Self::Important => "Important",
            Self::Warning => "Warning",
            Self::Caution => "Caution",
        }
    }
}

impl From<BlockQuoteKind> for AlertKind {
    fn from(kind: BlockQuoteKind) -> Self {
        match kind {
            BlockQuoteKind::Note => Self::Note,
            BlockQuoteKind::Tip => Self::Tip,
            BlockQuoteKind::Important => Self::Important,
            BlockQuoteKind::Warning => Self::Warning,
            BlockQuoteKind::Caution => Self::Caution,
        }
    }
}

/// The block quotes a block sits in, outermost first; `Some` marks an alert.
pub(super) type Quote = Vec<Option<AlertKind>>;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(super) enum ColumnAlign {
    #[default]
    Left,
    Center,
    Right,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct TaskItem {
    /// Ordinal among the document's task items.
    pub index: usize,
    pub checked: bool,
}

/// What a list looks like to the items inside it (for aligning their numbers).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct ListInfo {
    pub start: Option<u64>,
    pub count: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub(super) enum Block {
    Paragraph { prose: Prose, quote: Quote },
    Heading { level: u8, prose: Prose, id: Option<String>, classes: Vec<String> },
    ListItem { depth: u8, marker: String, ordinal: Option<u64>, list: usize, prose: Prose, quote: Quote, task: Option<TaskItem> },
    Code { language: String, text: String },
    Rule,
    Table { header: Vec<Prose>, rows: Vec<Vec<Prose>>, aligns: Vec<ColumnAlign> },
    Image { url: String, alt: String, title: String, link: Option<String>, quote: Quote },
    /// The title line of a GitHub alert.
    AlertTitle { kind: AlertKind, quote: Quote },
    DefTerm { prose: Prose },
    DefDetail { prose: Prose },
    /// Display math: `expr` is the parsed formula for [`MathMode::Layout`] (`None` when it does not parse).
    Math { tex: String, render: Option<MathRender>, expr: Option<TexNode> },
    Footnote { number: usize, label: String, prose: Prose },
}

/// What the parser needs from the style.
#[derive(Debug, Clone, PartialEq, Default)]
pub(super) struct ParseConfig {
    pub smart_punctuation: bool,
    pub underline_syntax: bool,
    pub soft_break: SoftBreak,
    pub autolink: bool,
    pub unicode_scripts: bool,
    pub task_glyphs: bool,
    pub math_hook: Option<MathHook>,
    pub math_mode: MathMode,
    pub features: MarkdownFeatures,
}

impl ParseConfig {
    pub fn from_style(style: &super::style::MarkdownStyle) -> Self {
        Self {
            smart_punctuation: style.smart_punctuation,
            underline_syntax: style.underline_syntax,
            soft_break: style.soft_break,
            autolink: style.autolink,
            unicode_scripts: style.unicode_scripts,
            task_glyphs: style.task_glyphs,
            math_hook: style.math_hook.clone(),
            math_mode: style.math_mode,
            features: style.features,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub(super) struct Parsed {
    pub blocks: Vec<Block>,
    /// The source byte range each block was written in, parallel to `blocks`. Empty for blocks the parser made up (the rule before the
    /// footnotes). Pieces of one split paragraph share their paragraph's range.
    pub ranges: Vec<Range<usize>>,
    /// Source byte range of each task item's `[ ]`/`[x]`.
    pub task_ranges: Vec<Range<usize>>,
    pub lists: Vec<ListInfo>,
}

const SUP_DIGITS: [char; 10] = ['\u{2070}', '\u{b9}', '\u{b2}', '\u{b3}', '\u{2074}', '\u{2075}', '\u{2076}', '\u{2077}', '\u{2078}', '\u{2079}'];
const SUB_DIGITS: [char; 10] = ['\u{2080}', '\u{2081}', '\u{2082}', '\u{2083}', '\u{2084}', '\u{2085}', '\u{2086}', '\u{2087}', '\u{2088}', '\u{2089}'];

fn script_char(c: char, sup: bool) -> Option<char> {
    if let Some(digit) = c.to_digit(10) {
        let table = if sup { &SUP_DIGITS } else { &SUB_DIGITS };
        return table.get(digit as usize).copied();
    }
    Some(match (c, sup) {
        ('+', true) => '\u{207a}',
        ('-', true) => '\u{207b}',
        ('=', true) => '\u{207c}',
        ('(', true) => '\u{207d}',
        (')', true) => '\u{207e}',
        ('n', true) => '\u{207f}',
        ('i', true) => '\u{2071}',
        ('+', false) => '\u{208a}',
        ('-', false) => '\u{208b}',
        ('=', false) => '\u{208c}',
        ('(', false) => '\u{208d}',
        (')', false) => '\u{208e}',
        ('a', false) => '\u{2090}',
        ('e', false) => '\u{2091}',
        ('o', false) => '\u{2092}',
        ('x', false) => '\u{2093}',
        ('h', false) => '\u{2095}',
        ('k', false) => '\u{2096}',
        ('l', false) => '\u{2097}',
        ('m', false) => '\u{2098}',
        ('n', false) => '\u{2099}',
        ('p', false) => '\u{209a}',
        ('s', false) => '\u{209b}',
        ('t', false) => '\u{209c}',
        _ => return None,
    })
}

/// `text` in Unicode superscript (or subscript) characters, when every character has one.
pub(super) fn to_script(text: &str, sup: bool) -> Option<String> {
    text.chars().map(|c| script_char(c, sup)).collect()
}

/// GitHub-style heading id: lower case, letters and digits kept, spaces and hyphens become `-`.
pub(super) fn slugify(text: &str) -> String {
    let mut slug = String::new();
    for c in text.trim().chars() {
        if c.is_alphanumeric() {
            slug.extend(c.to_lowercase());
        } else if c == ' ' || c == '-' || c == '_' {
            slug.push(if c == '_' { '_' } else { '-' });
        }
    }
    slug
}

/// A table being collected: header cells, body rows, column alignments.
type TableParts = (Vec<Prose>, Vec<Vec<Prose>>, Vec<ColumnAlign>);

struct ItemContext {
    marker: String,
    ordinal: Option<u64>,
    list: usize,
    task: Option<TaskItem>,
}

struct HeadingContext {
    level: u8,
    id: Option<String>,
    classes: Vec<String>,
}

struct ImageContext {
    url: String,
    title: String,
    alt: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DefKind {
    Term,
    Detail,
}

struct Builder<'a> {
    source: &'a str,
    config: &'a ParseConfig,
    blocks: Vec<Block>,
    ranges: Vec<Range<usize>>,
    /// Source range of the paragraph, heading, list item or definition being collected.
    unit: Range<usize>,
    code_range: Range<usize>,
    table_range: Range<usize>,
    /// Greater than zero while inside a construct the features turn off: its events are skipped.
    skip_depth: usize,
    prose: Prose,
    format: InlineFormat,
    heading: Option<HeadingContext>,
    quote: Quote,
    /// One entry per open list: (list id, next number for ordered lists).
    lists: Vec<(usize, Option<u64>)>,
    list_infos: Vec<ListInfo>,
    item: Option<ItemContext>,
    code: Option<String>,
    code_language: String,
    table: Option<TableParts>,
    row: Vec<Prose>,
    in_head: bool,
    image: Option<ImageContext>,
    link_urls: Vec<Option<String>>,
    def: Option<DefKind>,
    strong_is_underline: Vec<bool>,
    task_ranges: Vec<Range<usize>>,
    footnote_numbers: HashMap<String, usize>,
    footnote_defs: Vec<FootnoteDef>,
    footnote_stack: Vec<FootnoteDef>,
    used_slugs: HashMap<String, usize>,
}

/// A footnote definition being collected or finished: its label, the blocks inside it (or, while open, the blocks outside it) and their
/// ranges, and its own source range.
struct FootnoteDef {
    label: String,
    blocks: Vec<Block>,
    ranges: Vec<Range<usize>>,
    range: Range<usize>,
}

impl<'a> Builder<'a> {
    fn new(source: &'a str, config: &'a ParseConfig) -> Self {
        Self {
            source,
            config,
            blocks: Vec::new(),
            ranges: Vec::new(),
            unit: 0..0,
            code_range: 0..0,
            table_range: 0..0,
            skip_depth: 0,
            prose: Prose::default(),
            format: InlineFormat::default(),
            heading: None,
            quote: Vec::new(),
            lists: Vec::new(),
            list_infos: Vec::new(),
            item: None,
            code: None,
            code_language: String::new(),
            table: None,
            row: Vec::new(),
            in_head: false,
            image: None,
            link_urls: Vec::new(),
            def: None,
            strong_is_underline: Vec::new(),
            task_ranges: Vec::new(),
            footnote_numbers: HashMap::new(),
            footnote_defs: Vec::new(),
            footnote_stack: Vec::new(),
            used_slugs: HashMap::new(),
        }
    }

    fn finish_prose(&self, prose: Prose) -> Prose {
        if self.config.autolink {
            prose.linkify()
        } else {
            prose
        }
    }

    fn emit(&mut self, block: Block, range: Range<usize>) {
        self.blocks.push(block);
        self.ranges.push(range);
    }

    fn flush(&mut self) {
        self.flush_to(None);
    }

    /// Ends the block of prose being collected, as whatever kind of block the open context says it is. `limit` cuts the block's source
    /// range short (a list item ends where its nested list starts).
    fn flush_to(&mut self, limit: Option<usize>) {
        if self.prose.is_empty() && self.item.is_none() {
            self.prose = Prose::default();
            return;
        }
        let mut range = self.unit.clone();
        if let Some(limit) = limit {
            range.end = range.end.min(limit).max(range.start);
            // A nested list starts after its indentation: the item ends before that, on its own line.
            while range.end > range.start && self.source.get(..range.end).is_some_and(|head| head.ends_with([' ', '\t'])) {
                range.end -= 1;
            }
        }
        let taken = std::mem::take(&mut self.prose);
        let mut prose = self.finish_prose(taken);
        if let Some(heading) = self.heading.take() {
            let id = heading.id.or_else(|| {
                let slug = slugify(&prose.text);
                if slug.is_empty() {
                    return None;
                }
                let count = self.used_slugs.entry(slug.clone()).or_insert(0);
                let id = if *count == 0 { slug } else { format!("{slug}-{count}") };
                *count += 1;
                Some(id)
            });
            if let Some(id) = &id {
                prose.anchors.push(id.clone());
            }
            self.emit(Block::Heading { level: heading.level, prose, id, classes: heading.classes }, range);
            return;
        }
        if let Some(item) = self.item.take() {
            let block = Block::ListItem {
                depth: self.lists.len().saturating_sub(1) as u8,
                marker: item.marker,
                ordinal: item.ordinal,
                list: item.list,
                prose,
                quote: self.quote.clone(),
                task: item.task,
            };
            self.emit(block, range);
            return;
        }
        let block = match self.def {
            Some(DefKind::Term) => Block::DefTerm { prose },
            Some(DefKind::Detail) => Block::DefDetail { prose },
            None => Block::Paragraph { prose, quote: self.quote.clone() },
        };
        self.emit(block, range);
    }

    /// `range` of the source as a plain paragraph: what a construct turned off by the features shows.
    fn literal_block(&mut self, range: &Range<usize>) {
        self.flush();
        let text = self.source.get(range.clone()).unwrap_or_default().trim_end_matches(['\n', '\r']);
        let mut prose = Prose::default();
        prose.push(text, InlineFormat::default());
        self.emit(Block::Paragraph { prose, quote: self.quote.clone() }, range.clone());
    }

    /// Whether the features turn this construct off, and if so whether it is a block or inline.
    fn disabled(&self, tag: &Tag<'_>) -> Option<bool> {
        let f = &self.config.features;
        match tag {
            Tag::Heading { .. } => (!f.headings).then_some(true),
            Tag::BlockQuote(_) => (!f.block_quotes).then_some(true),
            Tag::CodeBlock(_) => (!f.code_blocks).then_some(true),
            Tag::List(_) => (!f.lists).then_some(true),
            Tag::Emphasis => (!f.emphasis).then_some(false),
            Tag::Strong => (!f.strong).then_some(false),
            Tag::Link { .. } => (!f.links).then_some(false),
            Tag::Image { .. } => (!f.images).then_some(false),
            _ => None,
        }
    }

    /// Ends a heading: like [`Self::flush`] but the heading context stays open until its end tag.
    fn end_heading(&mut self) {
        self.flush();
        self.heading = None;
    }

    fn push_text(&mut self, text: &str) {
        if self.image.as_ref().is_some_and(|_| self.table.is_none() && self.heading.is_none()) {
            if let Some(image) = self.image.as_mut() {
                image.alt.push_str(text);
            }
            return;
        }
        let sup = self.format.script == Script::Sup;
        if self.format.script != Script::None
            && self.config.unicode_scripts
            && let Some(mapped) = to_script(text, sup)
        {
            self.prose.push(&mapped, InlineFormat { script: Script::None, ..self.format });
            return;
        }
        self.prose.push(text, self.format);
    }

    fn link_index(&mut self, target: LinkTarget) -> usize {
        self.prose.links.push(target);
        self.prose.links.len() - 1
    }

    fn start(&mut self, tag: Tag<'_>, range: &Range<usize>) {
        match tag {
            Tag::Paragraph => self.unit = range.clone(),
            Tag::Heading { level, id, classes, .. } => {
                self.unit = range.clone();
                self.heading = Some(HeadingContext {
                    level: match level {
                        HeadingLevel::H1 => 1,
                        HeadingLevel::H2 => 2,
                        HeadingLevel::H3 => 3,
                        HeadingLevel::H4 => 4,
                        HeadingLevel::H5 => 5,
                        HeadingLevel::H6 => 6,
                    },
                    id: id.map(|id| id.into_string()),
                    classes: classes.into_iter().map(|class| class.into_string()).collect(),
                });
            }
            Tag::BlockQuote(kind) => {
                self.flush();
                let alert = kind.map(AlertKind::from);
                self.quote.push(alert);
                if let Some(kind) = alert {
                    let first_line = self.source.get(range.start..).and_then(|rest| rest.find('\n')).map_or(range.end, |at| range.start + at);
                    self.emit(Block::AlertTitle { kind, quote: self.quote.clone() }, range.start..first_line);
                }
            }
            Tag::CodeBlock(kind) => {
                self.flush();
                self.code_range = range.clone();
                self.code_language = match kind {
                    CodeBlockKind::Fenced(language) => language.into_string(),
                    CodeBlockKind::Indented => String::new(),
                };
                self.code = Some(String::new());
            }
            Tag::List(start) => {
                // A nested list ends the parent item's own text.
                self.flush_to(Some(range.start));
                self.list_infos.push(ListInfo { start, count: 0 });
                self.lists.push((self.list_infos.len() - 1, start));
            }
            Tag::Item => {
                self.flush();
                let (list, ordinal, marker) = match self.lists.last_mut() {
                    Some((list, Some(number))) => {
                        let ordinal = *number;
                        *number += 1;
                        (*list, Some(ordinal), format!("{ordinal}."))
                    }
                    Some((list, None)) => (*list, None, "\u{2022}".to_owned()),
                    None => (usize::MAX, None, "\u{2022}".to_owned()),
                };
                if let Some(info) = self.list_infos.get_mut(list) {
                    info.count += 1;
                }
                self.unit = range.clone();
                self.item = Some(ItemContext { marker, ordinal, list, task: None });
            }
            Tag::DefinitionList => self.flush(),
            Tag::DefinitionListTitle => {
                self.flush();
                self.unit = range.clone();
                self.def = Some(DefKind::Term);
            }
            Tag::DefinitionListDefinition => {
                self.flush();
                self.unit = range.clone();
                self.def = Some(DefKind::Detail);
            }
            Tag::FootnoteDefinition(label) => {
                self.flush();
                let outer = std::mem::take(&mut self.blocks);
                let outer_ranges = std::mem::take(&mut self.ranges);
                self.footnote_stack.push(FootnoteDef { label: label.into_string(), blocks: outer, ranges: outer_ranges, range: range.clone() });
            }
            Tag::Emphasis => self.format.italic = true,
            Tag::Strong => {
                let underline = self.config.underline_syntax && self.source.get(range.start..).is_some_and(|rest| rest.starts_with("__"));
                self.strong_is_underline.push(underline);
                if underline {
                    self.format.underline = true;
                } else {
                    self.format.bold = true;
                }
            }
            Tag::Strikethrough => self.format.strikethrough = true,
            Tag::Superscript => self.format.script = Script::Sup,
            Tag::Subscript => self.format.script = Script::Sub,
            Tag::Link { link_type, dest_url, title, .. } => {
                let url = dest_url.into_string();
                self.link_urls.push(Some(url.clone()));
                let kind = match link_type {
                    LinkType::WikiLink { .. } => LinkKind::Wiki,
                    _ if url.starts_with('#') => LinkKind::Anchor,
                    _ => LinkKind::External,
                };
                let index = self.link_index(LinkTarget { url, title: title.into_string(), kind });
                self.format.link = Some(index);
            }
            Tag::Image { dest_url, title, .. } => {
                if self.table.is_none() && self.heading.is_none() {
                    self.flush();
                }
                self.image = Some(ImageContext { url: dest_url.into_string(), title: title.into_string(), alt: String::new() });
            }
            Tag::Table(alignments) => {
                self.flush();
                self.table_range = range.clone();
                let aligns = alignments
                    .iter()
                    .map(|a| match a {
                        Alignment::Center => ColumnAlign::Center,
                        Alignment::Right => ColumnAlign::Right,
                        Alignment::None | Alignment::Left => ColumnAlign::Left,
                    })
                    .collect();
                self.table = Some((Vec::new(), Vec::new(), aligns));
            }
            Tag::TableHead => self.in_head = true,
            Tag::TableRow => self.row.clear(),
            Tag::TableCell => self.prose = Prose::default(),
            _ => {}
        }
    }

    fn end(&mut self, end: TagEnd) {
        match end {
            TagEnd::Paragraph => {
                if self.table.is_none() {
                    self.flush();
                }
            }
            TagEnd::Heading { .. } => self.end_heading(),
            TagEnd::BlockQuote { .. } => {
                self.flush();
                self.quote.pop();
            }
            TagEnd::CodeBlock => {
                if let Some(text) = self.code.take() {
                    let block = Block::Code { language: std::mem::take(&mut self.code_language), text: text.trim_end_matches('\n').to_owned() };
                    self.emit(block, self.code_range.clone());
                }
            }
            TagEnd::List { .. } => {
                self.flush();
                self.lists.pop();
            }
            TagEnd::Item => self.flush(),
            TagEnd::DefinitionListTitle | TagEnd::DefinitionListDefinition => {
                self.flush();
                self.def = None;
            }
            TagEnd::FootnoteDefinition => {
                self.flush();
                if let Some(open) = self.footnote_stack.pop() {
                    let blocks = std::mem::replace(&mut self.blocks, open.blocks);
                    let ranges = std::mem::replace(&mut self.ranges, open.ranges);
                    self.footnote_defs.push(FootnoteDef { label: open.label, blocks, ranges, range: open.range });
                }
            }
            TagEnd::Emphasis => self.format.italic = false,
            TagEnd::Strong => {
                if self.strong_is_underline.pop().unwrap_or(false) {
                    self.format.underline = false;
                } else {
                    self.format.bold = false;
                }
            }
            TagEnd::Strikethrough => self.format.strikethrough = false,
            TagEnd::Superscript | TagEnd::Subscript => self.format.script = Script::None,
            TagEnd::Link => {
                self.format.link = None;
                self.link_urls.pop();
            }
            TagEnd::Image => {
                if let Some(image) = self.image.take()
                    && self.table.is_none()
                    && self.heading.is_none()
                {
                    let link = self.link_urls.last().cloned().flatten();
                    let block = Block::Image { url: image.url, alt: image.alt, title: image.title, link, quote: self.quote.clone() };
                    self.emit(block, self.unit.clone());
                }
            }
            TagEnd::TableCell => {
                let cell = std::mem::take(&mut self.prose);
                let cell = self.finish_prose(cell);
                self.row.push(cell);
            }
            TagEnd::TableHead => {
                if let Some((header, _, _)) = self.table.as_mut() {
                    *header = std::mem::take(&mut self.row);
                }
                self.in_head = false;
            }
            TagEnd::TableRow => {
                if let Some((_, rows, _)) = self.table.as_mut()
                    && !self.in_head
                {
                    rows.push(std::mem::take(&mut self.row));
                }
            }
            TagEnd::Table => {
                if let Some((header, rows, aligns)) = self.table.take() {
                    self.emit(Block::Table { header, rows, aligns }, self.table_range.clone());
                }
            }
            _ => {}
        }
    }

    fn footnote_number(&mut self, label: &str) -> usize {
        let next = self.footnote_numbers.len() + 1;
        *self.footnote_numbers.entry(label.to_owned()).or_insert(next)
    }

    fn event(&mut self, event: Event<'_>, range: Range<usize>) {
        if self.skip_depth > 0 {
            match event {
                Event::Start(_) => self.skip_depth += 1,
                Event::End(_) => self.skip_depth -= 1,
                _ => {}
            }
            return;
        }
        if let Event::Start(tag) = &event
            && let Some(is_block) = self.disabled(tag)
        {
            // Turned off: the construct's source shows as written, and its events are skipped up to its end.
            if is_block {
                self.literal_block(&range);
            } else {
                let source = self.source.get(range.clone()).unwrap_or_default();
                self.prose.push(source, InlineFormat { link: None, ..self.format });
            }
            self.skip_depth = 1;
            return;
        }
        match event {
            Event::Start(tag) => self.start(tag, &range),
            Event::End(end) => self.end(end),
            Event::Text(text) => match self.code.as_mut() {
                Some(code) => code.push_str(&text),
                None => self.push_text(&text),
            },
            Event::Code(text) if !self.config.features.inline_code => {
                let _ = text;
                let source = self.source.get(range).unwrap_or_default();
                self.prose.push(source, self.format);
            }
            Event::Code(text) => self.prose.push(&text, InlineFormat { code: true, ..self.format }),
            Event::InlineMath(tex) => {
                let hooked = self.config.math_hook.as_ref().and_then(|hook| (hook.0)(&tex, false));
                let math = InlineFormat { math: true, ..self.format };
                match (hooked, self.config.math_mode) {
                    (Some(MathRender::Text(text)), _) => self.prose.push(&text, math),
                    (_, MathMode::Source) => self.prose.push(&tex, math),
                    (_, MathMode::Unicode) => {
                        let text = tex::tex_to_unicode(&tex).unwrap_or_else(|| tex.to_string());
                        self.prose.push(&text, math);
                    }
                    // Inline math sits in a text run, which cannot hold boxes, but it can hold raised and lowered runs.
                    (_, MathMode::Layout) => match tex::parse(&tex) {
                        Ok(node) => {
                            for (text, level) in tex::to_runs(&node) {
                                let sup = level == tex::ScriptLevel::Superscript;
                                let mapped = (self.config.unicode_scripts && level != tex::ScriptLevel::Normal).then(|| to_script(&text, sup)).flatten();
                                let script = match level {
                                    tex::ScriptLevel::Normal => math.script,
                                    tex::ScriptLevel::Superscript => Script::Sup,
                                    tex::ScriptLevel::Subscript => Script::Sub,
                                };
                                match mapped {
                                    Some(mapped) => self.prose.push(&mapped, math),
                                    None => self.prose.push(text.trim_start_matches('\u{2009}'), InlineFormat { script, ..math }),
                                }
                            }
                        }
                        Err(_) => self.prose.push(&tex, math),
                    },
                }
            }
            Event::DisplayMath(tex) => {
                self.flush();
                let render = self.config.math_hook.as_ref().and_then(|hook| (hook.0)(&tex, true));
                let expr = (self.config.math_mode == MathMode::Layout).then(|| tex::parse(&tex).ok()).flatten();
                let render = match (render, self.config.math_mode) {
                    (None, MathMode::Unicode) => tex::tex_to_unicode(&tex).map(MathRender::Text),
                    (render, _) => render,
                };
                self.emit(Block::Math { tex: tex.into_string(), render, expr }, range);
            }
            Event::FootnoteReference(label) => {
                let label = label.into_string();
                let number = self.footnote_number(&label);
                let first = !self.prose.anchors.iter().any(|anchor| *anchor == format!("fnref:{label}"));
                if first {
                    self.prose.anchors.push(format!("fnref:{label}"));
                }
                let shown = to_script(&number.to_string(), true).unwrap_or_else(|| format!("[{number}]"));
                let index = self.link_index(LinkTarget { url: format!("fn:{label}"), title: String::new(), kind: LinkKind::Footnote });
                self.prose.push(&shown, InlineFormat { link: Some(index), ..self.format });
            }
            Event::SoftBreak => {
                let text = if self.config.soft_break == SoftBreak::Newline { "\n" } else { " " };
                self.push_text(text);
            }
            Event::HardBreak => self.prose.push("\n", self.format),
            Event::Rule if !self.config.features.rules => self.literal_block(&range),
            Event::Rule => {
                self.flush();
                self.emit(Block::Rule, range);
            }
            Event::TaskListMarker(done) => {
                let index = self.task_ranges.len();
                self.task_ranges.push(range);
                if self.config.task_glyphs {
                    self.prose.push(if done { "\u{2611} " } else { "\u{2610} " }, self.format);
                } else if let Some(item) = self.item.as_mut() {
                    item.task = Some(TaskItem { index, checked: done });
                }
            }
            // Raw HTML is dropped.
            _ => {}
        }
    }

    /// Appends the footnote list, numbered by first reference.
    fn footnotes(&mut self) {
        let mut defs = std::mem::take(&mut self.footnote_defs);
        for def in &defs {
            self.footnote_number(&def.label);
        }
        defs.sort_by_key(|def| self.footnote_numbers.get(&def.label).copied().unwrap_or(usize::MAX));
        if defs.is_empty() {
            return;
        }
        self.emit(Block::Rule, 0..0);
        for FootnoteDef { label, blocks, ranges, range } in defs {
            let number = self.footnote_numbers.get(&label).copied().unwrap_or(0);
            let mut inner = blocks.into_iter().zip(ranges);
            let (mut prose, first_other) = match inner.next() {
                Some((Block::Paragraph { prose, .. }, _)) => (prose, None),
                Some(other) => (Prose::default(), Some(other)),
                None => (Prose::default(), None),
            };
            prose.anchors.push(format!("fn:{label}"));
            let index = prose.links.len();
            prose.links.push(LinkTarget { url: format!("fnref:{label}"), title: String::new(), kind: LinkKind::FootnoteBack });
            prose.push(" ", InlineFormat::default());
            prose.push("\u{21a9}", InlineFormat { link: Some(index), ..InlineFormat::default() });
            self.emit(Block::Footnote { number, label, prose }, range);
            for (block, range) in first_other.into_iter().chain(inner) {
                self.emit(block, range);
            }
        }
    }

    fn finish(mut self) -> Parsed {
        self.flush();
        self.footnotes();
        Parsed { blocks: self.blocks, ranges: self.ranges, task_ranges: self.task_ranges, lists: self.list_infos }
    }
}

/// Parses Markdown into blocks.
pub(super) fn parse_with(source: &str, config: &ParseConfig) -> Parsed {
    let f = &config.features;
    let mut options = Options::empty();
    for (on, option) in [
        (f.tables, Options::ENABLE_TABLES),
        (f.strikethrough, Options::ENABLE_STRIKETHROUGH),
        (f.task_lists, Options::ENABLE_TASKLISTS),
        (f.footnotes, Options::ENABLE_FOOTNOTES),
        (f.definition_lists, Options::ENABLE_DEFINITION_LIST),
        (f.math, Options::ENABLE_MATH),
        (f.superscript, Options::ENABLE_SUPERSCRIPT),
        (f.subscript, Options::ENABLE_SUBSCRIPT),
        (f.heading_attributes, Options::ENABLE_HEADING_ATTRIBUTES),
        (f.alerts, Options::ENABLE_GFM),
        (f.wikilinks, Options::ENABLE_WIKILINKS),
        (config.smart_punctuation, Options::ENABLE_SMART_PUNCTUATION),
    ] {
        if on {
            options |= option;
        }
    }
    let mut builder = Builder::new(source, config);
    for (event, range) in Parser::new_ext(source, options).into_offset_iter() {
        builder.event(event, range);
    }
    builder.finish()
}

/// Which heading anchors a document defines (for tests and [`MarkdownView::headings`](super::MarkdownView::headings)).
pub(super) fn anchors_of(blocks: &[Block]) -> HashSet<String> {
    let mut anchors = HashSet::new();
    let mut add = |prose: &Prose| anchors.extend(prose.anchors.iter().cloned());
    for block in blocks {
        match block {
            Block::Paragraph { prose, .. }
            | Block::Heading { prose, .. }
            | Block::ListItem { prose, .. }
            | Block::DefTerm { prose }
            | Block::DefDetail { prose }
            | Block::Footnote { prose, .. } => add(prose),
            Block::Table { header, rows, .. } => header.iter().chain(rows.iter().flatten()).for_each(&mut add),
            _ => {}
        }
    }
    anchors
}
