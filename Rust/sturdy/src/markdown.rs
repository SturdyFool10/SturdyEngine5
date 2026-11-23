//! Markdown rendered with the engine's UI: changelogs, mod descriptions, chat messages, README-style text.
//!
//! [`MarkdownView`] parses once and draws the document each frame as a column of blocks. Every block of prose is a
//! [`Ui::rich_text`](crate::ui::Ui::rich_text) block, so text wraps with the layout, is selectable and copyable, and links report clicks:
//!
//! ```ignore
//! // once, or whenever the text changes:
//! let mut view = MarkdownView::new(&changelog);
//! // each frame, inside the UI hook:
//! for event in view.show(ui, "changelog", &style) {
//!     match event {
//!         MarkdownEvent::LinkClicked(url) => open_in_browser(&url),
//!         MarkdownEvent::ImageRequested { url, .. } => start_loading(&url),
//!         _ => {}
//!     }
//! }
//! ```
//!
//! Supported: CommonMark; tables (with column alignment); strikethrough; task lists (real check boxes); footnotes; definition lists;
//! math (`$x$`, `$$x$$`, with a [`MathHook`]); superscript and subscript; heading attributes (`{#id .class}`); smart punctuation
//! (opt-in); GitHub alerts (`> [!NOTE]`); wiki links; underline (`__text__`, opt-in); images (the application supplies the textures);
//! bare URLs; nested quotes. Raw HTML is dropped. See `docs/markdown.md` in this crate for every construct and every
//! [`MarkdownStyle`] field, and what the engine's text blocks cannot do yet.
//!
//! Bold, italic and monospace runs use the font ids in [`MarkdownStyle`]; register those faces (for example with
//! [`crate::Engine::register_ui_font_stack`]) or leave them at 0 to draw everything in the regular face.

mod editor;
mod highlight;
mod inline;
mod live;
mod parse;
pub mod tex;
mod style;
mod view;

pub use editor::{EditorOutput, MarkdownEditor, RevealPolicy};
pub use highlight::{MarkdownHighlighter, LANGUAGES};
pub use live::{live_spans, shift_spans_for_insertion};
pub use inline::{inline_text, InlineLink, InlineText, MarkdownLinkKind, MarkdownViews};
pub use style::{
    AlertColours, CodeColorizer, CodeColours, CodeSpan, ImageHandle, LineStyle, LinkUnderline, MarkdownFeatures, MarkdownImage, MarkdownResources,
    MarkdownStyle, MathHook, MathMode, MathRender, NumberFormat, SoftBreak,
};
pub use view::MarkdownView;

/// Something the reader did in a [`MarkdownView`] this frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MarkdownEvent {
    /// A link was clicked; open it yourself. Includes `#anchor` links and a link around an image.
    LinkClicked(String),
    /// The document wants an image the application has not supplied (once per URL). Load it and put the result in
    /// [`MarkdownResources::images`]. `id` is the image's element id.
    ImageRequested { url: String, id: String },
    /// A picture that is not inside a link was clicked.
    ImageClicked(String),
    /// A task list check box was toggled. `block` is the block's index in the document, `item` the task item's ordinal (see
    /// [`MarkdownView::task_source_range`] to rewrite the source). The view shows the new state until the next `set_source`.
    TaskToggled { block: usize, item: usize, checked: bool },
    /// The copy button of a code block was clicked; put the text on the clipboard.
    CopyCode(String),
    /// A `[[wiki link]]` was clicked: the page name.
    WikiLinkClicked(String),
    /// A footnote reference, footnote back-link or `#heading` link was followed: the anchor name (`fn:1`, `fnref:1`, `my-heading`). The
    /// view has already scrolled to it when [`MarkdownResources::scroll_container`] is set.
    AnchorClicked(String),
    /// A text block was clicked (only with [`MarkdownResources::report_block_clicks`]). `block` indexes [`MarkdownView::blocks`];
    /// `char_index` counts characters of the block's plain text ([`BlockInfo::text`]) - [`MarkdownView::source_offset`] turns it into a
    /// position in the Markdown source. Link clicks are reported as well.
    BlockClicked { block: usize, char_index: usize },
    /// The pointer moved onto a link (its URL) or off the links (`None`).
    LinkHovered(Option<String>),
    /// A link was right-clicked: open your context menu at (`x`, `y`), the pointer position in layout pixels.
    LinkContext { url: String, x: i32, y: i32 },
    /// Ctrl+C with a selection that spans several blocks (a drag from one paragraph into another): put the text on the clipboard.
    /// A selection inside one block is copied by the block itself.
    CopySelection(String),
}

/// What kind of block a [`BlockInfo`] describes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockKind {
    Paragraph,
    /// Level 1 to 6.
    Heading(u8),
    /// One list item (nested items are separate blocks).
    ListItem,
    Code,
    Rule,
    Table,
    Image,
    /// The title line of a `> [!NOTE]` alert.
    AlertTitle,
    DefinitionTerm,
    DefinitionDetail,
    /// Display math.
    Math,
    Footnote,
}

/// One block of a parsed document, as an application (an editor, a table of contents, a search index) sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlockInfo {
    /// Position in [`MarkdownView::blocks`].
    pub index: usize,
    pub kind: BlockKind,
    /// The lines of the Markdown source the block was written in (byte range, from the start of its first line to the end of its last).
    /// Empty for a block the renderer added (the rule above the footnotes). The pieces of a paragraph that an image splits share one range,
    /// and a block quote's blocks keep the `> ` of their first line inside it.
    pub source: std::ops::Range<usize>,
    /// The block's text without markup.
    pub text: String,
    /// The ordinal of the block's task-list box, for [`MarkdownView::task_source_range`].
    pub task: Option<usize>,
}

/// A heading of the document, for building a table of contents.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MarkdownHeading {
    pub level: u8,
    pub text: String,
    /// The explicit `{#id}`, or a GitHub-style slug of the text; `None` for a heading without text. Pass it to
    /// [`MarkdownView::scroll_to`].
    pub id: Option<String>,
}

#[cfg(test)]
mod tests;
