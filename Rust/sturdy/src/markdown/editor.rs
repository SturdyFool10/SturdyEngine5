//! A live-preview Markdown editor in the style of Obsidian: the document is drawn rendered, and the block being edited turns into its
//! source in an editable text area, so the control symbols (`#`, `**`, `>`, fences) show only where the app wants them.
//!
//! [`MarkdownEditor`] is the ready-made version (click a block to edit it, click away to render it again). It is built from public
//! pieces you can use differently: [`MarkdownView::blocks`] (kind, source lines and plain text of every block),
//! [`MarkdownView::show_blocks`] (a callback that may draw any block itself), [`MarkdownEvent::BlockClicked`], and
//! [`MarkdownView::source_offset`] (where in the source a click landed). For example, to reveal the source of the block under the caret
//! of a single big text area instead, keep your own caret, find its block with [`MarkdownView::block_at_offset`] and replace just that block.

use std::ops::Range;

use super::style::{MarkdownResources, MarkdownStyle};
use super::view::MarkdownView;
use super::{BlockInfo, MarkdownEvent};
use crate::ui::{ElementDesc, Sizing, Ui};
use super::live::live_spans;
use super::style::MarkdownStyle as LiveStyle;
use crate::widgets::{RichSpan, ScrollAreaState, ScrollbarStyle, ScrollbarVisibility, TextEditState, TextInputStyle};

struct Active {
    /// The source lines being edited (byte range of the whole document's source).
    range: Range<usize>,
    state: TextEditState,
    scroll: ScrollAreaState,
    focus: bool,
}

/// What [`MarkdownEditor::show`] did this frame.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EditorOutput {
    /// What the reader did in the rendered blocks (links clicked, tasks toggled...). [`MarkdownEvent::BlockClicked`] is included.
    pub events: Vec<MarkdownEvent>,
    /// The source text changed this frame: save [`MarkdownEditor::source`].
    pub changed: bool,
    /// A block started or stopped being edited, so the next frame looks different. With on-demand rendering ask for another frame.
    pub needs_redraw: bool,
}

/// A Markdown document that renders itself and edits one block at a time.
pub struct MarkdownEditor {
    view: MarkdownView,
    active: Option<Active>,
    live: Option<Live>,
    reveal: RevealPolicy,
}

/// A custom reveal decision: the editor's view and the caret's byte offset in, byte ranges of the source out.
pub type RevealFn = Box<dyn FnMut(&MarkdownView, usize) -> Vec<Range<usize>> + Send>;

/// Which markup a live-preview editor ([`MarkdownEditor::show_live`]) shows. Everything else is hidden (the characters stay in the text).
pub enum RevealPolicy {
    /// The whole block (paragraph, heading, list item, code block...) the caret is in. The default, like Obsidian.
    ActiveBlock,
    /// Only the line the caret is on.
    ActiveLine,
    /// All markup: the source view.
    All,
    /// No markup: the preview alone.
    Nothing,
    /// Decided by the app: given the editor's view and the caret's byte offset in the source, the byte ranges whose markup shows.
    Custom(RevealFn),
}

/// The editable text of the live-preview mode and what was last computed for it.
struct Live {
    state: TextEditState,
    scroll: ScrollAreaState,
    spans: Vec<RichSpan>,
    key: Option<(String, Vec<Range<usize>>, bool, MarkdownStyle)>,
}

impl MarkdownEditor {
    pub fn new(source: &str) -> Self {
        Self { view: MarkdownView::new(source), active: None, live: None, reveal: RevealPolicy::ActiveBlock }
    }

    pub fn with_style(source: &str, style: &MarkdownStyle) -> Self {
        Self { view: MarkdownView::with_style(source, style), active: None, live: None, reveal: RevealPolicy::ActiveBlock }
    }

    pub fn source(&self) -> &str {
        self.view.source()
    }

    /// Replaces the document and stops editing.
    pub fn set_source(&mut self, source: &str) {
        self.active = None;
        if let Some(live) = self.live.as_mut() {
            live.state.set_text(source);
            live.key = None;
        }
        self.view.set_source(source);
    }

    /// The rendered document: blocks, plain text, links, headings, `scroll_to`.
    pub fn view(&self) -> &MarkdownView {
        &self.view
    }

    pub fn view_mut(&mut self) -> &mut MarkdownView {
        &mut self.view
    }

    pub fn is_editing(&self) -> bool {
        self.active.is_some()
    }

    /// The source range being edited, if any.
    pub fn editing_range(&self) -> Option<Range<usize>> {
        self.active.as_ref().map(|active| active.range.clone())
    }

    /// Starts editing block `block`'s source lines, with the caret at `caret` (a byte offset in the source) or at the end.
    /// `false` when there is no such block or it has no source (the rule above the footnotes).
    pub fn edit_block(&mut self, block: usize, caret: Option<usize>) -> bool {
        let Some(info) = self.view.blocks().get(block) else { return false };
        if info.source.is_empty() {
            return false;
        }
        let range = info.source.clone();
        self.edit_range(range, caret);
        true
    }

