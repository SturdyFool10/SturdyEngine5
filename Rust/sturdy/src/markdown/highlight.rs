//! A small built-in syntax highlighter for fenced code blocks: keywords, strings, comments, numbers, types, calls and punctuation, for the
//! languages a README or chat message is likely to quote. It is a lexer, not a parser, so it is cheap and never fails: unknown languages
//! simply come back uncoloured.

use glam::Vec4;

use super::style::{CodeColorizer, CodeColours, CodeSpan};

/// Colours code with the engine's built-in lexer. Use [`MarkdownHighlighter::colorizer`] to plug it into
/// [`MarkdownStyle::code_colorizer`](super::MarkdownStyle::code_colorizer), or set
/// [`MarkdownStyle::highlight_code`](super::MarkdownStyle::highlight_code) to use it with the style's own `code_colours`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct MarkdownHighlighter {
    pub colours: CodeColours,
}

/// The languages [`MarkdownHighlighter`] knows (the first name of each is canonical; the rest are fence aliases).
pub const LANGUAGES: &[&[&str]] = &[
    &["rust", "rs"],
    &["c", "h"],
    &["cpp", "c++", "cc", "cxx", "hpp", "hh", "hxx"],
    &["csharp", "cs", "c#"],
    &["java"],
    &["javascript", "js", "jsx", "mjs", "cjs"],
    &["typescript", "ts", "tsx"],
    &["python", "py"],
    &["go", "golang"],
    &["json", "jsonc"],
    &["toml"],
    &["yaml", "yml"],
    &["shell", "sh", "bash", "zsh", "console"],
    &["sql"],
    &["html", "xml", "svg", "htm"],
    &["css", "scss"],
    &["markdown", "md"],
];

impl MarkdownHighlighter {
    pub fn new(colours: CodeColours) -> Self {
        Self { colours }
    }

    /// Whether `language` (a fence info string such as `rust,no_run`) is one this highlighter knows.
    pub fn supports(language: &str) -> bool {
        canonical(language).is_some()
    }

    /// The coloured ranges of `code`, character offsets, sorted and non-overlapping.
    pub fn spans(&self, language: &str, code: &str) -> Vec<CodeSpan> {
        let Some(language) = canonical(language) else { return Vec::new() };
        let chars: Vec<char> = code.chars().collect();
        let tokens = match language {
            "html" => lex_html(&chars),
            "markdown" => lex_markdown(&chars),
            other => lex(&profile(other), &chars),
        };
        merge(tokens, &self.colours)
    }

    /// This highlighter as the hook [`MarkdownStyle::code_colorizer`](super::MarkdownStyle::code_colorizer) takes.
    pub fn colorizer(&self) -> CodeColorizer {
        let highlighter = *self;
        CodeColorizer::new(move |language, code| highlighter.spans(language, code))
    }
}

fn canonical(info: &str) -> Option<&'static str> {
    let name = info.split(|c: char| c.is_whitespace() || c == ',' || c == '{').next().unwrap_or_default().to_ascii_lowercase();
    LANGUAGES.iter().find(|names| names.contains(&name.as_str())).and_then(|names| names.first().copied())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Token {
    Keyword,
    Str,
    Comment,
    Number,
    Type,
    Function,
    Punct,
}

#[derive(Default)]
struct Profile {
    keywords: &'static str,
    types: &'static str,
    line_comments: &'static [&'static str],
    block_comment: Option<(&'static str, &'static str)>,
    /// String delimiters.
    quotes: &'static [char],
    /// `"""` and `'''` strings.
    triple_quotes: bool,
    /// `'a'` is a char literal, `'a` a lifetime.
    char_literals: bool,
    /// `r"..."`, `r#"..."#`.
    raw_strings: bool,
    ignore_case: bool,
    /// An upper-case-initial identifier with a lower-case letter is a type.
    capitalised_types: bool,
    /// `ident:` is a key.
    key_colon: bool,
    /// `ident =` is a key.
    key_equals: bool,
    /// `[section]` on its own line.
    section_headers: bool,
    /// `#include <..>`-style directives.
    directives: bool,
    /// `$name` and `${name}`.
    dollar_variables: bool,
    /// `@media`, `@Override`, `@decorator`.
    at_words: bool,
    /// `#rrggbb`.
    hash_colours: bool,
    /// `name!(` is a call.
    macros: bool,
}

