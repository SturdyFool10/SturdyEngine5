# Shell Demo 0.1 {#top .title}

A reference immediate-mode app for **SE5**. Everything here is rebuilt every frame. This page is the Markdown demo: every construct
`sturdy::markdown` draws is below. Jump to [the footnotes](#footnotes-and-definitions) or to [the code](#code).

## Inline text

**Bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`, and a [link with a title](https://example.com "Shown as a tooltip where the engine supports it").
Bare URLs link themselves: https://github.com/sturdyfool10/sturdyengine5, (www.example.org) and "see https://example.com/a_b."

Water is H ~2~ O, area is x ^2^ , and a ^word^ with no Unicode form is drawn smaller. Wiki links: [[Home Page]] and [[Other Page|a label]].
Underline is opt-in: __this is bold unless `underline_syntax` is on__. Smart punctuation is opt-in too: "quotes" -- dashes... it's.
Math: inline $E = mc^2$ and display math:

$$\int_0^1 x^2 \, dx = \tfrac{1}{3}$$

A soft
line break stays a space unless `soft_break` is `Newline`.

## Lists and tasks

- Custom *title bar* with OS drag and resize
- Virtualised lists (50 000 rows)
  - Nested bullets change character per level
    - Third level
- Live settings: ~~restart to apply~~ nothing needs a restart
  1. Graphics API and GPU
  2. Window blur and transparency
  3. Fonts and text size

8. Numbers are right-aligned in their column
9. Even when the width changes
10. Like this

- [x] Real check boxes
- [ ] Click one: the event carries the item number and the source range
- [ ] `task_read_only` makes them inert

## Quotes and alerts

> One level of quote.
>
> > Two levels, two bars.
> >
> > > Three.

> [!NOTE]
> Useful information that users should know.

> [!TIP]
> Helpful advice for doing things better.

> [!IMPORTANT]
> Key information users need to know.

> [!WARNING]
> Urgent info that needs immediate attention.

> [!CAUTION]
> Advises about risks or negative outcomes.

## Tables

| Left | Centre | Right |
|:-----|:------:|------:|
| a    |   b    |     c |
| long cell text | mid | 1 234 |
| zebra | rows | when set |

## Code

```rust
// This block does not wrap (`code_wrap: false`): the long line below scrolls sideways, and the line numbers stay in step with it.
const LONG: &str = "a very long line of code that goes well past the right edge of the column so that it needs a horizontal scrollbar to be read";
// A built-in highlighter colours this when `highlight_code` is on.
fn main() {
    let name: String = "world".into();
    println!("hello {name}, {}", 42u8);
}
```

```python
def greet(name: str) -> str:
    """Say hello."""
    return f"hello {name}"  # comment
```

```json
{ "name": "demo", "version": 1.5, "tags": [true, null, "x"] }
```

```sql
SELECT id, name FROM users WHERE created > '2026-01-01' ORDER BY id; -- recent
```

```toml
[package]
name = "demo"
```

```sh
echo "$HOME" && ls -la
```

## Images

![Alt text shown until the app supplies the texture](https://example.com/picture.png "An image title")

[![A badge that is also a link](https://example.com/badge.svg)](https://example.com/ci)

## Footnotes and definitions

Footnote references are superscript numbers[^first] that jump to the list at the end, and the list links back[^second].

Term
:   A definition, indented under its bold term.

Another term
:   First definition.
:   Second definition.

---

Raw HTML is dropped: <span style="color:red">this text stays, the tags do not</span>.

[^first]: The first footnote. Click the arrow to go back.
[^second]: The second one, with `code` in it.
