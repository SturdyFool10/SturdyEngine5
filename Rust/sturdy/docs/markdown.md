# `sturdy::markdown`

`MarkdownView` parses Markdown once and draws it every frame as a column of blocks with the engine's UI. Prose blocks are selectable
`Ui::rich_text` blocks. A demo document that uses everything below is `examples/shell_demo/src/markdown_demo.md` (the Home tab).

```rust
let mut view = MarkdownView::new(&text);                 // or MarkdownView::with_style(&text, &style)
let resources = MarkdownResources { scroll_container: Some("chat-scroll".into()), ..Default::default() };
for event in view.show_with(ui, "message-17", &style, &resources) { /* see Events */ }
```

`show(ui, id, &style)` is `show_with` with default resources. `id` must be unique per view per frame.

## Supported constructs

| Construct | Syntax | Notes |
|---|---|---|
| Headings | `# H1` .. `###### H6` | size from `heading_scale`, font `heading_font` (else `bold_font`) |
| Heading attributes | `# Title {#id .class}` | the id is kept (`headings()`, `scroll_to`); headings without one get a GitHub-style slug (`getting-started`, `-1` suffix for repeats) |
| Emphasis | `*i*` `**b**` `***bi***` `~~s~~` `` `code` `` | |
| Underline | `__text__` | **opt-in** `underline_syntax`; otherwise bold. Emulated: pulldown-cmark 0.13 has no underline option |
| Superscript / subscript | `^sup^` `~sub~` | the markers must be delimited by whitespace or punctuation (`x ^2^`, not `x^2^`; a pulldown-cmark rule). Written with Unicode super/subscript characters when every character has one (`unicode_scripts`), else drawn at `script_scale` size. No vertical offset: see Engine limits |
| Links | `[t](url "title")` | titles are kept but there is no hover tooltip (Engine limits) |
| Bare URLs | `https://..` `http://..` `www...` | `autolink` (on). Trailing `. , ; : ! ? ) * _ ~ ' "` trimmed (a `)` only if unbalanced). Not inside code, math or existing links. `www.` links get `http://` |
| Wiki links | `[[Page]]` `[[Page\|label]]` | `WikiLinkClicked(page)` |
| Footnotes | `text[^1]` / `[^1]: note` | superscript number linking to the list at the end (numbered by first reference, after a rule), each with a `↩` back-link. Clicking scrolls (`scroll_container`) and reports `AnchorClicked("fn:1")` / `("fnref:1")` |
| Definition lists | `Term` / `: definition` | term in `bold_font`, details indented `definition_indent` |
| Math | `$x$` `$$x$$` | LaTeX subset (see Math below). Inline math becomes Unicode text; display math is laid out as boxes (`math_mode`). `math_hook` can replace it with text or (display only) an image |
| Smart punctuation | `"q"` `--` `...` | **opt-in** `smart_punctuation` |
| Alerts | `> [!NOTE]` `TIP` `IMPORTANT` `WARNING` `CAUTION` | quote with a bar in `alert_colours` and a coloured title line |
| Block quotes | `> a` `> > b` | one bar and one `quote_padding` indent per level, bars continuous through the gaps |
| Lists | `-` `*` `+` `1.` | bullet per level from `bullets`; numbers via `number_format`/`number_suffix`; markers right-aligned in a column as wide as the list's widest |
| Task lists | `- [ ]` `- [x]` | real check boxes (`task_glyphs` for the old ☑/☐ characters); `task_read_only`; `TaskToggled` |
| Tables | pipes, `:---` `:---:` `---:` | per-column alignment; header background, zebra rows, padding, border colour |
| Code blocks | fenced or indented | optional built-in highlighter (languages below), language label, copy button, line numbers, max height with scroll |
| Rules | `---` | |
| Images | `![alt](url "title")` | the app supplies the texture (below); alt text until it arrives or on failure; an image inside a link reports the link. Inside table cells or headings an image stays its alt text |
| Raw HTML | | dropped, text between tags stays |