const RUST: &str = "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while";
const RUST_TYPES: &str = "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box";
const C: &str = "auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union volatile while NULL true false";
const C_TYPES: &str = "void char short int long float double signed unsigned size_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t bool";
const CPP: &str = "alignas alignof and asm auto break case catch class concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export extern false final for friend goto if inline mutable namespace new noexcept not nullptr operator or override private protected public register reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union using virtual volatile while xor";
const CPP_TYPES: &str = "void bool char wchar_t char8_t char16_t char32_t short int long float double signed unsigned size_t string vector map set unique_ptr shared_ptr optional variant";
const CSHARP: &str = "abstract as async await base break case catch checked class const continue default delegate do else enum event explicit extern false finally fixed for foreach goto if implicit in interface internal is lock namespace new null operator out override params private protected public readonly record ref return sealed sizeof stackalloc static struct switch this throw true try typeof unchecked unsafe using var virtual volatile when while yield";
const CSHARP_TYPES: &str = "bool byte char decimal double float int long object sbyte short string uint ulong ushort void dynamic";
const JAVA: &str = "abstract assert break case catch class const continue default do else enum extends final finally for goto if implements import instanceof interface native new null package private protected public return static strictfp super switch synchronized this throw throws transient true false try var volatile while record sealed permits yield";
const JAVA_TYPES: &str = "boolean byte char double float int long short void String Object Integer List Map";
const JS: &str = "async await break case catch class const continue debugger default delete do else export extends false finally for from function if import in instanceof let new null of return static super switch this throw true try typeof undefined var void while with yield";
const TS: &str = "abstract as async await break case catch class const continue declare default delete do else enum export extends false finally for from function if implements import in instanceof interface is keyof let namespace new null of private protected public readonly return satisfies static super switch this throw true try type typeof undefined var void while yield";
const TS_TYPES: &str = "any boolean never number object string symbol unknown bigint";
const PY: &str = "and as assert async await break class continue def del elif else except finally for from global if import in is lambda None nonlocal not or pass raise return self True False try while with yield match case";
const PY_TYPES: &str = "int float str bool bytes list dict set tuple object type complex frozenset";
const GO: &str = "break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var nil true false iota";
const GO_TYPES: &str = "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any";
const SH: &str = "if then else elif fi for while until do done case esac in function select time return exit break continue local export readonly declare unset source alias set shift trap";
const SH_TYPES: &str = "echo cd ls cat grep sed awk find mkdir rm cp mv chmod chown sudo git cargo make curl wget tar test printf read eval exec";
const SQL: &str = "add all alter and any as asc between by case check column constraint create cross database default delete desc distinct drop else end exists foreign from full group having if in index inner insert into is join key left like limit not null offset on or order outer primary references right select set table then top union unique update values view when where with";
const SQL_TYPES: &str = "int integer bigint smallint tinyint decimal numeric float real double char varchar text boolean bool date time timestamp datetime blob json uuid serial";
const CSS: &str = "important inherit initial unset none auto";
const LITERALS: &str = "true false null";
const YAML_LITERALS: &str = "true false null yes no on off";