    /// Starts editing the given lines of the source (widened to whole lines). Use it to begin a new block: `edit_range(len..len, None)`
    /// after appending a newline.
    pub fn edit_range(&mut self, range: Range<usize>, caret: Option<usize>) {
        let source = self.view.source();
        let range = super::view::expand_to_lines(source, &range);
        let text = source.get(range.clone()).unwrap_or_default().to_owned();
        let at = caret.map_or_else(|| text.chars().count(), |offset| source.get(range.start..offset.clamp(range.start, range.end)).map_or(0, |head| head.chars().count()));
        let mut state = TextEditState::with_text(&text);
        state.select_range(at, at);
        self.active = Some(Active { range, state, scroll: ScrollAreaState::new(), focus: true });
    }

    /// Starts editing the block containing byte `offset` of the source.
    pub fn edit_at(&mut self, offset: usize) -> bool {
        match self.view.block_at_offset(offset) {
            Some(block) => self.edit_block(block, Some(offset)),
            None => false,
        }
    }

    /// Stops editing: the block is drawn rendered again.
    pub fn stop_editing(&mut self) {
        self.active = None;
    }

    /// Draws the document. `id` must be unique in the frame. Clicking a rendered block starts editing it (the click position becomes the
    /// caret), clicking outside the editor stops. `resources.report_block_clicks` is switched on for the call.
    pub fn show(&mut self, ui: &mut Ui<'_>, id: &str, style: &MarkdownStyle, resources: &mut MarkdownResources) -> EditorOutput {
        let editor_id = format!("{id}::editor");
        let was_reporting = std::mem::replace(&mut resources.report_block_clicks, true);
        let mut drawn = false;
        let mut new_text: Option<String> = None;
        let events = {
            let active = &mut self.active;
            self.view.show_blocks(ui, id, style, resources, &mut |ui: &mut Ui<'_>, info: &BlockInfo| {
                let Some(active) = active.as_mut() else { return false };
                // A block belongs to the editor when its source lines lie inside the lines being edited.
                let inside = !info.source.is_empty() && info.source.start >= active.range.start && info.source.end <= active.range.end;
                if inside && !drawn {
                    drawn = true;
                    new_text = draw_editor(ui, &editor_id, style, active);
                }
                inside
            })
        };
        resources.report_block_clicks = was_reporting;
        if !drawn && let Some(active) = self.active.as_mut() {
            // Nothing in the document is inside the edited lines (a new or emptied block): the editor still has to be reachable.
            new_text = draw_editor(ui, &editor_id, style, active);
        }

        let mut output = EditorOutput { events, ..EditorOutput::default() };
        if let Some(text) = new_text
            && let Some(active) = self.active.as_mut()
        {
            let mut source = self.view.source().to_owned();
            if source.get(active.range.clone()).is_some() {
                source.replace_range(active.range.clone(), &text);
                active.range.end = active.range.start + text.len();
                self.view.set_source(&source);
                output.changed = true;
            }
        }

        let click = output.events.iter().find_map(|event| match event {
            MarkdownEvent::BlockClicked { block, char_index } => Some((*block, *char_index)),
            _ => None,
        });
        match click {
            Some((block, char_index)) => {
                let editing_it = self.view.blocks().get(block).zip(self.active.as_ref()).is_some_and(|(info, active)| {
                    !info.source.is_empty() && info.source.start >= active.range.start && info.source.end <= active.range.end
                });
                if !editing_it {
                    let caret = self.view.source_offset(block, char_index);
                    output.needs_redraw = self.edit_block(block, caret);
                }
            }
            None => {
                if self.active.is_some() && drawn_last_frame(ui, &editor_id) && ui.clicked_outside(&editor_id) {
                    self.active = None;
                    output.needs_redraw = true;
                }
            }
        }
        output
    }
}

impl MarkdownEditor {
    /// Chooses which markup [`MarkdownEditor::show_live`] shows.
    pub fn set_reveal(&mut self, policy: RevealPolicy) {
        self.reveal = policy;
        if let Some(live) = self.live.as_mut() {
            live.key = None;
        }
    }