Built-in highlighter languages (`MarkdownHighlighter`, fence aliases in `LANGUAGES`): Rust, C, C++, C#, Java, JavaScript, TypeScript,
Python, Go, JSON, TOML, YAML, shell, SQL, HTML/XML, CSS, Markdown. It is a lexer: keywords, strings, comments, numbers, types (and keys in
JSON/YAML/TOML), calls, punctuation. Unknown languages are left plain.

Not handled (the application pre-processes): spoilers `||x||`, emoji `:shortcodes:`.

## Events

| Event | When |
|---|---|
| `LinkClicked(url)` | any link, `#anchor` links and a link around an image included |
| `WikiLinkClicked(page)` | `[[page]]` |
| `AnchorClicked(name)` | footnote reference/back-link, or a `#heading` link whose target exists. The view has scrolled already if `scroll_container` is set |
| `ImageRequested { url, id }` | once per URL when it is not in `resources.images` (`clear_image_requests()` re-arms it) |
| `ImageClicked(url)` | a picture that is not inside a link |
| `TaskToggled { block, item, checked }` | a check box was clicked; the view shows the new state until `set_source`. `task_source_range(item)` gives the `[ ]` byte range to rewrite |
| `CopyCode(text)` | the copy button; the app owns the clipboard |

## Images

The engine fetches nothing. Answer `ImageRequested` by loading the picture however you like and putting it in
`MarkdownResources::images` (`MarkdownImage::Ready { texture, size }` or `Failed`) on the next frames. Images are scaled down to the
column width (up too with `image_upscale`) and to `image_max_height`.

## `MarkdownView` API

`new`, `with_style`, `set_source`, `source`, `show`, `show_with`, `plain_text` (no markup; blocks separated by blank lines, list items and table
rows by single newlines, code and math as written, images as alt text), `links` (every target in order, once each; footnote jumps excluded),
`headings` (level, text, id), `scroll_to(anchor)` (heading id or `fn:LABEL`/`fnref:LABEL`; performed on the next `show_with` inside
`scroll_container`), `task_source_range`, `mark_visited`, `clear_visited`, `clear_image_requests`.

Changing a parser switch in the style (`smart_punctuation`, `underline_syntax`, `soft_break`, `autolink`, `unicode_scripts`, `task_glyphs`,
`math_hook`) re-parses (selection resets). Any other style change only rebuilds the cached draw data. A frame with an unchanged style and
document does no parsing, span building, text-state syncing or marker measuring: those are cached per block. What remains per frame is
submitting each block to the UI (the FFI call, which converts the cached spans again).

## Presets

`MarkdownStyle::chat()` (inline formatting plus code blocks, quotes and lists; newlines kept; read-only check boxes; hover underline; highlighted code with a label and
copy button; tight gaps) and `MarkdownStyle::document()` (everything on, code highlighted with a copy button) are starting points: the style is a flat struct, so
adjust what you need with struct-update syntax. With headings off, a `# line` stays as typed as a block of its own.

## `MarkdownStyle` fields

Original (defaults unchanged): `code_colorizer`, `text`, `muted`, `link`, `code_background`, `quote_bar`, `rule`, `font_size`, `regular_font`,
`bold_font`, `italic_font`, `bold_italic_font`, `mono_font`, `heading_scale`, `block_gap`, `list_indent`.

Parser switches: `smart_punctuation` (false), `underline_syntax` (false), `soft_break` (`Space`; `Newline` for chat), `autolink` (true),
`unicode_scripts` (false: real shifted runs), `task_glyphs` (false), `math_hook` (None), `math_mode` (`Layout`), `features` (`MarkdownFeatures::all()`).