fn profile(language: &str) -> Profile {
    match language {
        "rust" => Profile {
            keywords: RUST,
            types: RUST_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"'],
            char_literals: true,
            raw_strings: true,
            capitalised_types: true,
            macros: true,
            ..Profile::default()
        },
        "c" => Profile {
            keywords: C,
            types: C_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\''],
            directives: true,
            ..Profile::default()
        },
        "cpp" => Profile {
            keywords: CPP,
            types: CPP_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\''],
            raw_strings: false,
            capitalised_types: true,
            directives: true,
            ..Profile::default()
        },
        "csharp" => Profile {
            keywords: CSHARP,
            types: CSHARP_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\''],
            capitalised_types: true,
            directives: true,
            ..Profile::default()
        },
        "java" => Profile {
            keywords: JAVA,
            types: JAVA_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\''],
            capitalised_types: true,
            at_words: true,
            ..Profile::default()
        },
        "javascript" => Profile {
            keywords: JS,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\'', '`'],
            capitalised_types: true,
            ..Profile::default()
        },
        "typescript" => Profile {
            keywords: TS,
            types: TS_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\'', '`'],
            capitalised_types: true,
            at_words: true,
            ..Profile::default()
        },
        "python" => Profile {
            keywords: PY,
            types: PY_TYPES,
            line_comments: &["#"],
            quotes: &['"', '\''],
            triple_quotes: true,
            capitalised_types: true,
            at_words: true,
            ..Profile::default()
        },
        "go" => Profile {
            keywords: GO,
            types: GO_TYPES,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\'', '`'],
            capitalised_types: true,
            ..Profile::default()
        },
        "json" => Profile {
            keywords: LITERALS,
            line_comments: &["//"],
            block_comment: Some(("/*", "*/")),
            quotes: &['"'],
            key_colon: true,
            ..Profile::default()
        },
        "toml" => Profile {
            keywords: LITERALS,
            line_comments: &["#"],
            quotes: &['"', '\''],
            triple_quotes: true,
            key_equals: true,
            section_headers: true,
            ..Profile::default()
        },
        "yaml" => Profile {
            keywords: YAML_LITERALS,
            line_comments: &["#"],
            quotes: &['"', '\''],
            key_colon: true,
            ..Profile::default()
        },
        "shell" => Profile {
            keywords: SH,
            types: SH_TYPES,
            line_comments: &["#"],
            quotes: &['"', '\'', '`'],
            dollar_variables: true,
            ..Profile::default()
        },
        "sql" => Profile {
            keywords: SQL,
            types: SQL_TYPES,
            line_comments: &["--"],
            block_comment: Some(("/*", "*/")),
            quotes: &['\'', '"'],
            ignore_case: true,
            ..Profile::default()
        },
        // "css"
        _ => Profile {
            keywords: CSS,
            block_comment: Some(("/*", "*/")),
            quotes: &['"', '\''],
            key_colon: true,
            at_words: true,
            hash_colours: true,
            ..Profile::default()
        },
    }
}

fn has_word(list: &str, word: &str, ignore_case: bool) -> bool {
    list.split_whitespace().any(|candidate| if ignore_case { candidate.eq_ignore_ascii_case(word) } else { candidate == word })
}

fn at(chars: &[char], i: usize) -> char {
    chars.get(i).copied().unwrap_or('\0')
}

fn matches_at(chars: &[char], i: usize, pattern: &str) -> bool {
    pattern.chars().enumerate().all(|(offset, c)| chars.get(i + offset) == Some(&c))
}

fn is_ident_start(c: char) -> bool {
    c.is_alphabetic() || c == '_' || c == '$'
}

fn is_ident(c: char) -> bool {
    c.is_alphanumeric() || c == '_' || c == '$'
}

/// The first index after `i` that is not a space or tab.
fn skip_blanks(chars: &[char], mut i: usize) -> usize {
    while matches!(chars.get(i), Some(' ' | '\t')) {
        i += 1;
    }
    i
}

fn line_end(chars: &[char], mut i: usize) -> usize {
    while i < chars.len() && at(chars, i) != '\n' {
        i += 1;
    }
    i
}

/// Index just past a string that opened at `start` with `quote`: backslash escapes, ends at the closing quote (or the line's end
/// for single-line strings; backtick strings may span lines).
fn string_end(chars: &[char], start: usize, quote: char) -> usize {
    let mut i = start + 1;
    while i < chars.len() {
        let c = at(chars, i);
        if c == '\\' {
            i += 2;
        } else if c == quote {
            return i + 1;
        } else if c == '\n' && quote != '`' {
            return i;
        } else {
            i += 1;
        }
    }
    chars.len()
}