    /// Draws the whole document as one editable text, styled like the rendered Markdown: headings large, bold and italic runs, coloured
    /// links, code in the mono font with syntax colours, quotes muted. The markup characters (`#`, `**`, `[`, `](url)`, fences, `>`) are
    /// hidden except where [`RevealPolicy`] says to show them (by default the block the caret is in). It is real text editing: caret,
    /// selection, clipboard and undo work on the source. Images, tables and display math show as their source here (use
    /// [`MarkdownEditor::show`] for the block-by-block mode that renders them).
    pub fn show_live(&mut self, ui: &mut Ui<'_>, id: &str, style: &LiveStyle) -> EditorOutput {
        let source = self.view.source().to_owned();
        let live = self.live.get_or_insert_with(|| Live {
            state: TextEditState::with_text(&source),
            scroll: ScrollAreaState::new(),
            spans: Vec::new(),
            key: None,
        });
        if live.state.text() != source {
            live.state.set_text(&source);
        }
        // Which source ranges show their markup this frame.
        let caret_byte = source.char_indices().nth(live.state.caret()).map_or(source.len(), |(byte, _)| byte);
        let (selection_start, selection_end) = live.state.selection();
        let mut ranges: Vec<Range<usize>> = match &mut self.reveal {
            RevealPolicy::ActiveBlock => {
                let mut found: Vec<Range<usize>> = self
                    .view
                    .block_at_offset(caret_byte)
                    .and_then(|block| self.view.blocks().get(block))
                    .map(|info| vec![info.source.clone()])
                    .unwrap_or_default();
                if found.is_empty() {
                    found.push(super::view::expand_to_lines(&source, &(caret_byte..caret_byte)));
                }
                found
            }
            RevealPolicy::ActiveLine => vec![super::view::expand_to_lines(&source, &(caret_byte..caret_byte))],
            RevealPolicy::All | RevealPolicy::Nothing => Vec::new(),
            RevealPolicy::Custom(decide) => decide(&self.view, caret_byte),
        };
        if selection_end > selection_start && !matches!(self.reveal, RevealPolicy::Nothing | RevealPolicy::All) {
            let to_byte = |chars: usize| source.char_indices().nth(chars).map_or(source.len(), |(byte, _)| byte);
            ranges.push(super::view::expand_to_lines(&source, &(to_byte(selection_start)..to_byte(selection_end))));
        }
        let reveal_all = matches!(self.reveal, RevealPolicy::All);
        let stale = live.key.as_ref().is_none_or(|(text, known, all, known_style)| {
            *text != source || *known != ranges || *all != reveal_all || known_style != style
        });
        if stale {
            live.spans = live_spans(&source, style, &ranges, reveal_all);
            live.key = Some((source.clone(), ranges, reveal_all, style.clone()));
        }

        let desc = ElementDesc::new().id(format!("{id}::live")).width(Sizing::Grow).height(Sizing::Fit).height_min(f32::from(style.font_size) * 2.0);
        let text_style = TextInputStyle {
            idle: glam::Vec4::ZERO,
            hovered: glam::Vec4::ZERO,
            focused: glam::Vec4::ZERO,
            disabled: glam::Vec4::ZERO,
            border_idle: glam::Vec4::ZERO,
            border_focused: glam::Vec4::ZERO,
            text_color: style.text,
            caret_color: style.text,
            corner_radius: 0.0,
            font_id: style.regular_font,
            font_size: style.font_size,
            wrap_lines: true,
            letter_spacing: style.letter_spacing,
            line_height: style.line_height,
            ..TextInputStyle::default()
        };
        // The frame in which text is typed draws before the styling can follow the new text: line the old spans up with it.
        let typed = ui.text_input().typed_text.chars().count();
        let drawn_spans;
        let spans: &[RichSpan] = if typed > 0 && live.state.focused() && !live.state.has_selection() {
            drawn_spans = super::live::shift_spans_for_insertion(&live.spans, live.state.caret(), typed);
            &drawn_spans
        } else {
            &live.spans
        };
        let result = ui.rich_text_edit(
            &desc,
            &text_style,
            &ScrollbarStyle { visibility: ScrollbarVisibility::AlwaysHidden, ..ScrollbarStyle::default() },
            &mut live.state,
            &mut live.scroll,
            spans,
            "",
            true,
        );
        let mut output = EditorOutput::default();
        if result.changed {
            let text = live.state.text();
            self.view.set_source(&text);
            output.changed = true;
            // The styling follows the text one frame late: draw that frame at once.
            output.needs_redraw = true;
            ui.engine().request_redraw();
        }
        output
    }
}

fn drawn_last_frame(ui: &Ui<'_>, editor_id: &str) -> bool {
    ui.element_bounds(editor_id).is_some()
}

/// Draws the text area for the block being edited; the new text when it changed this frame.
fn draw_editor(ui: &mut Ui<'_>, editor_id: &str, style: &MarkdownStyle, active: &mut Active) -> Option<String> {
    let desc = ElementDesc::new().id(editor_id.to_owned()).width(Sizing::Grow).height(Sizing::Fit).height_min(f32::from(style.font_size) * 2.0);
    let text_style = TextInputStyle {
        idle: style.code_background,
        hovered: style.code_background,
        focused: style.code_background,
        border_idle: style.rule,
        border_focused: style.link,
        text_color: style.text,
        font_id: style.mono_font,
        font_size: style.font_size,
        corner_radius: style.code_corner_radius,
        wrap_lines: true,
        ..TextInputStyle::default()
    };
    if active.focus {
        active.state.set_focused(true);
        ui.focus(editor_id);
        active.focus = false;
    }
    let result = ui.text_area(
        &desc,
        &text_style,
        &ScrollbarStyle { visibility: ScrollbarVisibility::AlwaysHidden, ..ScrollbarStyle::default() },
        &mut active.state,
        &mut active.scroll,
        "",
        true,
    );
    result.changed.then(|| active.state.text())
}
