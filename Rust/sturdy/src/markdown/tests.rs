//! Unit tests: everything that does not need a running UI (parsing, the pure layout decisions, the highlighter, the span builder).

use std::collections::HashSet;

use glam::{Vec2, Vec4};

use super::highlight::{MarkdownHighlighter, LANGUAGES};
use super::parse::{find_urls, parse_with, slugify, to_script, AlertKind, Block, ColumnAlign, LinkKind, ParseConfig, Parsed, Prose, Script};
use super::style::*;
use super::view::{code_prose, fit_image, format_number, gap_between, links_of, marker_text, pack_color, plain_text_of, unpack_color};
use super::MarkdownView;

fn parse(source: &str) -> Vec<Block> {
    parse_with(source, &ParseConfig::from_style(&MarkdownStyle::default())).blocks
}

fn parse_style(source: &str, style: &MarkdownStyle) -> Parsed {
    parse_with(source, &ParseConfig::from_style(style))
}

fn paragraph(blocks: &[Block]) -> &Prose {
    blocks
        .iter()
        .find_map(|b| match b {
            Block::Paragraph { prose, .. } => Some(prose),
            _ => None,
        })
        .unwrap_or_else(|| panic!("no paragraph in {blocks:?}"))
}

// -- The original behaviour ----------------------------------------------------------------------------------------------------------