fn number_end(chars: &[char], start: usize) -> usize {
    let mut i = start;
    let hex = at(chars, i) == '0' && matches!(at(chars, i + 1), 'x' | 'X');
    while i < chars.len() {
        let c = at(chars, i);
        let exponent_sign = matches!(c, '+' | '-') && !hex && matches!(at(chars, i.saturating_sub(1)), 'e' | 'E') && i > start;
        if c.is_alphanumeric() || c == '_' || exponent_sign || (c == '.' && at(chars, i + 1).is_ascii_digit()) {
            i += 1;
        } else {
            break;
        }
    }
    i
}

fn lex(profile: &Profile, chars: &[char]) -> Vec<(usize, usize, Token)> {
    let mut out = Vec::new();
    let mut i = 0usize;
    let mut line_start = true;
    while i < chars.len() {
        let c = at(chars, i);
        if c == '\n' {
            line_start = true;
            i += 1;
            continue;
        }
        if c.is_whitespace() {
            i += 1;
            continue;
        }
        let at_line_start = std::mem::replace(&mut line_start, false);
        let start = i;

        if profile.line_comments.iter().any(|marker| matches_at(chars, i, marker)) {
            i = line_end(chars, i);
            out.push((start, i, Token::Comment));
            continue;
        }
        if let Some((open, close)) = profile.block_comment
            && matches_at(chars, i, open)
        {
            i += open.chars().count();
            while i < chars.len() && !matches_at(chars, i, close) {
                i += 1;
            }
            i = (i + close.chars().count()).min(chars.len());
            out.push((start, i, Token::Comment));
            continue;
        }
        if profile.section_headers && at_line_start && c == '[' {
            i = line_end(chars, i);
            out.push((start, i, Token::Keyword));
            continue;
        }
        if profile.directives && at_line_start && c == '#' {
            i += 1;
            while is_ident(at(chars, i)) {
                i += 1;
            }
            out.push((start, i, Token::Keyword));
            let after = skip_blanks(chars, i);
            if at(chars, after) == '<' {
                let mut end = after;
                while end < chars.len() && at(chars, end) != '>' && at(chars, end) != '\n' {
                    end += 1;
                }
                i = (end + 1).min(chars.len());
                out.push((after, i, Token::Str));
            }
            continue;
        }
        if profile.triple_quotes && matches!(c, '"' | '\'') && at(chars, i + 1) == c && at(chars, i + 2) == c {
            i += 3;
            while i < chars.len() && !(at(chars, i) == c && at(chars, i + 1) == c && at(chars, i + 2) == c) {
                i += if at(chars, i) == '\\' { 2 } else { 1 };
            }
            i = (i + 3).min(chars.len());
            out.push((start, i, Token::Str));
            continue;
        }
        if profile.raw_strings && c == 'r' && matches!(at(chars, i + 1), '"' | '#') {
            let mut hashes = 0usize;
            let mut j = i + 1;
            while at(chars, j) == '#' {
                hashes += 1;
                j += 1;
            }
            if at(chars, j) == '"' {
                j += 1;
                while j < chars.len() {
                    if at(chars, j) == '"' && (0..hashes).all(|h| at(chars, j + 1 + h) == '#') {
                        j += 1 + hashes;
                        break;
                    }
                    j += 1;
                }
                i = j.min(chars.len());
                out.push((start, i, Token::Str));
                continue;
            }
        }
        if profile.char_literals && c == '\'' {
            // 'x' or '\n' is a char literal; 'a without a closing quote is a lifetime.
            let literal_end = if at(chars, i + 1) == '\\' { i + 3 } else { i + 2 };
            if at(chars, literal_end) == '\'' {
                i = literal_end + 1;
                out.push((start, i, Token::Str));
            } else {
                i += 1;
                while is_ident(at(chars, i)) {
                    i += 1;
                }
                out.push((start, i, Token::Type));
            }
            continue;
        }
        if profile.quotes.contains(&c) {
            i = string_end(chars, i, c);
            let after = skip_blanks(chars, i);
            let is_key = profile.key_colon && at(chars, after) == ':' && at(chars, after + 1) != ':';
            out.push((start, i, if is_key { Token::Type } else { Token::Str }));
            continue;
        }
        if c.is_ascii_digit() || (c == '.' && at(chars, i + 1).is_ascii_digit()) {
            i = number_end(chars, i);
            out.push((start, i, Token::Number));
            continue;
        }
        if profile.hash_colours && c == '#' && at(chars, i + 1).is_ascii_hexdigit() {
            i += 1;
            while at(chars, i).is_ascii_hexdigit() {
                i += 1;
            }
            out.push((start, i, Token::Number));
            continue;
        }
        if profile.dollar_variables && c == '$' {
            i += 1;
            if at(chars, i) == '{' {
                while i < chars.len() && at(chars, i) != '}' && at(chars, i) != '\n' {
                    i += 1;
                }
                i = (i + 1).min(chars.len());
            } else {
                while is_ident(at(chars, i)) || (i == start + 1 && matches!(at(chars, i), '?' | '#' | '@' | '*' | '!')) {
                    i += 1;
                }
            }
            out.push((start, i, Token::Type));
            continue;
        }
        if profile.at_words && c == '@' && is_ident_start(at(chars, i + 1)) {
            i += 1;
            while is_ident(at(chars, i)) || at(chars, i) == '-' {
                i += 1;
            }
            out.push((start, i, Token::Keyword));
            continue;
        }
        if is_ident_start(c) {
            while is_ident(at(chars, i)) {
                i += 1;
            }
            let word: String = chars.get(start..i).unwrap_or_default().iter().collect();
            let next = skip_blanks(chars, i);
            let token = if has_word(profile.keywords, &word, profile.ignore_case) {
                Some(Token::Keyword)
            } else if has_word(profile.types, &word, profile.ignore_case)
                || (profile.capitalised_types
                    && word.chars().next().is_some_and(char::is_uppercase)
                    && word.chars().any(char::is_lowercase))
                || (profile.key_colon && at(chars, next) == ':' && at(chars, next + 1) != ':')
                || (profile.key_equals && at(chars, next) == '=' && at(chars, next + 1) != '=')
            {
                Some(Token::Type)
            } else if at(chars, next) == '(' || (profile.macros && at(chars, i) == '!' && matches!(at(chars, i + 1), '(' | '[' | '{')) {
                Some(Token::Function)
            } else {
                None
            };
            if let Some(token) = token {
                out.push((start, i, token));
            }
            continue;
        }
        if "{}()[];,.:<>=+-*/%&|!^~?@".contains(c) {
            i += 1;
            out.push((start, i, Token::Punct));
            continue;
        }
        i += 1;
    }
    out
}

