//! Markdown for places that are not a document: a label, a tooltip, a button, a chat bubble of your own. [`inline_text`] turns a string
//! into text plus [`RichSpan`]s you hand to [`Ui::rich_text`](crate::ui::Ui::rich_text) (or any widget that takes spans), and
//! [`MarkdownViews`] keeps one [`MarkdownView`] per key so hundreds of messages cost a lookup each.

use std::collections::HashMap;

use super::parse::{parse_with, Block, LinkKind, ParseConfig, Prose};
use super::style::{MarkdownFeatures, MarkdownResources, MarkdownStyle};
use super::view::{spans_of, MarkdownView};
use super::MarkdownEvent;
use crate::ui::Ui;
use crate::widgets::RichSpan;

/// What a link does when clicked.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MarkdownLinkKind {
    /// A URL for the application to open.
    External,
    /// `[[Page]]`.
    Wiki,
    /// `[text](#heading)`.
    Anchor,
    /// A footnote reference.
    Footnote,
    /// The arrow back from a footnote.
    FootnoteBack,
}

impl From<LinkKind> for MarkdownLinkKind {
    fn from(kind: LinkKind) -> Self {
        match kind {
            LinkKind::External => Self::External,
            LinkKind::Wiki => Self::Wiki,
            LinkKind::Anchor => Self::Anchor,
            LinkKind::Footnote => Self::Footnote,
            LinkKind::FootnoteBack => Self::FootnoteBack,
        }
    }
}

/// A link in an [`InlineText`]: `start`/`len` count characters, like [`RichSpan`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InlineLink {
    pub start: usize,
    pub len: usize,
    pub url: String,
    pub title: String,
    pub kind: MarkdownLinkKind,
}

/// A line of Markdown turned into text and styled ranges.
#[derive(Debug, Clone, PartialEq)]
pub struct InlineText {
    /// The text without markup (`**bold**` is `bold`).
    pub text: String,
    /// Fonts, colours, underline and strikethrough per run, in the style's terms.
    pub spans: Vec<RichSpan>,
    pub links: Vec<InlineLink>,
}

impl InlineText {
    /// The link under the `char_index`-th character (a click position from [`Ui::rich_text`](crate::ui::Ui::rich_text)).
    pub fn link_at(&self, char_index: usize) -> Option<&InlineLink> {
        self.links.iter().find(|link| char_index >= link.start && char_index < link.start + link.len)
    }
}

/// `source` as inline Markdown: emphasis, strikethrough, code, links, bare URLs, math (as Unicode text), sub/superscript, footnote
/// references and wiki links, as far as `style.features` allows them. Block constructs (headings, lists, quotes, code blocks, tables,
/// images...) are never interpreted here: they stay literal. Paragraphs are joined by a newline.
pub fn inline_text(source: &str, style: &MarkdownStyle) -> InlineText {
    let f = style.features;
    let features = MarkdownFeatures {
        headings: false,
        heading_attributes: false,
        images: false,
        tables: false,
        task_lists: false,
        lists: false,
        block_quotes: false,
        alerts: false,
        code_blocks: false,
        rules: false,
        definition_lists: false,
        footnotes: false,
        ..f
    };
    let config = ParseConfig { features, ..ParseConfig::from_style(style) };
    let parsed = parse_with(source, &config);
    let mut prose = Prose::default();
    for block in &parsed.blocks {
        if let Block::Paragraph { prose: part, .. } = block {
            if !prose.text.is_empty() {
                prose.push("\n", Default::default());
            }
            prose.append(part);
        }
    }
    let links = prose
        .runs
        .iter()
        .filter_map(|(start, len, format)| {
            let target = format.link.and_then(|index| prose.links.get(index))?;
            Some(InlineLink { start: *start, len: *len, url: target.url.clone(), title: target.title.clone(), kind: target.kind.into() })
        })
        .collect();
    InlineText { spans: spans_of(&prose, style), text: prose.text, links }
}

struct Entry {
    view: MarkdownView,
    used: u64,
}

/// One [`MarkdownView`] per key, created on first use, re-parsed only when its text changes, and dropped least-recently-used once there
/// are more than `capacity`. The shape for a chat: `views.show(ui, &message.id, &message.text, &style, &resources)` for every visible message.
pub struct MarkdownViews {
    entries: HashMap<String, Entry>,
    clock: u64,
    capacity: usize,
    /// The column width the views last had; new views wrap to it from their first frame.
    width: f32,
}

impl Default for MarkdownViews {
    fn default() -> Self {
        Self::new(512)
    }
}

impl MarkdownViews {
    pub fn new(capacity: usize) -> Self {
        Self { entries: HashMap::new(), clock: 0, capacity: capacity.max(1), width: 0.0 }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Draws the document `source` under `key` (which is also its element id, so it must be unique in the frame).
    pub fn show(&mut self, ui: &mut Ui<'_>, key: &str, source: &str, style: &MarkdownStyle, resources: &MarkdownResources) -> Vec<MarkdownEvent> {
        let width = self.width;
        let view = self.view_for(key, source, style);
        view.set_width_hint(width);
        let events = view.show_with(ui, key, style, resources);
        if let Some(learnt) = view.last_width() {
            self.width = learnt;
        }
        events
    }

    /// The view for `key` holding `source`, creating or updating it. Use it to call [`MarkdownView::plain_text`], `scroll_to` and the
    /// rest, or to draw with [`MarkdownView::show_blocks`].
    pub fn view_for(&mut self, key: &str, source: &str, style: &MarkdownStyle) -> &mut MarkdownView {
        self.clock += 1;
        let clock = self.clock;
        if !self.entries.contains_key(key) {
            if self.entries.len() >= self.capacity {
                self.evict();
            }
            self.entries.insert(key.to_owned(), Entry { view: MarkdownView::with_style(source, style), used: clock });
        }
        let entry = self.entries.entry(key.to_owned()).or_insert_with(|| Entry { view: MarkdownView::with_style(source, style), used: clock });
        entry.used = clock;
        if entry.view.source() != source {
            entry.view.set_source(source);
        }
        &mut entry.view
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut MarkdownView> {
        self.entries.get_mut(key).map(|entry| &mut entry.view)
    }

    pub fn remove(&mut self, key: &str) -> bool {
        self.entries.remove(key).is_some()
    }

    pub fn clear(&mut self) {
        self.entries.clear();
    }

    fn evict(&mut self) {
        // A tenth of the capacity at once, so a long scroll does not evict on every frame.
        let drop_count = (self.capacity / 10).max(1);
        let mut by_age: Vec<(u64, String)> = self.entries.iter().map(|(key, entry)| (entry.used, key.clone())).collect();
        by_age.sort_unstable();
        for (_, key) in by_age.into_iter().take(drop_count) {
            self.entries.remove(&key);
        }
    }
}