Font slots (`None` keeps the default): `heading_font` (bold), `quote_font` (regular), `table_font` (regular), `table_header_font` (bold),
`link_font` (the run's own), `list_marker_font` (regular).

Links: `link` (normal), `link_visited` (None = `link`; set by clicking or `mark_visited`), `link_hover`, `link_pressed` (None = keep), `link_underline` (`Always`/`Never`/`OnHover`), `link_underline_style` (`Solid`), `link_underline_thickness` (0), `link_underline_color` (None), `link_tooltips` (true).

Text metrics: `line_height` (0), `letter_spacing` (0), `code_line_height` (0), `code_wrap` (true).

Spacing (`None` = `block_gap`): `heading_margin_top`, `heading_margin_bottom`, `rule_margin`, `list_item_gap`. A margin on the block that
follows wins, else the one on the block before, else `block_gap`. Others: `rule_thickness` (1), `quote_padding` (12), `quote_bar_width` (3),
`list_marker_gap` (6), `definition_indent` (20).

Lists: `bullets` (`["•"]`, cycled per level), `number_format` (`Decimal`, `LowerAlpha`, `UpperAlpha`, `LowerRoman`, `UpperRoman`), `number_suffix` (`"."`).

Tables: `table_header_background` (None = `code_background`), `table_zebra` (transparent), `table_border` (None = `rule`),
`table_cell_padding_x` (8), `table_cell_padding_y` (4).

Code: `code_padding` (10), `code_corner_radius` (6), `code_colours` (`CodeColours { keyword, string, comment, number, ty, function,
punctuation }`), `highlight_code` (false; uses the built-in highlighter when `code_colorizer` is None), `code_language_label`,
`code_copy_button`, `code_line_numbers`, `code_max_height` (all off/None).

Scripts: `script_scale` (0.7), `superscript_shift` (0.35), `subscript_shift` (-0.15).

Other: `alert_colours` (`AlertColours { note, tip, important, warning, caution }`), `script_scale` (0.7), `math_colour`, `task_read_only`,
`task_accent`, `task_box_size` (16), `image_max_height` (400), `image_upscale` (false).

## Using it anywhere

**Switch constructs on and off.** `MarkdownStyle::features` is a `MarkdownFeatures` of one bool per construct (headings, emphasis, strong,
strikethrough, inline code, links, wiki links, images, footnotes, math, superscript, subscript, tables, task lists, lists, block quotes, alerts,
code blocks, rules, definition lists, heading attributes). A disabled construct is shown as written (`# not a heading`, `**not bold**`),
never dropped. Presets: `all()` (default), `commonmark()`, `chat()` (inline + code blocks, quotes, lists), `inline()`, `none()` (plain text).
Change one and the view re-parses.

**Markdown in a label, tooltip or button.** `markdown::inline_text(source, &style)` returns `InlineText { text, spans, links }`: plain text,
`RichSpan`s and link ranges (`link_at(char)`), ready for `Ui::rich_text` or any widget that takes spans. Block syntax stays literal.

**Many messages.** `MarkdownViews::show(ui, key, source, &style, &resources)` keeps one `MarkdownView` per key, re-parses only when the text
changes, and evicts the least recently used past its capacity. `view_for(key, source, &style)` gives the view for `plain_text()`, `scroll_to`, etc.

**Take over blocks.** `MarkdownView::blocks()` lists every block as a `BlockInfo { index, kind, source (byte range of its lines), text, task }`;
`block_at_offset(byte)` and `source_offset(block, char)` map between positions; `show_blocks(ui, id, &style, &resources, &mut |ui, info| bool)`
calls you before each block and skips its rendering when you return `true` after drawing your own. Blocks around a replaced one keep their
selection state. With `MarkdownResources::report_block_clicks` a click on a text block reports `BlockClicked { block, char_index }`.

**Editors (Obsidian-style live preview).** `MarkdownEditor` is built from those pieces: the document is drawn rendered, clicking a block turns
its source lines into an editable text area (caret near the click), clicking away renders it again. Each edit splices the source and re-parses.
`edit_block`, `edit_at(offset)`, `edit_range`, `stop_editing` let the app decide which block shows its control symbols (for example the one
under a caret you track yourself); `EditorOutput { events, changed, needs_redraw }` says when to save and when to ask for another frame.
Granularity is a block (a paragraph, heading, list item, code block, table, alert line); hiding markers *inside* a paragraph while the rest
of it stays editable needs the engine's text area to take styled spans while editable, which it does not yet.

## Math

`markdown::tex` is usable on its own: `tex::parse(src) -> Result<TexNode, TexError>`, `tex::to_unicode(&node)` (one line: `x²`, `α ≤ β`,
`a/b`, `√(x + 1)`, combining accents) and `tex::draw(ui, &node, &TexStyle)` (boxes: fractions with a rule, scripts raised and lowered, roots with an
overbar, limits above and below `\sum`/`\lim`, matrices in columns, delimiters sized to their content). Supported: Greek and the common symbols,
operators and relations, arrows, `^ _ '`, `\frac \dfrac \tfrac \binom`, `\sqrt[n]{}`, big operators, named functions, `\hat \bar \vec \dot \ddot \tilde
\overline \underline`, `\left..\right`, `\text`, `\mathbb \mathcal \mathfrak \mathbf \mathrm \mathsf \mathtt \mathit`, spacing, `matrix pmatrix bmatrix
Bmatrix vmatrix Vmatrix cases aligned align array`. Unknown commands show as written; a formula that does not parse shows its source. Nesting
is capped at 48 levels. `tex::to_runs` is the inline form for text that can shift runs from the baseline (`e^{i\pi}` is `e` plus a raised `iπ`). `MathMode::{Source, Unicode, Layout}` picks the Markdown behaviour (`Layout` default: inline as text with raised/lowered runs, display as boxes; `Unicode`: one line of Unicode text). The box layout
places nested boxes by centred alignment, an approximation of TeX's baseline rules.

## Text layout (superscript, subscript, wrapping)

`RichSpan::baseline_shift` raises (positive) or lowers (negative) a run from the baseline, as a fraction of the block's font size; with
`size_scale` it makes real superscripts (`MarkdownStyle::superscript_shift` 0.35, `subscript_shift` -0.15, `script_scale` 0.7). The engine's text
areas (`TextEdit.cpp`) lay out wrapped paragraphs themselves: the paragraph is broken into visual lines by measuring its words in each run's own
font and size (kept together across style changes, an over-long word broken between characters), each visual line is a row of runs
aligned on their bottoms, and a run's shift lifts or lowers it inside its row. So paragraphs with bold, links, scripts, a caret or a selection
wrap like plain text, and pointer hit-testing walks the runs of the clicked row. The width wrapped to is the widget's viewport from the
previous frame, so the very first frame of a paragraph with several styles is unwrapped and the widget asks for one more frame (`relayout`);
a resize shows the old wrapping for one frame. Breaks are cached per paragraph (text, styles, width). Caret and selection do not affect them.

## Links, selection and text metrics

* **Hover, pressed, tooltips, context menu.** `link_hover` and `link_pressed` colour the link under the pointer (`link_pressed` while the button is
  down), `link_underline` can be `OnHover`, `link_underline_style` (`Solid`, `Dashed`, `Dotted`, `Double`), `link_underline_thickness` and
  `link_underline_color` shape the line, and `link_tooltips` shows a link's title after the pointer rests on it. Events: `LinkHovered(Option<url>)`
  when the pointer moves onto or off a link and `LinkContext { url, x, y }` on a right-click (position in layout pixels, for your menu).
  The hover is the engine's "character under the pointer" (`RichTextResult::hovered_char`, and `Ui::rich_text_char_at` for any point).
* **Selection across blocks.** A drag that starts in one block and ends in another selects the end of the first, whole blocks between, and the
  start of the last; `selected_text()` returns it (blocks separated by a blank line, table cells by a newline) and Ctrl+C with such a selection
  reports `CopySelection(text)` for the app to put on the clipboard (a selection inside one block is copied by the block). `clear_selection()` clears.
* **Text metrics.** `line_height`/`code_line_height` (pixels, 0 = the font's own; a heading's line scales with its text) and `letter_spacing`
  (pixels after every glyph, honoured by measuring, wrapping and hit-testing) are engine text features: `TextEditStyle::{line_height,
  letter_spacing}`. The engine used to ignore `letter_spacing` in shaping.
* **Code that does not wrap.** `code_wrap: false` keeps a line of code on one row; the block scrolls sideways (scrollbar when needed) and the
  line-number gutter stays in step. (With wrapping on, the gutter still drifts on wrapped lines.)
* **Wrap points** follow the Unicode line-breaking rules (UAX #14), so CJK text and hyphens break where they should, not only at spaces.
* The engine caches a text block's spans per text; a changed colour for the same text (a hover, a theme switch) used to be ignored.
  `TextEditStyle::highlighter_revision` (the FFI fingerprints the spans) now invalidates the cache.

## Live preview, culling and layout details

* **Live preview editor** (`MarkdownEditor::show_live`): the whole document is one editable text drawn with spans from `markdown::live_spans(source,
  &style, reveal_ranges, reveal_all)`: headings large, bold/italic runs, coloured links, code in the mono font with the built-in highlighter's
  colours, quotes muted, scripts raised. The markup characters (`#`, `**`, `[`, `](url)`, fences, `>`, `$`) are `RichSpan::hidden`
  (no room, no drawing, still in the text, so caret, selection, clipboard and undo work on the source) except in the ranges the app reveals, where they
  are dimmed. `RevealPolicy` decides: `ActiveBlock` (default), `ActiveLine`, `All`, `Nothing`, or `Custom(|view, caret_byte| ranges)`; a selection
  always reveals the lines it touches. Images, tables and display math show as source in this mode (`show` is the block-by-block mode that renders them).
  Spans follow the text one frame late after an edit, so the editor asks for the next frame at once; for typed text it lines the old spans up with the
  new text for that frame (`shift_spans_for_insertion`) so nothing flashes mis-styled (deletions still show one frame of old styling). IME composition goes through the
  engine's own composition path and was not tested against hidden runs.
* **Culling.** With `MarkdownResources::scroll_container` set and `cull_offscreen` (default), each block is wrapped in an element; blocks more than a
  viewport away from the visible part are drawn as empty space of their last height, so a chat with hundreds of messages costs what is on screen
  (and images are only requested when they come near). Nothing is skipped while a scroll to an anchor is pending or the column was just resized.
  Selection state of skipped blocks is kept.
* **Auto-scroll while selecting.** Dragging near the top or bottom of the scroll container scrolls it.
* **Code gutter.** With `code_wrap` on, a wrapped line takes several rows; the gutter reads the engine's visual layout
  (`Ui::rich_text_row_counts`) and numbers only the first row of each line.
* **Real baselines.** Runs of different sizes in a row share one baseline: each is dropped by the difference between the highest reach (font
  ascent plus its raise) in the row and its own (`Context::text_ascent`).
* **First frame.** `MarkdownResources::width_hint` (and what `MarkdownViews` learns from earlier views) is the width text wraps to before the
  first layout, so a new view is not drawn unwrapped; `TextInputStyle::wrap_width_hint` is the engine side.

## Limits that remain

* **A resize shows the old wrapping for one frame** (the width wrapped to is the previous frame's viewport). Fixing it needs the host to lay a frame out
  twice, which would run the draw hook twice; not done.
* **Inline images and inline math boxes** cannot sit in a text run: the flow layout only places text. Images are blocks, inline math is text with raised
  runs, and the live editor shows both as source. An "inline object" span (reserve a size, emit an element) is the engine change that would allow it.
* **Text blocks do not flow together.** Each block is its own text area, so selection across blocks is simulated (above).

`Ui::rich_text` always word-wraps; use `Ui::rich_text_wrapping` (which honours `TextInputStyle::wrap_lines`) for text that must stay on one row, as non-wrapping code does.