/// HTML/XML: comments, tag names, attribute names and values, entities.
fn lex_html(chars: &[char]) -> Vec<(usize, usize, Token)> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < chars.len() {
        let c = at(chars, i);
        if matches_at(chars, i, "<!--") {
            let start = i;
            i += 4;
            while i < chars.len() && !matches_at(chars, i, "-->") {
                i += 1;
            }
            i = (i + 3).min(chars.len());
            out.push((start, i, Token::Comment));
        } else if c == '<' && (is_ident_start(at(chars, i + 1)) || matches!(at(chars, i + 1), '/' | '!' | '?')) {
            out.push((i, i + 1, Token::Punct));
            i += 1;
            if matches!(at(chars, i), '/' | '!' | '?') {
                out.push((i, i + 1, Token::Punct));
                i += 1;
            }
            let name_start = i;
            while is_ident(at(chars, i)) || matches!(at(chars, i), '-' | ':' | '.') {
                i += 1;
            }
            out.push((name_start, i, Token::Keyword));
            // Attributes up to the closing '>'.
            while i < chars.len() && at(chars, i) != '>' {
                let a = at(chars, i);
                if matches!(a, '"' | '\'') {
                    let start = i;
                    i = string_end(chars, i, a).max(i + 1);
                    out.push((start, i, Token::Str));
                } else if is_ident_start(a) {
                    let start = i;
                    while is_ident(at(chars, i)) || matches!(at(chars, i), '-' | ':' | '.') {
                        i += 1;
                    }
                    out.push((start, i, Token::Type));
                } else {
                    if matches!(a, '=' | '/') {
                        out.push((i, i + 1, Token::Punct));
                    }
                    i += 1;
                }
            }
            if i < chars.len() {
                out.push((i, i + 1, Token::Punct));
                i += 1;
            }
        } else if c == '&' && is_ident_start(at(chars, i + 1)) {
            let start = i;
            i += 1;
            while is_ident(at(chars, i)) {
                i += 1;
            }
            if at(chars, i) == ';' {
                i += 1;
            }
            out.push((start, i, Token::Number));
        } else {
            i += 1;
        }
    }
    out
}