#[test]
fn parses_headings_paragraphs_and_inline_formatting() {
    let blocks = parse("# Title\n\nSome **bold** and *italic* and `code` and [a link](https://example.com).");
    assert_eq!(blocks.len(), 2);
    let Block::Heading { level, prose, .. } = &blocks[0] else { panic!("{blocks:?}") };
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
    assert!(blocks.iter().any(|b| matches!(b, Block::Code { language, text } if language == "rust" && text == "fn main() {}")));
    let table = blocks.iter().find_map(|b| match b {
        Block::Table { header, rows, .. } => Some((header.len(), rows.len(), rows[0][1].text.clone())),
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

#[test]
fn default_style_keeps_the_original_numbers() {
    let style = MarkdownStyle::default();
    assert_eq!((style.font_size, style.block_gap, style.list_indent), (15, 10, 20));
    assert_eq!(style.heading_scale, [1.8, 1.5, 1.3, 1.15, 1.05, 1.0]);
    assert_eq!((style.quote_padding, style.quote_bar_width, style.list_marker_gap), (12, 3, 6));
    assert_eq!((style.code_padding, style.code_corner_radius, style.rule_thickness), (10, 6.0, 1.0));
    assert_eq!((style.table_cell_padding_x, style.table_cell_padding_y), (8, 4));
    assert_eq!(style.bullets, vec!["\u{2022}".to_owned()]);
    // Opt-in features are off.
    assert!(!style.smart_punctuation && !style.underline_syntax && !style.task_glyphs && !style.task_read_only);
    assert!(!style.highlight_code && !style.code_copy_button && !style.code_line_numbers && !style.code_language_label);
    assert_eq!(style.soft_break, SoftBreak::Space);
    assert_eq!(style.link_underline, LinkUnderline::Always);
    assert!(style.heading_font.is_none() && style.quote_font.is_none() && style.table_font.is_none());
    assert!(style.table_header_font.is_none() && style.link_font.is_none() && style.list_marker_font.is_none());
}

#[test]
fn style_presets_configure_whole_use_cases() {
    let chat = MarkdownStyle::chat();
    assert_eq!(chat.features, super::MarkdownFeatures::chat());
    assert!(chat.task_read_only && chat.highlight_code && chat.code_copy_button);
    // Inline formatting works, headings stay literal, newlines are kept.
    let blocks = parse_style("# not a heading\n**bold** line\nnext line", &chat).blocks;
    let texts: Vec<_> = blocks.iter().map(|b| if let Block::Paragraph { prose, .. } = b { prose.text.clone() } else { String::new() }).collect();
    // The heading line stays as typed (a block of its own); the lines after it keep their line break.
    assert_eq!(texts, vec!["# not a heading", "bold line\nnext line"]);
    let document = MarkdownStyle::document();
    assert_eq!(document.features, super::MarkdownFeatures::all());
    assert!(document.highlight_code && !document.task_read_only);
    assert_eq!(MarkdownStyle { highlight_code: false, code_copy_button: false, code_language_label: false, ..document }, MarkdownStyle::default());
}

// -- 1. Parser options ---------------------------------------------------------------------------------------------------------------

#[test]
fn footnotes_are_numbered_by_first_reference_and_listed_at_the_end() {
    let blocks = parse("Alpha[^b] and beta[^a] and again[^b].\n\n[^a]: Note A.\n[^b]: Note B.\n");
    let prose = paragraph(&blocks);
    assert_eq!(prose.text, "Alpha\u{b9} and beta\u{b2} and again\u{b9}.");
    let kinds: Vec<_> = prose.links.iter().map(|l| (l.kind, l.url.as_str())).collect();
    assert_eq!(kinds, vec![(LinkKind::Footnote, "fn:b"), (LinkKind::Footnote, "fn:a"), (LinkKind::Footnote, "fn:b")]);
    assert_eq!(prose.anchors, vec!["fnref:b".to_owned(), "fnref:a".to_owned()]);
    // After the paragraph: a rule, then the footnotes in reference order, each ending in a back-link.
    assert!(matches!(blocks.get(1), Some(Block::Rule)));
    let notes: Vec<_> = blocks
        .iter()
        .filter_map(|b| match b {
            Block::Footnote { number, label, prose } => Some((*number, label.clone(), prose.text.clone(), prose.anchors.clone(), prose.links.clone())),
            _ => None,
        })
        .collect();
    assert_eq!(notes.len(), 2);
    assert_eq!((notes[0].0, notes[0].1.as_str(), notes[0].2.as_str()), (1, "b", "Note B. \u{21a9}"));
    assert_eq!((notes[1].0, notes[1].1.as_str(), notes[1].2.as_str()), (2, "a", "Note A. \u{21a9}"));
    assert_eq!(notes[0].3, vec!["fn:b".to_owned()]);
    assert_eq!(notes[0].4[0].kind, LinkKind::FootnoteBack);
    assert_eq!(notes[0].4[0].url, "fnref:b");
}

#[test]
fn unreferenced_footnote_is_listed_after_referenced_ones() {
    let blocks = parse("Text[^x].\n\n[^orphan]: Alone.\n[^x]: Used.\n");
    let numbers: Vec<_> = blocks
        .iter()
        .filter_map(|b| match b {
            Block::Footnote { number, label, .. } => Some((*number, label.clone())),
            _ => None,
        })
        .collect();
    assert_eq!(numbers, vec![(1, "x".to_owned()), (2, "orphan".to_owned())]);
}

#[test]
fn definition_lists_have_terms_and_details() {
    let blocks = parse("Apple\n:   A fruit.\n\nCherry\n:   Small and red.\n:   Also a tree.\n");
    let seen: Vec<_> = blocks
        .iter()
        .map(|b| match b {
            Block::DefTerm { prose } => format!("term:{}", prose.text),
            Block::DefDetail { prose } => format!("detail:{}", prose.text),
            other => format!("other:{other:?}"),
        })
        .collect();
    assert_eq!(seen, vec!["term:Apple", "detail:A fruit.", "term:Cherry", "detail:Small and red.", "detail:Also a tree."]);
}

#[test]
fn math_is_marked_and_the_hook_can_replace_it() {
    let blocks = parse("Euler: $e^{i\\pi}+1=0$.\n\n$$\\int_0^1 x\\,dx$$\n");
    let prose = paragraph(&blocks);
    assert_eq!(prose.text, "Euler: eiπ + 1 = 0.", "in Layout mode the exponent is its own raised run");
    assert!(prose.runs.iter().any(|(_, _, f)| f.math && f.script == Script::Sup));
    let source_mode = MarkdownStyle { math_mode: MathMode::Source, ..MarkdownStyle::default() };
    assert_eq!(paragraph(&parse_style("Euler: $e^{i\\pi}+1=0$.", &source_mode).blocks).text, "Euler: e^{i\\pi}+1=0.");
    assert!(prose.runs.iter().any(|(_, _, f)| f.math));
    assert!(blocks.iter().any(|b| matches!(b, Block::Math { tex, render: None, .. } if tex == "\\int_0^1 x\\,dx")));

    let math_hook = MathHook::new(|tex, display| {
        if display {
            Some(MathRender::Image { texture: ImageHandle::default(), size: Vec2::new(40.0, 12.0) })
        } else {
            Some(MathRender::Text(tex.replace("^{2}", "\u{b2}")))
        }
    });
    let style = MarkdownStyle { math_hook: Some(math_hook), ..MarkdownStyle::default() };
    let parsed = parse_style("Area $r^{2}$.\n\n$$a$$\n", &style);
    assert_eq!(paragraph(&parsed.blocks).text, "Area r\u{b2}.");
    assert!(parsed.blocks.iter().any(|b| matches!(b, Block::Math { render: Some(MathRender::Image { .. }), .. })));
}

#[test]
fn superscript_and_subscript_are_real_shifted_runs_or_unicode_on_request() {
    // The markers must be delimited by whitespace or punctuation (a pulldown-cmark rule): `H~2~O` stays literal.
    assert_eq!(paragraph(&parse("H~2~O")).text, "H~2~O");

    // By default the text is kept and the run is marked, so the view draws it smaller and shifted from the baseline.
    let blocks = parse("H ~2~ O and x ^2^ and y ^n+1^");
    let prose = paragraph(&blocks);
    assert_eq!(prose.text, "H 2 O and x 2 and y n+1");
    let scripts: Vec<_> = prose.runs.iter().map(|(_, _, f)| f.script).filter(|s| *s != Script::None).collect();
    assert_eq!(scripts, vec![Script::Sub, Script::Sup, Script::Sup]);

    // Unicode forms on request, where every character of the run has one.
    let unicode = MarkdownStyle { unicode_scripts: true, ..MarkdownStyle::default() };
    let parsed = parse_style("H ~2~ O and x ^2^ and y ^n+1^ (z ^i^) a ^bc^", &unicode);
    assert_eq!(paragraph(&parsed.blocks).text, "H \u{2082} O and x \u{b2} and y \u{207f}\u{207a}\u{b9} (z \u{2071}) a bc");
    let prose = paragraph(&parsed.blocks);
    assert_eq!(prose.runs.iter().filter(|(_, _, f)| f.script == Script::Sup).count(), 1, "only `bc` has no Unicode form");
    assert_eq!(to_script("12", true).as_deref(), Some("\u{b9}\u{b2}"));
    assert_eq!(to_script("q", false), None);
}

#[test]
fn script_runs_are_smaller_and_shifted_from_the_baseline() {
    let style = MarkdownStyle { script_scale: 0.6, superscript_shift: 0.4, subscript_shift: -0.2, ..MarkdownStyle::default() };
    let spans = cell_spans("a ^b^ c ~d~ e", &style, &[]);
    let shifts: Vec<_> = spans.iter().map(|s| (s.size_scale, s.baseline_shift)).collect();
    assert_eq!(shifts, vec![(1.0, 0.0), (0.6, 0.4), (1.0, 0.0), (0.6, -0.2), (1.0, 0.0)]);
    // The defaults put a superscript above and a subscript below the baseline.
    let defaults = MarkdownStyle::default();
    assert!(defaults.superscript_shift > 0.0 && defaults.subscript_shift < 0.0);
    // Neighbouring runs with the same shift merge; different shifts do not.
    let merged = super::view::merge_spans(vec![
        crate::widgets::RichSpan { start: 0, len: 1, baseline_shift: 0.3, ..Default::default() },
        crate::widgets::RichSpan { start: 1, len: 1, baseline_shift: 0.3, ..Default::default() },
        crate::widgets::RichSpan { start: 2, len: 1, baseline_shift: -0.1, ..Default::default() },
    ]);
    assert_eq!(merged.len(), 2);
}

#[test]
fn heading_attributes_keep_the_id_and_classes_and_plain_headings_get_slugs() {
    let blocks = parse("# Title {#top .big .red}\n\n## Getting Started\n\n## Getting Started\n\n### Ünï & Co.\n");
    let headings: Vec<_> = blocks
        .iter()
        .filter_map(|b| match b {
            Block::Heading { level, prose, id, classes } => Some((*level, prose.text.clone(), id.clone(), classes.clone(), prose.anchors.clone())),
            _ => None,
        })
        .collect();
    assert_eq!(headings[0], (1, "Title".into(), Some("top".into()), vec!["big".into(), "red".into()], vec!["top".into()]));
    assert_eq!(headings[1].2.as_deref(), Some("getting-started"));
    assert_eq!(headings[2].2.as_deref(), Some("getting-started-1"));
    assert_eq!(headings[3].2.as_deref(), Some("ünï--co"));
    assert_eq!(slugify("  Hello, World! "), "hello-world");
}

#[test]
fn smart_punctuation_is_a_style_flag_off_by_default() {
    let source = "\"Quoted\" -- and... it's";
    assert_eq!(paragraph(&parse(source)).text, source);
    let style = MarkdownStyle { smart_punctuation: true, ..MarkdownStyle::default() };
    let parsed = parse_style(source, &style);
    assert_eq!(paragraph(&parsed.blocks).text, "\u{201c}Quoted\u{201d} \u{2013} and\u{2026} it\u{2019}s");
}

#[test]
fn github_alerts_become_a_titled_quote() {
    let blocks = parse("> [!WARNING]\n> Careful here.\n\n> [!tip]\n> Nice.\n\n> plain quote\n");
    let kinds: Vec<_> = blocks
        .iter()
        .filter_map(|b| match b {
            Block::AlertTitle { kind, quote } => Some((*kind, quote.clone())),
            _ => None,
        })
        .collect();
    assert_eq!(kinds, vec![(AlertKind::Warning, vec![Some(AlertKind::Warning)]), (AlertKind::Tip, vec![Some(AlertKind::Tip)])]);
    let first = blocks.iter().find_map(|b| match b {
        Block::Paragraph { prose, quote } => Some((prose.text.clone(), quote.clone())),
        _ => None,
    });
    assert_eq!(first, Some(("Careful here.".into(), vec![Some(AlertKind::Warning)])));
    assert!(blocks.iter().any(|b| matches!(b, Block::Paragraph { prose, quote } if prose.text == "plain quote" && *quote == vec![None])));
    assert_eq!(AlertKind::Important.title(), "Important");
}

#[test]
fn wikilinks_are_a_distinct_link_kind() {
    let blocks = parse("See [[Home Page]] and [[Other|the label]] and [normal](https://x.org).");
    let prose = paragraph(&blocks);
    assert_eq!(prose.text, "See Home Page and the label and normal.");
    let links: Vec<_> = prose.links.iter().map(|l| (l.kind, l.url.as_str())).collect();
    assert_eq!(links, vec![(LinkKind::Wiki, "Home Page"), (LinkKind::Wiki, "Other"), (LinkKind::External, "https://x.org")]);
}

#[test]
fn underline_syntax_is_a_flag_off_by_default() {
    let blocks = parse("a __under__ b **bold**");
    let prose = paragraph(&blocks);
    assert!(prose.runs.iter().filter(|(_, _, f)| f.bold).count() == 2 && prose.runs.iter().all(|(_, _, f)| !f.underline));

    let style = MarkdownStyle { underline_syntax: true, ..MarkdownStyle::default() };
    let parsed = parse_style("a __under__ b **bold**", &style);
    let prose = paragraph(&parsed.blocks);
    let under: Vec<_> = prose.runs.iter().filter(|(_, _, f)| f.underline).map(|(s, l, f)| (*s, *l, f.bold)).collect();
    assert_eq!(under, vec![(2, 5, false)]);
    assert_eq!(prose.runs.iter().filter(|(_, _, f)| f.bold).count(), 1);
}

// -- 2. Elements drawn wrongly -------------------------------------------------------------------------------------------------------

#[test]
fn images_become_blocks_with_alt_title_and_enclosing_link() {
    let blocks = parse("Before ![the alt](pic.png \"A title\") after\n\n[![badge](b.svg)](https://ci.example)\n");
    assert!(matches!(&blocks[0], Block::Paragraph { prose, .. } if prose.text == "Before "));
    assert!(matches!(&blocks[1], Block::Image { url, alt, title, link: None, .. } if url == "pic.png" && alt == "the alt" && title == "A title"));
    assert!(matches!(&blocks[2], Block::Paragraph { prose, .. } if prose.text == " after"));
    assert!(blocks.iter().any(|b| matches!(b, Block::Image { url, link: Some(link), .. } if url == "b.svg" && link == "https://ci.example")));
}

#[test]
fn images_inside_tables_and_headings_stay_alt_text() {
    let blocks = parse("| a |\n|---|\n| ![pic](p.png) |\n\n# Head ![icon](i.png)\n");
    assert!(!blocks.iter().any(|b| matches!(b, Block::Image { .. })));
    let Some(Block::Table { rows, .. }) = blocks.first() else { panic!("{blocks:?}") };
    assert_eq!(rows[0][0].text, "pic");
}

#[test]
fn bare_urls_become_links_without_trailing_punctuation() {
    let found = |text: &str| find_urls(text).into_iter().map(|(a, b)| text[a..b].to_owned()).collect::<Vec<_>>();
    assert_eq!(found("see https://example.com/a?b=1."), vec!["https://example.com/a?b=1"]);
    assert_eq!(found("(http://example.com/x)"), vec!["http://example.com/x"]);
    assert_eq!(found("(see https://en.wikipedia.org/wiki/Rust_(language))"), vec!["https://en.wikipedia.org/wiki/Rust_(language)"]);
    assert_eq!(found("go to www.example.org, then"), vec!["www.example.org"]);
    assert_eq!(found("a, https://a.io/x; b!"), vec!["https://a.io/x"]);
    assert_eq!(found("nohttps://x.io and https:// and www."), Vec::<String>::new());
    assert_eq!(found("ünï https://ü.example/é"), vec!["https://ü.example/é"]);

    let blocks = parse("Visit https://example.com. Or www.rust-lang.org! Not `https://in.code` nor [x](https://l.ink) https://after.com");
    let prose = paragraph(&blocks);
    let urls: Vec<_> = prose.links.iter().map(|l| l.url.as_str()).collect();
    assert_eq!(urls.iter().collect::<HashSet<_>>(), HashSet::from([&"https://example.com", &"http://www.rust-lang.org", &"https://l.ink", &"https://after.com"]));
    assert_eq!(urls.len(), 4);
    assert_eq!(prose.text, "Visit https://example.com. Or www.rust-lang.org! Not https://in.code nor x https://after.com");
    let at = prose.text.find("www").unwrap();
    assert_eq!(prose.link_at(at), Some("http://www.rust-lang.org"));
    let code_at = prose.text.find("in.code").unwrap();
    assert_eq!(prose.link_at(code_at), None);

    let off = MarkdownStyle { autolink: false, ..MarkdownStyle::default() };
    assert!(paragraph(&parse_style("https://example.com", &off).blocks).links.is_empty());
}

#[test]
fn task_items_carry_state_and_their_source_range() {
    let source = "- [ ] open\n- [x] done\n- plain\n";
    let parsed = parse_style(source, &MarkdownStyle::default());
    let tasks: Vec<_> = parsed
        .blocks
        .iter()
        .filter_map(|b| match b {
            Block::ListItem { prose, task, .. } => Some((prose.text.clone(), task.map(|t| (t.index, t.checked)))),
            _ => None,
        })
        .collect();
    assert_eq!(tasks, vec![("open".into(), Some((0, false))), ("done".into(), Some((1, true))), ("plain".into(), None)]);
    let ranges: Vec<_> = parsed.task_ranges.iter().map(|r| &source[r.clone()]).collect();
    assert_eq!(ranges, vec!["[ ]", "[x]"]);

    let glyphs = MarkdownStyle { task_glyphs: true, ..MarkdownStyle::default() };
    let parsed = parse_style(source, &glyphs);
    assert!(parsed.blocks.iter().any(|b| matches!(b, Block::ListItem { prose, task: None, .. } if prose.text == "\u{2610} open")));
    assert!(parsed.blocks.iter().any(|b| matches!(b, Block::ListItem { prose, .. } if prose.text == "\u{2611} done")));
}

#[test]
fn table_alignment_is_read_per_column() {
    let blocks = parse("| a | b | c | d |\n|:--|:-:|--:|---|\n| 1 | 2 | 3 | 4 |\n");
    let Some(Block::Table { aligns, .. }) = blocks.first() else { panic!("{blocks:?}") };
    assert_eq!(aligns, &vec![ColumnAlign::Left, ColumnAlign::Center, ColumnAlign::Right, ColumnAlign::Left]);
}

#[test]
fn nested_quotes_report_their_depth() {
    let blocks = parse("> one\n>\n> > two\n> > > three\n\nout\n");
    let depths: Vec<_> = blocks
        .iter()
        .filter_map(|b| match b {
            Block::Paragraph { prose, quote } => Some((prose.text.clone(), quote.len())),
            _ => None,
        })
        .collect();
    assert_eq!(depths, vec![("one".into(), 1), ("two".into(), 2), ("three".into(), 3), ("out".into(), 0)]);
}

#[test]
fn link_titles_are_kept() {
    let blocks = parse("[a](https://u.rl \"the title\") and [b](#sec)");
    let prose = paragraph(&blocks);
    assert_eq!(prose.links[0].title, "the title");
    assert_eq!(prose.links[1].kind, LinkKind::Anchor);
    assert_eq!(prose.link_target_at(0).map(|t| t.title.as_str()), Some("the title"));
}

#[test]
fn soft_breaks_follow_the_style() {
    let source = "line one\nline two";
    assert_eq!(paragraph(&parse(source)).text, "line one line two");
    let style = MarkdownStyle { soft_break: SoftBreak::Newline, ..MarkdownStyle::default() };
    assert_eq!(paragraph(&parse_style(source, &style).blocks).text, "line one\nline two");
}

#[test]
fn list_markers_follow_level_and_number_format() {
    assert_eq!(format_number(3, NumberFormat::Decimal), "3");
    assert_eq!(format_number(1, NumberFormat::LowerAlpha), "a");
    assert_eq!(format_number(26, NumberFormat::LowerAlpha), "z");
    assert_eq!(format_number(27, NumberFormat::LowerAlpha), "aa");
    assert_eq!(format_number(28, NumberFormat::UpperAlpha), "AB");
    assert_eq!(format_number(4, NumberFormat::LowerRoman), "iv");
    assert_eq!(format_number(1994, NumberFormat::UpperRoman), "MCMXCIV");
    assert_eq!(format_number(0, NumberFormat::UpperRoman), "0");
    assert_eq!(format_number(4000, NumberFormat::LowerRoman), "4000");
    assert_eq!(format_number(0, NumberFormat::LowerAlpha), "0");

    let style = MarkdownStyle {
        bullets: vec!["\u{2022}".into(), "\u{25e6}".into(), "\u{25aa}".into()],
        number_format: NumberFormat::LowerRoman,
        number_suffix: ")".into(),
        ..MarkdownStyle::default()
    };
    assert_eq!(marker_text(None, 0, &style), "\u{2022}");
    assert_eq!(marker_text(None, 1, &style), "\u{25e6}");
    assert_eq!(marker_text(None, 3, &style), "\u{2022}");
    assert_eq!(marker_text(Some(9), 0, &style), "ix)");
    let empty = MarkdownStyle { bullets: Vec::new(), ..MarkdownStyle::default() };
    assert_eq!(marker_text(None, 2, &empty), "\u{2022}");
    assert_eq!(marker_text(Some(2), 0, &MarkdownStyle::default()), "2.");
}

#[test]
fn lists_know_how_many_items_they_hold() {
    let parsed = parse_style("3. a\n4. b\n5. c\n\n- x\n  - y\n", &MarkdownStyle::default());
    let infos: Vec<_> = parsed.lists.iter().map(|l| (l.start, l.count)).collect();
    assert_eq!(infos, vec![(Some(3), 3), (None, 1), (None, 1)]);
    let markers: Vec<_> = parsed
        .blocks
        .iter()
        .filter_map(|b| match b {
            Block::ListItem { ordinal, .. } => Some(*ordinal),
            _ => None,
        })
        .collect();
    assert_eq!(markers, vec![Some(3), Some(4), Some(5), None, None]);
}

// -- 3. Styling ----------------------------------------------------------------------------------------------------------------------

fn cell_spans(source: &str, style: &MarkdownStyle, visited: &[&str]) -> Vec<crate::widgets::RichSpan> {
    let blocks = parse_style(source, style).blocks;
    let prose = paragraph(&blocks);
    let look = super::view::test_look(style.regular_font);
    let visited: HashSet<String> = visited.iter().map(|s| (*s).to_owned()).collect();
    super::view::test_spans(prose, look, style, &visited)
}

#[test]
fn font_slots_apply_per_element_and_none_keeps_the_default() {
    let mut style = MarkdownStyle { bold_font: 2, italic_font: 3, mono_font: 4, ..MarkdownStyle::default() };
    let source = "**b** *i* `c` [l](https://l.ink) plain";
    let fonts = |spans: &[crate::widgets::RichSpan]| spans.iter().map(|s| s.font_id).collect::<Vec<_>>();
    assert_eq!(fonts(&cell_spans(source, &style, &[])), vec![Some(2), None, Some(3), None, Some(4), None, None, None]);
    style.link_font = Some(9);
    let spans = cell_spans(source, &style, &[]);
    assert_eq!(spans.iter().filter(|s| s.font_id == Some(9)).count(), 1);
}

#[test]
fn link_colours_underline_and_visited_state() {
    let mut style = MarkdownStyle { link: Vec4::new(0.0, 0.0, 1.0, 1.0), ..MarkdownStyle::default() };
    let source = "a [one](https://one.example) [two](https://two.example)";
    let links = |spans: Vec<crate::widgets::RichSpan>| spans.into_iter().filter(|s| s.color.is_some()).collect::<Vec<_>>();

    let spans = links(cell_spans(source, &style, &[]));
    assert_eq!(spans.len(), 2);
    assert!(spans.iter().all(|s| s.underline && s.color == Some(style.link)));

    style.link_visited = Some(Vec4::new(0.5, 0.0, 0.5, 1.0));
    let spans = links(cell_spans(source, &style, &["https://two.example"]));
    assert_eq!(spans[0].color, Some(style.link));
    assert_eq!(spans[1].color, style.link_visited);

    style.link_underline = LinkUnderline::Never;
    assert!(links(cell_spans(source, &style, &[])).iter().all(|s| !s.underline));
}

#[test]
fn underline_runs_and_inline_math_colour() {
    let style = MarkdownStyle { underline_syntax: true, math_colour: Some(Vec4::new(1.0, 0.0, 0.0, 1.0)), mono_font: 5, ..MarkdownStyle::default() };
    let spans = cell_spans("__u__ and $x$", &style, &[]);
    assert!(spans[0].underline && !spans[1].underline);
    let math = spans.last().unwrap();
    assert_eq!((math.font_id, math.color), (Some(5), style.math_colour));
}

#[test]
fn spacing_overrides_beat_block_gap_and_default_to_it() {
    let paragraph = Block::Paragraph { prose: Prose::default(), quote: Vec::new() };
    let heading = Block::Heading { level: 2, prose: Prose::default(), id: None, classes: Vec::new() };
    let item = |n: u64| Block::ListItem { depth: 0, marker: format!("{n}."), ordinal: Some(n), list: 0, prose: Prose::default(), quote: Vec::new(), task: None };
    let mut style = MarkdownStyle::default();
    assert_eq!(gap_between(&paragraph, &heading, &style), 10);
    assert_eq!(gap_between(&heading, &paragraph, &style), 10);
    assert_eq!(gap_between(&Block::Rule, &paragraph, &style), 10);
    assert_eq!(gap_between(&item(1), &item(2), &style), 10);

    style.block_gap = 12;
    style.heading_margin_top = Some(24);
    style.heading_margin_bottom = Some(4);
    style.rule_margin = Some(30);
    style.list_item_gap = Some(2);
    assert_eq!(gap_between(&paragraph, &heading, &style), 24);
    assert_eq!(gap_between(&heading, &paragraph, &style), 4);
    assert_eq!(gap_between(&paragraph, &Block::Rule, &style), 30);
    assert_eq!(gap_between(&Block::Rule, &paragraph, &style), 30);
    assert_eq!(gap_between(&item(1), &item(2), &style), 2);
    assert_eq!(gap_between(&paragraph, &item(1), &style), 12);
    assert_eq!(gap_between(&heading, &heading, &style), 24);
}

// -- 4. Code -------------------------------------------------------------------------------------------------------------------------

fn tokens(language: &str, code: &str) -> Vec<(String, Vec4)> {
    let chars: Vec<char> = code.chars().collect();
    MarkdownHighlighter::default()
        .spans(language, code)
        .into_iter()
        .map(|s| (chars[s.start..s.start + s.len].iter().collect::<String>(), s.color))
        .collect()
}

fn colour_of(language: &str, code: &str, word: &str) -> Option<Vec4> {
    tokens(language, code).into_iter().find(|(text, _)| text == word).map(|(_, color)| color)
}

#[test]
fn highlighter_knows_every_requested_language() {
    for name in ["rust", "c", "c++", "cpp", "c#", "csharp", "java", "javascript", "js", "typescript", "ts", "python", "go", "json", "toml", "yaml", "shell", "bash", "sql", "html", "css", "markdown", "md", "rs,no_run", "RUST"] {
        assert!(MarkdownHighlighter::supports(name), "{name}");
    }
    assert!(!MarkdownHighlighter::supports("brainfuck"));
    assert!(!MarkdownHighlighter::supports(""));
    assert!(MarkdownHighlighter::default().spans("brainfuck", "+++").is_empty());
    assert_eq!(LANGUAGES.len(), 17);
}

#[test]
fn highlighter_colours_each_token_kind() {
    let c = super::CodeColours::default();
    let rust = "fn main() { let s: String = \"hi\"; // note\n let n = 42u8; }";
    assert_eq!(colour_of("rust", rust, "fn"), Some(c.keyword));
    assert_eq!(colour_of("rust", rust, "main"), Some(c.function));
    assert_eq!(colour_of("rust", rust, "String"), Some(c.ty));
    assert_eq!(colour_of("rust", rust, "\"hi\""), Some(c.string));
    assert_eq!(colour_of("rust", rust, "// note"), Some(c.comment));
    assert_eq!(colour_of("rust", rust, "42u8"), Some(c.number));
    assert_eq!(colour_of("rust", "x = 'a' + 'b;", "'a'"), Some(c.string));
    assert_eq!(colour_of("rust", "fn f<'a>(x: &'a str) {}", "'a"), Some(c.ty));
    assert_eq!(colour_of("rust", "let r = r#\"raw \" text\"#;", "r#\"raw \" text\"#"), Some(c.string));
    assert_eq!(colour_of("rust", "println!(\"x\");", "println"), Some(c.function));
    assert_eq!(colour_of("rust", "/* a\nb */ x", "/* a\nb */"), Some(c.comment));
}

#[test]
fn highlighter_handles_the_other_languages() {
    let c = super::CodeColours::default();
    assert_eq!(colour_of("c", "#include <stdio.h>\nint x;", "#include"), Some(c.keyword));
    assert_eq!(colour_of("c", "#include <stdio.h>\nint x;", "<stdio.h>"), Some(c.string));
    assert_eq!(colour_of("c", "#include <stdio.h>\nint x;", "int"), Some(c.ty));
    assert_eq!(colour_of("cpp", "template <class T> class Foo {};", "template"), Some(c.keyword));
    assert_eq!(colour_of("cpp", "template <class T> class Foo {};", "Foo"), Some(c.ty));
    assert_eq!(colour_of("csharp", "public static void Main() {}", "public"), Some(c.keyword));
    assert_eq!(colour_of("java", "@Override public void run() {}", "@Override"), Some(c.keyword));
    assert_eq!(colour_of("javascript", "const $el = `t`;", "`t`"), Some(c.string));
    assert_eq!(colour_of("javascript", "const $el = 1;", "const"), Some(c.keyword));
    assert_eq!(colour_of("typescript", "let x: number = 1;", "number"), Some(c.ty));
    assert_eq!(colour_of("python", "def f(): \"\"\"doc\nmore\"\"\"", "\"\"\"doc\nmore\"\"\""), Some(c.string));
    assert_eq!(colour_of("python", "x = 1  # c", "# c"), Some(c.comment));
    assert_eq!(colour_of("python", "class Foo(Base): pass", "Foo"), Some(c.ty));
    assert_eq!(colour_of("go", "func main() { x := 0x1F }", "func"), Some(c.keyword));
    assert_eq!(colour_of("go", "func main() { x := 0x1F }", "0x1F"), Some(c.number));
    assert_eq!(colour_of("json", "{\"a\": [true, 1.5e3, \"v\"]}", "\"a\""), Some(c.ty));
    assert_eq!(colour_of("json", "{\"a\": [true, 1.5e3, \"v\"]}", "\"v\""), Some(c.string));
    assert_eq!(colour_of("json", "{\"a\": [true, 1.5e3, \"v\"]}", "true"), Some(c.keyword));
    assert_eq!(colour_of("json", "{\"a\": [true, 1.5e3, \"v\"]}", "1.5e3"), Some(c.number));
    assert_eq!(colour_of("toml", "[package]\nname = \"x\"\n", "[package]"), Some(c.keyword));
    assert_eq!(colour_of("toml", "[package]\nname = \"x\"\n", "name"), Some(c.ty));
    assert_eq!(colour_of("yaml", "name: Bob # who\nok: true", "name"), Some(c.ty));
    assert_eq!(colour_of("yaml", "name: Bob # who\nok: true", "# who"), Some(c.comment));
    assert_eq!(colour_of("yaml", "name: Bob # who\nok: true", "true"), Some(c.keyword));
    assert_eq!(colour_of("sh", "if [ -f \"$HOME/x\" ]; then echo $FOO; fi", "if"), Some(c.keyword));
    assert_eq!(colour_of("sh", "echo $FOO", "$FOO"), Some(c.ty));
    assert_eq!(colour_of("sql", "SELECT id FROM t WHERE x = 'a' -- c", "SELECT"), Some(c.keyword));
    assert_eq!(colour_of("sql", "select 1", "select"), Some(c.keyword));
    assert_eq!(colour_of("sql", "SELECT id FROM t WHERE x = 'a' -- c", "'a'"), Some(c.string));
    assert_eq!(colour_of("html", "<!-- c --><a href=\"u\">x &amp; y</a>", "<!-- c -->"), Some(c.comment));
    assert_eq!(colour_of("html", "<a href=\"u\">x</a>", "a"), Some(c.keyword));
    assert_eq!(colour_of("html", "<a href=\"u\">x</a>", "href"), Some(c.ty));
    assert_eq!(colour_of("html", "<a href=\"u\">x</a>", "\"u\""), Some(c.string));
    assert_eq!(colour_of("html", "x &amp; y", "&amp;"), Some(c.number));
    assert_eq!(colour_of("css", "a { color: #fff; margin: 10px; } /* c */", "color"), Some(c.ty));
    assert_eq!(colour_of("css", "a { color: #fff; margin: 10px; } /* c */", "#fff"), Some(c.number));
    assert_eq!(colour_of("css", "a { color: #fff; margin: 10px; } /* c */", "10px"), Some(c.number));
    assert_eq!(colour_of("css", "@media screen {}", "@media"), Some(c.keyword));
    assert_eq!(colour_of("markdown", "# Title\n> quote\n`code` and [t](http://u)", "# Title"), Some(c.keyword));
    assert_eq!(colour_of("markdown", "# Title\n> quote\n`code` and [t](http://u)", "> quote"), Some(c.comment));
    assert_eq!(colour_of("markdown", "# Title\n> quote\n`code` and [t](http://u)", "`code`"), Some(c.string));
}

#[test]
fn highlighter_spans_are_sorted_non_overlapping_and_in_range() {
    for (language, code) in [("rust", "fn é() { \"ü\" }"), ("html", "<p a='b'>é</p>"), ("markdown", "- *é* `x`"), ("python", "\"\"\"unterminated"), ("rust", "r##\"open"), ("json", "\"open"), ("c", "/* open")] {
        let total = code.chars().count();
        let spans = MarkdownHighlighter::default().spans(language, code);
        let mut at = 0;
        for span in spans {
            assert!(span.start >= at && span.len > 0 && span.start + span.len <= total, "{language}: {span:?} in {total}");
            at = span.start + span.len;
        }
    }
    assert!(MarkdownHighlighter::default().spans("rust", "").is_empty());
}

#[test]
fn highlighter_uses_the_style_colours_and_plugs_into_the_hook() {
    let green = Vec4::new(0.0, 1.0, 0.0, 1.0);
    let colours = CodeColours { keyword: green, ..CodeColours::default() };
    let colorizer = MarkdownHighlighter::new(colours).colorizer();
    let spans = (colorizer.0)("rust", "fn x() {}");
    assert_eq!(spans[0], CodeSpan { start: 0, len: 2, color: green });

    let mut style = MarkdownStyle { code_colours: colours, ..MarkdownStyle::default() };
    let plain = code_prose("rust", "fn x() {}", &style);
    assert_eq!(plain.runs.len(), 1);
    style.highlight_code = true;
    let coloured = code_prose("rust", "fn x() {}", &style);
    assert_eq!(coloured.runs[0].2.color, Some(pack_color(green)));
    assert_eq!(coloured.runs.iter().map(|(_, len, _)| len).sum::<usize>(), 9);
    // An explicit colorizer wins over the built-in one.
    style.code_colorizer = Some(CodeColorizer::new(|_, code| vec![CodeSpan { start: 0, len: code.chars().count(), color: Vec4::ONE }]));
    assert_eq!(code_prose("rust", "fn x() {}", &style).runs.len(), 1);
}

#[test]
fn code_prose_covers_every_char_even_with_wild_colorizer_output() {
    let style = MarkdownStyle {
        code_colorizer: Some(CodeColorizer::new(|_, _| {
            vec![
                CodeSpan { start: 5, len: 100, color: Vec4::ONE },
                CodeSpan { start: 1, len: 2, color: Vec4::ONE },
                CodeSpan { start: 2, len: 2, color: Vec4::ONE },
                CodeSpan { start: usize::MAX, len: 4, color: Vec4::ONE },
            ]
        })),
        ..MarkdownStyle::default()
    };
    let prose = code_prose("x", "ünïcödé!!", &style);
    let mut at = 0;
    for (start, len, _) in &prose.runs {
        assert_eq!(*start, at);
        at += len;
    }
    assert_eq!(at, 9);
}

// -- 5. Behaviour --------------------------------------------------------------------------------------------------------------------

const DOCUMENT: &str = "# Intro {#start}\n\nHello **world** with [a link](https://a.example) and [[Wiki]].\n\n- one\n- [x] two\n\n| h1 | h2 |\n|----|----|\n| c1 | c2 |\n\n```rust\nlet x = 1;\n```\n\n![alt text](img.png)\n\n> [!NOTE]\n> Remember.\n\nTerm\n:   Detail\n\nFoot[^1].\n\n[^1]: The note.\n\n[![b](b.svg)](https://badge.example) and https://a.example again\n";

#[test]
fn plain_text_has_no_markup() {
    let view = MarkdownView::new(DOCUMENT);
    let text = view.plain_text();
    assert!(text.starts_with("Intro\n\nHello world with a link and Wiki.\n\n\u{2022} one\n\u{2022} [x] two\n\nh1\th2\nc1\tc2\n\nlet x = 1;\n\nalt text\n\nNote\nRemember.\n\nTerm\nDetail\n\n"), "{text:?}");
    assert!(text.contains("Foot\u{b9}.") && text.contains("1. The note."));
    assert!(!text.contains('*') && !text.contains('#') && !text.contains("]("));
    assert_eq!(plain_text_of(&[]), "");
}

#[test]
fn links_lists_every_target_once_in_order() {
    let view = MarkdownView::new(DOCUMENT);
    assert_eq!(view.links(), vec!["https://a.example", "Wiki", "https://badge.example"]);
    assert!(links_of(&[]).is_empty());
}

#[test]
fn headings_and_anchors_can_be_scrolled_to() {
    let mut view = MarkdownView::new(DOCUMENT);
    let headings = view.headings();
    assert_eq!(headings.len(), 1);
    assert_eq!((headings[0].level, headings[0].text.as_str(), headings[0].id.as_deref()), (1, "Intro", Some("start")));
    assert!(view.scroll_to("start"));
    assert!(view.scroll_to("#start"));
    assert!(view.scroll_to("fn:1"));
    assert!(view.scroll_to("fnref:1"));
    assert!(!view.scroll_to("missing"));
}

#[test]
fn task_ranges_let_the_app_rewrite_its_source() {
    let view = MarkdownView::new("- [ ] a\n- [x] b\n");
    let range = view.task_source_range(0).unwrap();
    assert_eq!(&view.source()[range], "[ ]");
    assert_eq!(view.task_source_range(5), None);
}

#[test]
fn set_source_replaces_the_document() {
    let mut view = MarkdownView::new("# one");
    view.set_source("two [l](u)");
    assert_eq!(view.plain_text(), "two l");
    assert_eq!(view.links(), vec!["u"]);
    view.set_source("");
    assert_eq!(view.plain_text(), "");
}

#[test]
fn image_sizing_scales_to_the_column_and_max_height() {
    let size = Vec2::new(800.0, 400.0);
    assert_eq!(fit_image(size, Some(400.0), 1000.0, false), Vec2::new(400.0, 200.0));
    assert_eq!(fit_image(size, Some(1600.0), 1000.0, false), size);
    assert_eq!(fit_image(size, Some(1600.0), 1000.0, true), Vec2::new(1600.0, 800.0));
    assert_eq!(fit_image(size, Some(400.0), 100.0, false), Vec2::new(200.0, 100.0));
    assert_eq!(fit_image(size, None, 0.0, false), size);
    assert_eq!(fit_image(Vec2::ZERO, Some(400.0), 100.0, false), Vec2::ZERO);
    assert_eq!(fit_image(Vec2::new(-1.0, 5.0), Some(400.0), 100.0, false), Vec2::ZERO);
}

#[test]
fn parsing_hostile_input_does_not_panic() {
    let nasty = [
        "",
        "\0",
        "[^",
        "[^1]: ",
        "[^a]: [^a]",
        "> [!NOTE]",
        "> > > > [!TIP]\n> > > >",
        "$$",
        "$",
        "[[",
        "[[]]",
        "![](",
        "![[x]](y)",
        "~~~~",
        "^^ ~~ __ **",
        "# {#}",
        "# {#a} {#a}",
        "| a |\n|:-:|\n",
        "|||\n|-|\n|![i](u)|",
        "term\n: ",
        "- [ ]",
        "- [x]\n  - [ ] \n    ```\n",
        "www. http:// https://\u{0}",
        "(https://a.io/)))",
        "😀 https://😀.example/😀 www.😀",
    ];
    for source in nasty {
        for smart in [false, true] {
            let style = MarkdownStyle { smart_punctuation: smart, underline_syntax: smart, soft_break: SoftBreak::Newline, ..MarkdownStyle::default() };
            let mut view = MarkdownView::with_style(source, &style);
            let _ = (view.plain_text(), view.links(), view.headings(), view.scroll_to("x"));
            view.set_source(source);
        }
    }
}


#[test]
fn spans_that_look_alike_are_merged_so_the_engine_can_wrap() {
    use crate::widgets::RichSpan;
    let plain = RichSpan { size_scale: 1.0, ..RichSpan::default() };
    let spans = vec![
        RichSpan { start: 0, len: 3, ..plain },
        RichSpan { start: 3, len: 2, ..plain },
        RichSpan { start: 5, len: 1, underline: true, ..plain },
        RichSpan { start: 6, len: 1, underline: true, ..plain },
        RichSpan { start: 8, len: 1, underline: true, ..plain },
    ];
    let merged = super::view::merge_spans(spans);
    assert_eq!(merged.iter().map(|s| (s.start, s.len, s.underline)).collect::<Vec<_>>(), vec![(0, 5, false), (5, 2, true), (8, 1, true)]);
    assert!(super::view::merge_spans(Vec::new()).is_empty());
}

// -- LaTeX ---------------------------------------------------------------------------------------------------------------------------

mod latex {
    use super::super::tex::{parse, tex_to_unicode, to_unicode, style_char, AccentKind, ColumnAlignment, SymbolClass, TexFont, TexNode};

    fn uni(source: &str) -> String {
        tex_to_unicode(source).unwrap_or_else(|| panic!("{source} did not parse"))
    }

    #[test]
    fn symbols_greek_and_operators_become_unicode() {
        assert_eq!(uni("\\alpha + \\beta = \\gamma"), "α + β = γ");
        assert_eq!(uni("a \\leq b \\neq c \\to d"), "a ≤ b ≠ c → d");
        assert_eq!(uni("x \\in \\mathbb{R}"), "x ∈ ℝ");
        assert_eq!(uni("\\forall x \\exists y"), "∀x∃y");
        assert_eq!(uni("\\infty \\partial \\nabla"), "∞∂∇");
        assert_eq!(uni("a - b"), "a − b");
        assert_eq!(uni("\\pm 1"), "± 1");
    }

    #[test]
    fn scripts_use_unicode_forms_or_fall_back_to_markers() {
        assert_eq!(uni("x^2"), "x²");
        assert_eq!(uni("a_{i+1}"), "a_(i + 1)");
        assert_eq!(uni("a_{n+1}"), "aₙ₊₁");
        assert_eq!(uni("e^{i\\pi}"), "e^(iπ)");
        assert_eq!(uni("x_n^2"), "xₙ²");
        assert_eq!(uni("f'"), "f′");
        assert_eq!(uni("x^{y}"), "x^y");
    }

    #[test]
    fn scripts_on_plain_bases_become_raised_and_lowered_runs() {
        use crate::markdown::tex::{to_runs, ScriptLevel::{Normal, Subscript, Superscript}};
        let runs = |source: &str| to_runs(&parse(source).unwrap());
        assert_eq!(runs("e^{i\\pi}+1=0"), vec![("e".to_owned(), Normal), ("iπ".to_owned(), Superscript), (" + 1 = 0".to_owned(), Normal)]);
        assert_eq!(runs("x_n^2"), vec![("x".to_owned(), Normal), ("n".to_owned(), Subscript), ("2".to_owned(), Superscript)]);
        assert_eq!(runs("a_{i+1} b"), vec![("a".to_owned(), Normal), ("i + 1".to_owned(), Subscript), ("b".to_owned(), Normal)]);
        assert_eq!(runs("\\alpha"), vec![("α".to_owned(), Normal)]);
        // No scripts: one run, the same text as `to_unicode`.
        assert_eq!(runs("\\frac{a}{b} \\leq c"), vec![("a/b ≤ c".to_owned(), Normal)]);
        assert!(runs("").is_empty());
    }

    #[test]
    fn fractions_roots_and_accents() {
        assert_eq!(uni("\\frac{a}{b}"), "a/b");
        assert_eq!(uni("\\frac{a+b}{c}"), "(a + b)/c");
        assert_eq!(uni("\\sqrt{x}"), "√x");
        assert_eq!(uni("\\sqrt{x+1}"), "√(x + 1)");
        assert_eq!(uni("\\sqrt[3]{8}"), "∛8");
        assert_eq!(uni("\\hat{x}"), "x\u{302}");
        assert_eq!(uni("\\vec{v}"), "v\u{20d7}");
        assert_eq!(uni("\\binom{n}{k}"), "(n k)");
    }

    #[test]
    fn big_operators_functions_and_text() {
        assert_eq!(uni("\\sum_{i=1}^{n} i"), "∑_(i = 1)ⁿi");
        assert_eq!(uni("\\int_0^1 x\\,dx"), "∫₀¹x\u{2009}dx");
        assert_eq!(uni("\\sin x + \\log y"), "sin\u{2009}x + log\u{2009}y");
        assert_eq!(uni("\\lim_{x \\to 0} f"), "lim\u{2009}_(x → 0)f");
        assert_eq!(uni("\\text{if } x > 0"), "if x > 0");
    }

    #[test]
    fn delimiters_matrices_and_environments() {
        assert_eq!(uni("\\left( \\frac{a}{b} \\right)"), "(a/b)");
        assert_eq!(uni("\\left\\{ x \\right."), "{x");
        assert_eq!(uni("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}"), "(a, b; c, d)");
        let Ok(TexNode::Matrix { rows, open, close, align }) = parse("\\begin{cases} 1 & x>0 \\\\ 0 & \\text{else} \\end{cases}") else { panic!() };
        assert_eq!((rows.len(), rows[0].len(), open, close, align), (2, 2, Some('{'), None, ColumnAlignment::Left));
        let Ok(TexNode::Matrix { rows, .. }) = parse("\\begin{matrix} 1 & 2 \\\\ 3 & 4 \\\\ \\end{matrix}") else { panic!() };
        assert_eq!(rows.len(), 2, "a trailing \\\\ adds no row");
    }

    #[test]
    fn font_commands_map_alphabets() {
        assert_eq!(style_char(TexFont::Blackboard, 'N'), 'ℕ');
        assert_eq!(style_char(TexFont::Blackboard, 'A'), '𝔸');
        assert_eq!(style_char(TexFont::Calligraphic, 'L'), 'ℒ');
        assert_eq!(style_char(TexFont::Fraktur, 'g'), '𝔤');
        assert_eq!(style_char(TexFont::Bold, 'x'), 'x');
        assert_eq!(uni("\\mathcal{F}"), "ℱ");
        assert_eq!(uni("\\mathbf{v}"), "v");
    }

    #[test]
    fn tree_shape_and_classes() {
        let Ok(TexNode::Row(items)) = parse("a+b") else { panic!() };
        assert!(matches!(&items[1], TexNode::Symbol { ch: '+', class: SymbolClass::Binary }));
        assert!(matches!(parse("x^2"), Ok(TexNode::Script { sub: None, sup: Some(_), .. })));
        assert!(matches!(parse("\\overline{x}"), Ok(TexNode::Accent { kind: AccentKind::Overline, .. })));
        assert!(matches!(parse("12.5"), Ok(TexNode::Number(n)) if n == "12.5"));
        assert!(matches!(parse("\\lim"), Ok(TexNode::Operator { limits: true, .. })));
        assert!(matches!(parse("\\sin"), Ok(TexNode::Operator { limits: false, .. })));
    }

    #[test]
    fn unknown_commands_are_shown_as_written_and_errors_are_reported() {
        assert_eq!(uni("\\frobnicate x"), "\\frobnicatex");
        for bad in ["\\frac{a}", "{a", "a}", "\\left( x", "x \\right)", "\\begin{pmatrix} a", "\\begin{nonsense} \\end{nonsense}", "a & b", "x^", "\\end{x}", "\\\\"] {
            assert!(parse(bad).is_err(), "{bad:?} should fail");
        }
        assert_eq!(parse("a & b").unwrap_err().message, "unexpected &");
    }

    #[test]
    fn hostile_input_neither_panics_nor_overflows_the_stack() {
        let deep = "{".repeat(5000) + &"}".repeat(5000);
        assert!(parse(&deep).is_err());
        let deep_frac = "\\frac{".repeat(2000);
        assert!(parse(&deep_frac).is_err());
        for src in ["", " ", "\\", "\\\\\\", "^_", "{}^{}_{}", "😀 \\alpha 😀", "\u{0}", "\\begin", "\\begin{", "\\text{", "\\sqrt[", "\\left", "'''", "1.2.3.4", ".", "..5"] {
            let _ = parse(src).map(|n| to_unicode(&n));
        }
        assert_eq!(uni(""), "");
    }
}

// -- Feature switches, source ranges, editor pieces ---------------------------------------------------------------------------------

mod features {
    use super::*;
    use crate::markdown::{inline_text, BlockKind, MarkdownEditor, MarkdownFeatures, MarkdownLinkKind, MarkdownViews, MathMode};
    use super::super::view::{approx_source_offset, expand_to_lines};

    fn with(features: MarkdownFeatures) -> MarkdownStyle {
        MarkdownStyle { features, ..MarkdownStyle::default() }
    }

    fn texts(source: &str, style: &MarkdownStyle) -> Vec<String> {
        parse_style(source, style)
            .blocks
            .iter()
            .map(|b| match b {
                Block::Paragraph { prose, .. } | Block::Heading { prose, .. } | Block::ListItem { prose, .. } => prose.text.clone(),
                Block::Code { text, .. } => format!("code:{text}"),
                other => format!("{other:?}").chars().take(12).collect(),
            })
            .collect()
    }

    #[test]
    fn disabled_blocks_show_their_source_as_written() {
        let source = "# Title\n\n- a\n- b\n\n> quote\n\n```rs\nx\n```\n\n---\n";
        assert_eq!(texts(source, &MarkdownStyle::default()).len(), 6);
        assert_eq!(texts(source, &with(MarkdownFeatures { headings: false, ..MarkdownFeatures::all() }))[0], "# Title");
        let literal = texts(source, &with(MarkdownFeatures::none()));
        assert_eq!(literal, vec!["# Title", "- a\n- b", "> quote", "```rs\nx\n```", "---"]);
        let no_lists = texts(source, &with(MarkdownFeatures { lists: false, ..MarkdownFeatures::all() }));
        assert!(no_lists.contains(&"- a\n- b".to_owned()));
        let no_code = texts(source, &with(MarkdownFeatures { code_blocks: false, ..MarkdownFeatures::all() }));
        assert!(no_code.contains(&"```rs\nx\n```".to_owned()));
        let no_rules = texts(source, &with(MarkdownFeatures { rules: false, ..MarkdownFeatures::all() }));
        assert_eq!(no_rules.last().map(String::as_str), Some("---"));
        assert!(!parse_style(source, &with(MarkdownFeatures { rules: false, ..MarkdownFeatures::all() })).blocks.contains(&Block::Rule));
    }

    #[test]
    fn disabled_inline_constructs_keep_their_markers() {
        let source = "a **b** *c* `d` [e](u) ~~f~~ $x$";
        let text = |features| paragraph(&parse_style(source, &with(features)).blocks).text.clone();
        assert_eq!(text(MarkdownFeatures::all()), "a b c d e f x");
        assert_eq!(text(MarkdownFeatures { strong: false, ..MarkdownFeatures::all() }), "a **b** c d e f x");
        assert_eq!(text(MarkdownFeatures { emphasis: false, ..MarkdownFeatures::all() }), "a b *c* d e f x");
        assert_eq!(text(MarkdownFeatures { inline_code: false, ..MarkdownFeatures::all() }), "a b c `d` e f x");
        assert_eq!(text(MarkdownFeatures { links: false, ..MarkdownFeatures::all() }), "a b c d [e](u) f x");
        assert_eq!(text(MarkdownFeatures { strikethrough: false, ..MarkdownFeatures::all() }), "a b c d e ~~f~~ x");
        assert_eq!(text(MarkdownFeatures { math: false, ..MarkdownFeatures::all() }), "a b c d e f $x$");
        let parsed = parse_style("see ![g](h) end", &with(MarkdownFeatures { images: false, ..MarkdownFeatures::all() }));
        assert!(!parsed.blocks.iter().any(|b| matches!(b, Block::Image { .. })));
        assert_eq!(paragraph(&parsed.blocks).text, "see ![g](h) end");
        assert_eq!(text(MarkdownFeatures::none()), source);
        // A disabled link does not leave link runs behind.
        let parsed = parse_style("[e](u) after", &with(MarkdownFeatures { links: false, ..MarkdownFeatures::all() }));
        assert!(paragraph(&parsed.blocks).links.is_empty());
    }

    #[test]
    fn disabled_nested_constructs_do_not_leak_events() {
        let style = with(MarkdownFeatures { strong: false, ..MarkdownFeatures::all() });
        let parsed = parse_style("x **a *b* [l](u)** y\n\nnext", &style);
        assert_eq!(parsed.blocks.len(), 2);
        assert_eq!(paragraph(&parsed.blocks).text, "x **a *b* [l](u)** y");
        assert!(matches!(&parsed.blocks[1], Block::Paragraph { prose, .. } if prose.text == "next"));
    }

    #[test]
    fn extension_switches_leave_the_syntax_literal() {
        let case = |features: MarkdownFeatures, source: &str| texts(source, &with(features)).join("|");
        let all = MarkdownFeatures::all();
        assert_eq!(case(MarkdownFeatures { tables: false, ..all }, "| a |\n|---|\n| 1 |"), "| a | |---| | 1 |");
        assert_eq!(case(MarkdownFeatures { task_lists: false, ..all }, "- [x] done"), "[x] done");
        assert_eq!(case(MarkdownFeatures { footnotes: false, ..all }, "x[^1]\n\n[^1]: n"), "x^1", "plain CommonMark: a reference link with a definition");
        assert_eq!(case(MarkdownFeatures { heading_attributes: false, ..all }, "# T {#id}"), "T {#id}");
        assert_eq!(case(MarkdownFeatures { superscript: false, ..all }, "a ^b^ c"), "a ^b^ c");
        assert_eq!(case(MarkdownFeatures { subscript: false, ..all }, "a ~b~ c"), "a b c", "`~b~` falls back to strikethrough");
        assert_eq!(case(MarkdownFeatures { wikilinks: false, ..all }, "[[Page]]"), "[[Page]]");
        assert_eq!(case(MarkdownFeatures { definition_lists: false, ..all }, "T\n: d"), "T : d");
        assert_eq!(case(MarkdownFeatures { alerts: false, ..all }, "> [!NOTE]\n> hi"), "[!NOTE] hi");
        let commonmark = MarkdownFeatures::commonmark();
        assert!(!commonmark.tables && !commonmark.math && commonmark.headings && commonmark.links);
        let chat = MarkdownFeatures::chat();
        assert!(chat.strong && chat.code_blocks && chat.lists && !chat.headings && !chat.tables);
        assert_eq!(MarkdownFeatures::default(), MarkdownFeatures::all());
    }

    #[test]
    fn changing_a_feature_reparses_a_live_view() {
        let mut view = MarkdownView::with_style("# T", &MarkdownStyle::default());
        assert_eq!(view.headings().len(), 1);
        let off = with(MarkdownFeatures { headings: false, ..MarkdownFeatures::all() });
        // prepare() is what show_with calls first; it needs no UI.
        view.set_source("# T");
        let blocks = parse_style("# T", &off).blocks;
        assert!(matches!(&blocks[0], Block::Paragraph { .. }));
        assert_ne!(ParseConfig::from_style(&off), ParseConfig::from_style(&MarkdownStyle::default()));
    }

    #[test]
    fn math_modes_choose_unicode_layout_or_source() {
        let source = "$\\alpha^2$ and\n\n$$\\frac{a}{b}$$\n";
        let style = |math_mode| MarkdownStyle { math_mode, ..MarkdownStyle::default() };
        for (mode, inline, display_has_expr, display_text) in [
            (MathMode::Layout, "α2 and", true, None),
            (MathMode::Unicode, "α² and", false, Some("a/b")),
            (MathMode::Source, "\\alpha^2 and", false, None),
        ] {
            let parsed = parse_style(source, &style(mode));
            assert_eq!(paragraph(&parsed.blocks).text, inline, "{mode:?}");
            let Some(Block::Math { expr, render, .. }) = parsed.blocks.iter().find(|b| matches!(b, Block::Math { .. })) else { panic!() };
            assert_eq!(expr.is_some(), display_has_expr, "{mode:?}");
            assert_eq!(render.as_ref().map(|r| matches!(r, MathRender::Text(t) if Some(t.as_str()) == display_text)), display_text.map(|_| true), "{mode:?}");
        }
        // A formula that does not parse stays as written in every mode.
        let parsed = parse_style("$\\frac{a}$ and $$\\frac{a}$$", &style(MathMode::Layout));
        assert_eq!(paragraph(&parsed.blocks).text.trim_end(), "\\frac{a} and");
        assert!(matches!(parsed.blocks.iter().find(|b| matches!(b, Block::Math { .. })), Some(Block::Math { expr: None, render: None, .. })));
    }

    #[test]
    fn blocks_know_their_source_lines() {
        let source = "# Title\n\nSome *text*\nover two lines.\n\n- one\n  - nested\n- two\n\n```rs\nfn x() {}\n```\n\n> [!NOTE]\n> inside\n\n| a |\n|---|\n| 1 |\n";
        let view = MarkdownView::new(source);
        let slice = |i: usize| &source[view.blocks()[i].source.clone()];
        let kinds: Vec<_> = view.blocks().iter().map(|b| b.kind).collect();
        assert_eq!(
            kinds,
            vec![
                BlockKind::Heading(1),
                BlockKind::Paragraph,
                BlockKind::ListItem,
                BlockKind::ListItem,
                BlockKind::ListItem,
                BlockKind::Code,
                BlockKind::AlertTitle,
                BlockKind::Paragraph,
                BlockKind::Table
            ]
        );
        assert_eq!(slice(0), "# Title");
        assert_eq!(slice(1), "Some *text*\nover two lines.");
        assert_eq!(slice(2), "- one");
        assert_eq!(slice(3), "  - nested");
        assert_eq!(slice(4), "- two");
        assert_eq!(slice(5), "```rs\nfn x() {}\n```");
        assert_eq!(slice(6), "> [!NOTE]");
        assert_eq!(slice(7), "> inside");
        assert_eq!(slice(8), "| a |\n|---|\n| 1 |");
        assert_eq!(view.blocks()[1].text, "Some text over two lines.");
    }

    #[test]
    fn block_lookup_by_offset_and_source_offset_of_a_click() {
        let source = "# Title\n\nHello **bold** world\n";
        let view = MarkdownView::new(source);
        assert_eq!(view.block_at_offset(0), Some(0));
        assert_eq!(view.block_at_offset(source.find("Hello").unwrap() + 3), Some(1));
        assert_eq!(view.block_at_offset(source.len() + 50), None);
        // Character 8 of "Hello bold world" is the 'l' of "bold", after the `**`.
        let offset = view.source_offset(1, 8).unwrap();
        assert_eq!(&source[offset..offset + 4], "ld**");
        assert_eq!(view.source_offset(9, 0), None);
        assert_eq!(approx_source_offset("a **b** c", "a b c", 2), 4);
        assert_eq!(approx_source_offset("plain", "plain", 3), 3);
        assert_eq!(approx_source_offset("", "x", 0), 0);
        assert_eq!(approx_source_offset("ab", "abcdef", 5), 2);
    }

    #[test]
    fn expanding_ranges_to_whole_lines() {
        let s = "one\ntwo three\nfour\r\nfive";
        assert_eq!(&s[expand_to_lines(s, &(5..8))], "two three");
        assert_eq!(&s[expand_to_lines(s, &(4..14))], "two three");
        assert_eq!(&s[expand_to_lines(s, &(0..23))], s);
        assert_eq!(&s[expand_to_lines(s, &(14..18))], "four");
        assert_eq!(&s[expand_to_lines(s, &(100..200))], "five", "past the end means the last line");
        assert_eq!(expand_to_lines("", &(0..0)), 0..0);
        assert_eq!(&"é\nü"[expand_to_lines("é\nü", &(3..3))], "ü");
    }

    #[test]
    fn inline_text_gives_spans_for_other_widgets() {
        let style = MarkdownStyle { bold_font: 2, link: Vec4::new(0.0, 0.0, 1.0, 1.0), ..MarkdownStyle::default() };
        let inline = inline_text("Hi **there** [site](https://x.io \"T\") and `code`, [[Wiki]] # not a heading", &style);
        assert_eq!(inline.text, "Hi there site and code, Wiki # not a heading");
        assert!(inline.spans.iter().any(|s| s.font_id == Some(2)));
        assert_eq!(inline.links.len(), 2);
        assert_eq!((inline.links[0].url.as_str(), inline.links[0].title.as_str(), inline.links[0].kind), ("https://x.io", "T", MarkdownLinkKind::External));
        assert_eq!(inline.links[1].kind, MarkdownLinkKind::Wiki);
        let at = inline.text.find("site").unwrap();
        assert_eq!(inline.link_at(at).map(|l| l.url.as_str()), Some("https://x.io"));
        assert!(inline.link_at(0).is_none());
        // Block syntax stays literal, paragraphs join with a newline.
        let blocks = inline_text("- not a list\n\n> not a quote", &style);
        assert_eq!(blocks.text, "- not a list\n> not a quote");
        assert_eq!(inline_text("", &style).text, "");
        // The style's features still apply.
        let plain = MarkdownStyle { features: MarkdownFeatures { strong: false, ..MarkdownFeatures::all() }, ..MarkdownStyle::default() };
        assert_eq!(inline_text("**x**", &plain).text, "**x**");
    }

    #[test]
    fn markdown_views_cache_per_key_and_evict_the_least_recently_used() {
        let style = MarkdownStyle::default();
        let mut views = MarkdownViews::new(10);
        assert!(views.is_empty());
        assert_eq!(views.view_for("a", "# one", &style).plain_text(), "one");
        assert_eq!(views.view_for("a", "# two", &style).plain_text(), "two", "changed text re-parses");
        assert_eq!(views.len(), 1);
        for i in 0..10 {
            views.view_for(&format!("k{i}"), "x", &style);
            views.view_for("a", "# two", &style); // keep "a" fresh
        }
        assert!(views.len() <= 10);
        assert!(views.get_mut("a").is_some(), "the recently used view survives");
        assert!(views.get_mut("k0").is_none(), "the oldest was evicted");
        assert!(views.remove("a") && !views.remove("a"));
        views.clear();
        assert!(views.is_empty());
        assert_eq!(MarkdownViews::new(0).len(), 0);
    }

    #[test]
    fn editor_edits_the_lines_of_a_block() {
        let mut editor = MarkdownEditor::new("# Title\n\nHello world\n\nlast");
        assert!(!editor.is_editing());
        assert!(editor.edit_block(1, Some(14)));
        assert_eq!(editor.editing_range(), Some(9..20));
        assert!(!editor.edit_block(99, None));
        editor.stop_editing();
        assert_eq!(editor.editing_range(), None);
        assert!(editor.edit_at(2));
        assert_eq!(editor.editing_range(), Some(0..7));
        editor.edit_range(24..24, None);
        assert_eq!(editor.editing_range(), Some(22..26));
        editor.set_source("new");
        assert!(!editor.is_editing());
        assert_eq!(editor.source(), "new");
        assert_eq!(editor.view().plain_text(), "new");
        let mut empty = MarkdownEditor::new("");
        empty.edit_range(0..0, None);
        assert_eq!(empty.editing_range(), Some(0..0));
    }

    #[test]
    fn footnote_rule_has_no_source_and_cannot_be_edited() {
        let mut editor = MarkdownEditor::new("x[^1]\n\n[^1]: note\n");
        let rule = editor.view().blocks().iter().position(|b| b.kind == BlockKind::Rule).unwrap();
        assert!(editor.view().blocks()[rule].source.is_empty());
        assert!(!editor.edit_block(rule, None));
        let footnote = editor.view().blocks().iter().position(|b| b.kind == BlockKind::Footnote).unwrap();
        assert!(editor.edit_block(footnote, None));
    }
}

// -- Hover, metrics, selection across blocks ---------------------------------------------------------------------------------------

mod interaction {
    use super::*;
    use crate::markdown::{LineStyle, LinkUnderline};
    use super::super::view::{test_cell_style, test_hover_spans};

    #[test]
    fn link_index_at_finds_the_link_under_a_char() {
        let blocks = parse("a [one](https://one.example) b [two](https://two.example)");
        let prose = paragraph(&blocks);
        assert_eq!(prose.link_index_at(0), None);
        assert_eq!(prose.link_index_at(2), Some(0));
        assert_eq!(prose.link_index_at(prose.text.find("two").unwrap()), Some(1));
        assert_eq!(prose.link_index_at(500), None);
    }

    #[test]
    fn hover_and_pressed_colours_apply_to_the_hovered_link_only() {
        let style = MarkdownStyle {
            link: Vec4::new(0.0, 0.0, 1.0, 1.0),
            link_hover: Some(Vec4::new(0.5, 0.5, 1.0, 1.0)),
            link_pressed: Some(Vec4::new(1.0, 0.5, 0.0, 1.0)),
            ..MarkdownStyle::default()
        };
        let blocks = parse_style("[a](https://a.io) and [b](https://b.io)", &style).blocks;
        let prose = paragraph(&blocks);
        let colours = |spans: Vec<crate::widgets::RichSpan>| spans.into_iter().filter(|s| s.color.is_some()).map(|s| s.color).collect::<Vec<_>>();
        assert_eq!(colours(test_hover_spans(prose, &style, 1, false)), vec![Some(style.link), style.link_hover]);
        assert_eq!(colours(test_hover_spans(prose, &style, 0, true)), vec![style.link_pressed, Some(style.link)]);
        // Without a pressed colour the hover colour is used; without either the link keeps its colour.
        let hover_only = MarkdownStyle { link_pressed: None, ..style.clone() };
        assert_eq!(colours(test_hover_spans(prose, &hover_only, 0, true))[0], style.link_hover);
        let none = MarkdownStyle { link_hover: None, link_pressed: None, ..style.clone() };
        assert_eq!(colours(test_hover_spans(prose, &none, 0, true)), vec![Some(style.link), Some(style.link)]);
    }

    #[test]
    fn underline_on_hover_style_thickness_and_colour() {
        let style = MarkdownStyle {
            link_underline: LinkUnderline::OnHover,
            link_underline_style: LineStyle::Dotted,
            link_underline_thickness: 2.0,
            link_underline_color: Some(Vec4::new(1.0, 0.0, 0.0, 1.0)),
            ..MarkdownStyle::default()
        };
        let blocks = parse_style("[a](https://a.io) and [b](https://b.io)", &style).blocks;
        let prose = paragraph(&blocks);
        let links = |spans: Vec<crate::widgets::RichSpan>| spans.into_iter().filter(|s| s.color.is_some()).collect::<Vec<_>>();
        let hovered = links(test_hover_spans(prose, &style, 0, false));
        assert_eq!((hovered[0].underline, hovered[1].underline), (true, false));
        assert_eq!(hovered[0].line_style, LineStyle::Dotted);
        assert_eq!(hovered[0].line_thickness, 2.0);
        assert_eq!(hovered[0].line_color, style.link_underline_color);
        let resting = links(super::cell_spans("[a](https://a.io)", &style, &[]));
        assert!(resting.iter().all(|s| !s.underline));
        // Text that is not a link keeps the default line style.
        let plain = super::cell_spans("~~gone~~", &style, &[]);
        assert_eq!((plain[0].line_style, plain[0].line_thickness, plain[0].line_color), (LineStyle::Solid, 0.0, None));
        let defaults = MarkdownStyle::default();
        assert_eq!((defaults.link_hover, defaults.link_pressed, defaults.link_underline_style, defaults.link_underline_thickness), (None, None, LineStyle::Solid, 0.0));
        assert!(defaults.link_tooltips && defaults.link_underline == LinkUnderline::Always);
    }

    #[test]
    fn line_height_letter_spacing_and_code_wrapping_reach_the_text_style() {
        let style = MarkdownStyle { line_height: 20, letter_spacing: 2, code_line_height: 14, code_wrap: false, ..MarkdownStyle::default() };
        let prose = Prose::default();
        let body = test_cell_style(&prose, &style, 1.0, false);
        assert_eq!((body.line_height, body.letter_spacing, body.wrap_lines), (20, 2, true));
        // A heading's line grows with its text.
        assert_eq!(test_cell_style(&prose, &style, 1.5, false).line_height, 30);
        let code = test_cell_style(&prose, &style, 1.0, true);
        assert_eq!((code.line_height, code.wrap_lines), (14, false));
        let wrapped = MarkdownStyle { code_wrap: true, ..style.clone() };
        assert!(test_cell_style(&prose, &wrapped, 1.0, true).wrap_lines);
        let defaults = MarkdownStyle::default();
        assert_eq!((defaults.line_height, defaults.letter_spacing, defaults.code_line_height), (0, 0, 0));
        assert!(defaults.code_wrap);
        assert_eq!(test_cell_style(&prose, &defaults, 1.8, false).line_height, 0, "0 stays the font's own");
    }

    #[test]
    fn selected_text_joins_blocks_with_blank_lines_and_cells_with_newlines() {
        let mut view = MarkdownView::new("First paragraph.\n\nSecond one.\n\n| a | b |\n|---|---|\n| c | d |\n\nLast.");
        assert_eq!(view.selected_text(), "");
        // Cells in document order: paragraph, paragraph, a, b, c, d, paragraph.
        view.test_select(&[(6, 16), (0, 6), (0, 1), (0, 1), (0, 0), (0, 0), (0, 4)]);
        assert_eq!(view.selected_text(), "paragraph.\n\nSecond\n\na\nb\n\nLast");
        view.clear_selection();
        assert_eq!(view.selected_text(), "");
    }
}

// -- Live preview ----------------------------------------------------------------------------------------------------------------------

#[allow(clippy::single_range_in_vec_init)]
mod live {
    use super::*;
    use crate::markdown::{live_spans, MarkdownEditor, RevealPolicy};
    use crate::widgets::RichSpan;

    /// The text of each span with its flags, for reading a result at a glance.
    fn describe(source: &str, spans: &[RichSpan]) -> Vec<(String, bool, Option<u16>)> {
        let chars: Vec<char> = source.chars().collect();
        spans.iter().map(|s| (chars[s.start..s.start + s.len].iter().collect(), s.hidden, s.font_id)).collect()
    }

    fn style() -> MarkdownStyle {
        MarkdownStyle { bold_font: 2, italic_font: 3, mono_font: 4, heading_font: Some(5), ..MarkdownStyle::default() }
    }

    #[test]
    fn spans_cover_every_char_in_order() {
        for source in ["", "plain", "# Title\n\n**b** *i* `c` [l](u) ![a](i) $x$\n\n> q\n\n- a\n  - b\n\n```rs\nfn x() {}\n```\n\n---\n", "é **ü** ñ [😀](u)"] {
            let spans = live_spans(source, &style(), &[], false);
            let total = source.chars().count();
            let mut at = 0;
            for span in &spans {
                assert_eq!(span.start, at, "{source:?}");
                assert!(span.len > 0);
                at += span.len;
            }
            assert_eq!(at, total, "{source:?}");
        }
    }

    #[test]
    fn markup_is_hidden_unless_revealed() {
        let source = "a **bold** b";
        let hidden = describe(source, &live_spans(source, &style(), &[], false));
        assert_eq!(hidden, vec![("a ".into(), false, None), ("**".into(), true, None), ("bold".into(), false, Some(2)), ("**".into(), true, None), (" b".into(), false, None)]);
        let shown = describe(source, &live_spans(source, &style(), &[0..source.len()], false));
        assert!(shown.iter().all(|(_, hidden, _)| !hidden));
        assert!(shown.iter().any(|(text, _, _)| text == "**"));
        let all = describe(source, &live_spans(source, &style(), &[], true));
        assert!(all.iter().all(|(_, hidden, _)| !hidden));
        // The revealed markup is dimmed.
        let spans = live_spans(source, &style(), &[0..source.len()], false);
        assert_eq!(spans.iter().find(|s| s.start == 2).and_then(|s| s.color), Some(style().muted));
    }

    #[test]
    fn headings_links_code_and_quotes_are_styled() {
        let s = style();
        let source = "## Head\n\n[txt](https://u.rl) `c` > not\n\n> quote\n\n```rs\nfn main() {}\n```\n";
        let spans = live_spans(source, &s, &[], false);
        let d = describe(source, &spans);
        assert_eq!(d[0], ("## ".into(), true, Some(5)));
        assert_eq!(d[1], ("Head".into(), false, Some(5)));
        assert_eq!(d[2].0, "\n\n", "the line break after a heading is not drawn at heading size");
        assert_eq!(d[2].2, None);
        let heading = spans.iter().find(|x| x.start == 3).unwrap();
        assert_eq!(heading.size_scale, s.heading_scale[1]);
        let link = spans.iter().find(|x| source.chars().nth(x.start) == Some('t')).unwrap();
        assert!(link.underline && link.color == Some(s.link) && !link.hidden);
        assert!(d.iter().any(|(t, hidden, _)| t == "](https://u.rl)" && *hidden));
        assert!(d.iter().any(|(t, hidden, font)| t == "c" && !hidden && *font == Some(4)));
        // Quote: the `> ` is markup, the text is italic and muted.
        assert!(d.iter().any(|(t, hidden, _)| t == "> " && *hidden));
        let quote = spans.iter().find(|x| source.chars().nth(x.start) == Some('q')).unwrap();
        assert_eq!((quote.font_id, quote.color), (Some(3), Some(s.muted)));
        // Fenced code: fences are markup, the body is mono and coloured by the highlighter.
        assert!(d.iter().any(|(t, hidden, _)| t.starts_with("```") && *hidden));
        let keyword = spans.iter().find(|x| source.chars().nth(x.start) == Some('f')).unwrap();
        assert_eq!(keyword.font_id, Some(4));
        // Colours are carried as 8 bits per channel.
        assert!((keyword.color.unwrap() - s.code_colours.keyword).abs().max_element() < 0.01);
    }

    #[test]
    fn scripts_math_and_strikethrough() {
        let s = style();
        let source = "x ^2^ y ~~gone~~ $e$";
        let spans = live_spans(source, &s, &[], false);
        let sup = spans.iter().find(|x| source.chars().nth(x.start) == Some('2')).unwrap();
        assert_eq!((sup.size_scale, sup.baseline_shift), (s.script_scale, s.superscript_shift));
        let gone = spans.iter().find(|x| source.chars().nth(x.start) == Some('g')).unwrap();
        assert!(gone.strikethrough);
        let math = spans.iter().find(|x| source.chars().nth(x.start) == Some('e')).unwrap();
        assert_eq!(math.font_id, Some(4));
    }

    #[test]
    fn reveal_ranges_choose_which_blocks_show_markup() {
        let source = "# One\n\n**two**\n";
        let view = MarkdownView::new(source);
        let block = |offset: usize| view.blocks()[view.block_at_offset(offset).unwrap()].source.clone();
        let hidden_in = |reveal: Vec<std::ops::Range<usize>>| -> Vec<String> {
            let spans = live_spans(source, &style(), &reveal, false);
            describe(source, &spans).into_iter().filter(|(_, h, _)| *h).map(|(t, _, _)| t).collect()
        };
        assert_eq!(hidden_in(vec![]), vec!["# ", "**", "**"]);
        assert_eq!(hidden_in(vec![block(1)]), vec!["**", "**"], "only the heading shows its markup");
        assert_eq!(hidden_in(vec![block(9)]), vec!["# "], "only the bold paragraph shows its markup");
    }

    #[test]
    #[allow(clippy::reversed_empty_ranges)] // a reversed range is exactly the hostile input
    fn hostile_source_does_not_panic() {
        for source in ["**", "[", "[](", "![", "# ", "#", "> ", ">", "```", "```\n", "~~~~", "$$", "$", "`", "``", "[[", "[[]]", "^^", "~~", "# {#", "- ", "1.", "\u{0}", "😀 **😀**", "[a](b) [c](d", "\n\n\n"] {
            for reveal in [vec![], vec![0..source.len()], vec![5..2], vec![100..200]] {
                let spans = live_spans(source, &style(), &reveal, false);
                assert_eq!(spans.iter().map(|s| s.len).sum::<usize>(), source.chars().count(), "{source:?}");
            }
        }
    }

    #[test]
    fn spans_follow_typed_text() {
        use crate::markdown::shift_spans_for_insertion;
        let spans = vec![
            RichSpan { start: 0, len: 4, ..Default::default() },
            RichSpan { start: 4, len: 3, hidden: true, ..Default::default() },
            RichSpan { start: 7, len: 2, ..Default::default() },
        ];
        // Typed inside the first span: it grows, the rest move.
        let shifted = shift_spans_for_insertion(&spans, 2, 1);
        assert_eq!(shifted.iter().map(|s| (s.start, s.len)).collect::<Vec<_>>(), vec![(0, 5), (5, 3), (8, 2)]);
        // Typed at the end of a span: that span takes the text.
        let at_end = shift_spans_for_insertion(&spans, 4, 2);
        assert_eq!(at_end.iter().map(|s| (s.start, s.len)).collect::<Vec<_>>(), vec![(0, 6), (6, 3), (9, 2)]);
        // Typed before everything.
        let front = shift_spans_for_insertion(&spans, 0, 1);
        assert_eq!(front.iter().map(|s| s.start).collect::<Vec<_>>(), vec![1, 5, 8]);
        assert!(shift_spans_for_insertion(&[], 3, 2).is_empty());
    }

    #[test]
    fn the_editor_keeps_a_reveal_policy() {
        let mut editor = MarkdownEditor::new("# a");
        editor.set_reveal(RevealPolicy::All);
        editor.set_reveal(RevealPolicy::Custom(Box::new(|view, caret| view.block_at_offset(caret).map(|b| vec![view.blocks()[b].source.clone()]).unwrap_or_default())));
        editor.set_reveal(RevealPolicy::ActiveLine);
        editor.set_source("# b");
        assert_eq!(editor.source(), "# b");
    }
}

// -- The decisions behind culling, auto-scroll and selection across blocks ---------------------------------------------------------

mod decisions {
    use super::super::view::{auto_scroll_step, cell_selection, is_culled};
    use glam::Vec2;

    #[test]
    fn blocks_far_from_the_viewport_are_culled() {
        let view = (Vec2::new(0.0, 100.0), Vec2::new(500.0, 400.0)); // visible 100..500
        let culled = |y: f32, h: f32| is_culled(view.0, view.1, Vec2::new(0.0, y), Vec2::new(500.0, h));
        assert!(!culled(120.0, 50.0), "inside");
        assert!(!culled(-200.0, 50.0), "just above, within a viewport");
        assert!(culled(-400.0, 50.0), "more than a viewport above");
        assert!(!culled(-340.0, 50.0), "ends within a viewport above (-300 and up)");
        assert!(culled(-360.0, 50.0));
        assert!(!culled(900.0, 50.0), "starts within a viewport below the visible part (100 + 2 x 400)");
        assert!(culled(900.1, 50.0));
        assert!(!culled(-500.0, 800.0), "a tall block that reaches into the margin is kept");
    }

    #[test]
    fn auto_scroll_starts_at_the_edges_and_is_capped() {
        // Container spans 100..500.
        assert_eq!(auto_scroll_step(100.0, 400.0, 300.0), 0.0, "middle");
        assert_eq!(auto_scroll_step(100.0, 400.0, 141.0), 0.0, "just inside the edge band");
        assert_eq!(auto_scroll_step(100.0, 400.0, 130.0), -2.5, "10px into the top band");
        assert_eq!(auto_scroll_step(100.0, 400.0, 470.0), 2.5, "10px into the bottom band");
        assert_eq!(auto_scroll_step(100.0, 400.0, 0.0), -20.0, "far above: capped");
        assert_eq!(auto_scroll_step(100.0, 400.0, 900.0), 20.0, "far below: capped");
    }

    #[test]
    fn selection_across_cells_takes_the_end_whole_cells_and_the_start() {
        let first = (1, 4);
        let last = (3, 2);
        let lens = [10, 10, 10, 10, 10];
        let picked: Vec<_> = lens.iter().enumerate().map(|(i, n)| cell_selection(i, *n, first, last)).collect();
        assert_eq!(picked, vec![(0, 0), (4, 10), (0, 10), (0, 2), (0, 0)]);
        // Offsets beyond a cell's length are clamped.
        assert_eq!(cell_selection(1, 3, (1, 99), (2, 99)), (3, 3));
        assert_eq!(cell_selection(2, 3, (1, 99), (2, 99)), (0, 3));
        // Within one cell: from the first to the last offset, never reversed.
        assert_eq!(cell_selection(2, 10, (2, 3), (2, 7)), (3, 7));
        assert_eq!(cell_selection(2, 10, (2, 7), (2, 3)), (7, 7));
    }
}