/// Markdown: headings, quotes, list markers, fences, inline code, link targets.
fn lex_markdown(chars: &[char]) -> Vec<(usize, usize, Token)> {
    let mut out = Vec::new();
    let mut i = 0usize;
    let mut in_fence = false;
    while i < chars.len() {
        let end = line_end(chars, i);
        let first = skip_blanks(chars, i);
        let marker = at(chars, first);
        if matches_at(chars, first, "```") || matches_at(chars, first, "~~~") {
            in_fence = !in_fence;
            out.push((i, end, Token::Comment));
        } else if in_fence {
            out.push((i, end, Token::Str));
        } else if marker == '#' {
            out.push((first, end, Token::Keyword));
        } else if marker == '>' {
            out.push((first, end, Token::Comment));
        } else {
            if matches!(marker, '-' | '*' | '+') && at(chars, first + 1) == ' ' {
                out.push((first, first + 1, Token::Punct));
            }
            let mut j = first;
            while j < end {
                let c = at(chars, j);
                if c == '`' {
                    let close = (j + 1..end).find(|&k| at(chars, k) == '`').map_or(end, |k| k + 1);
                    out.push((j, close, Token::Str));
                    j = close;
                } else if c == ']' && at(chars, j + 1) == '(' {
                    let close = (j + 2..end).find(|&k| at(chars, k) == ')').map_or(end, |k| k + 1);
                    out.push((j + 1, close, Token::Str));
                    j = close;
                } else if matches!(c, '*' | '_') && at(chars, j + 1) == c {
                    out.push((j, j + 2, Token::Punct));
                    j += 2;
                } else {
                    j += 1;
                }
            }
        }
        i = end + 1;
    }
    out
}

/// Colours the tokens and merges neighbours of one colour into one span; sorted, non-overlapping.
fn merge(mut tokens: Vec<(usize, usize, Token)>, colours: &CodeColours) -> Vec<CodeSpan> {
    tokens.sort_by_key(|(start, _, _)| *start);
    let colour = |token: Token| -> Vec4 {
        match token {
            Token::Keyword => colours.keyword,
            Token::Str => colours.string,
            Token::Comment => colours.comment,
            Token::Number => colours.number,
            Token::Type => colours.ty,
            Token::Function => colours.function,
            Token::Punct => colours.punctuation,
        }
    };
    let mut spans: Vec<CodeSpan> = Vec::new();
    let mut covered = 0usize;
    for (start, end, token) in tokens {
        let start = start.max(covered);
        if end <= start {
            continue;
        }
        covered = end;
        let color = colour(token);
        match spans.last_mut() {
            Some(last) if last.start + last.len == start && last.color == color => last.len += end - start,
            _ => spans.push(CodeSpan { start, len: end - start, color }),
        }
    }
    spans
}
