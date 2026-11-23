//! A LaTeX math subset: parsed to a tree ([`TexNode`]), written as Unicode text ([`to_unicode`], for running text) or drawn as laid-out
//! UI elements ([`draw`], for display math). Independent of Markdown: use it for any formula the UI needs.
//!
//! Supported: letters, numbers, operators and relations, Greek, arrows and the common symbols (`\alpha`, `\leq`, `\infty`, `\partial`...),
//! `^` `_` and primes, `\frac` `\dfrac` `\tfrac` `\binom`, `\sqrt[n]{x}`, big operators with limits (`\sum_{i=1}^n`, `\int`, `\lim`),
//! named functions (`\sin`, `\log`...), accents (`\hat` `\bar` `\vec` `\dot` `\tilde` `\overline` `\underline`), `\left..\right`, `\text{}`,
//! font commands (`\mathbb` `\mathcal` `\mathbf` `\mathrm` `\mathfrak` `\mathsf` `\mathtt` `\mathit`), spacing (`\,` `\;` `\quad`),
//! and the `matrix` family, `cases`, `aligned`/`align`/`array` environments. Unknown commands are shown as written rather than failing.

use glam::Vec4;

use crate::ui::{AlignX, AlignY, Border, ChildAlignment, Direction, ElementDesc, Padding, Sizing, TextStyle, Ui};

/// Why a formula did not parse; the caller shows the TeX source instead.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TexError {
    pub message: String,
    /// Char offset in the source.
    pub position: usize,
}

impl std::fmt::Display for TexError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} at {}", self.message, self.position)
    }
}

impl std::error::Error for TexError {}

/// How a symbol spaces itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SymbolClass {
    Ordinary,
    /// Binary operator: `+ - \times`.
    Binary,
    /// Relation: `= \leq \to`.
    Relation,
    Open,
    Close,
    Punctuation,
    /// Big operator (`\sum`): limits go above and below in display math.
    BigOperator,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TexFont {
    Roman,
    Bold,
    Italic,
    Blackboard,
    Calligraphic,
    Fraktur,
    SansSerif,
    Monospace,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AccentKind {
    Hat,
    Bar,
    Vec,
    Dot,
    DoubleDot,
    Tilde,
    Overline,
    Underline,
}

#[derive(Debug, Clone, PartialEq)]
pub enum TexNode {
    Row(Vec<TexNode>),
    /// A variable: drawn italic.
    Ident(String),
    Number(String),
    Symbol { ch: char, class: SymbolClass },
    /// Upright text: `\text{}`, function names (`sin`), unknown commands.
    Text(String),
    /// A function name; `limits` puts a subscript below it (`\lim`).
    Operator { name: String, limits: bool },
    Frac { numerator: Box<TexNode>, denominator: Box<TexNode>, rule: bool },
    Sqrt { index: Option<Box<TexNode>>, body: Box<TexNode> },
    Script { base: Box<TexNode>, sub: Option<Box<TexNode>>, sup: Option<Box<TexNode>> },
    Accent { kind: AccentKind, body: Box<TexNode> },
    Delimited { open: Option<char>, close: Option<char>, body: Box<TexNode> },
    Matrix { rows: Vec<Vec<TexNode>>, open: Option<char>, close: Option<char>, align: ColumnAlignment },
    Space(f32),
    Font { font: TexFont, body: Box<TexNode> },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColumnAlignment {
    Center,
    /// `cases`, `aligned`: columns flush left.
    Left,
}

const MAX_DEPTH: usize = 48;

fn symbol(name: &str) -> Option<(char, SymbolClass)> {
    use SymbolClass::{Binary, BigOperator, Close, Ordinary, Open, Punctuation, Relation};
    Some(match name {
        "alpha" => ('α', Ordinary), "beta" => ('β', Ordinary), "gamma" => ('γ', Ordinary), "delta" => ('δ', Ordinary),
        "epsilon" => ('ϵ', Ordinary), "varepsilon" => ('ε', Ordinary), "zeta" => ('ζ', Ordinary), "eta" => ('η', Ordinary),
        "theta" => ('θ', Ordinary), "vartheta" => ('ϑ', Ordinary), "iota" => ('ι', Ordinary), "kappa" => ('κ', Ordinary),
        "lambda" => ('λ', Ordinary), "mu" => ('μ', Ordinary), "nu" => ('ν', Ordinary), "xi" => ('ξ', Ordinary),
        "pi" => ('π', Ordinary), "varpi" => ('ϖ', Ordinary), "rho" => ('ρ', Ordinary), "varrho" => ('ϱ', Ordinary),
        "sigma" => ('σ', Ordinary), "varsigma" => ('ς', Ordinary), "tau" => ('τ', Ordinary), "upsilon" => ('υ', Ordinary),
        "phi" => ('ϕ', Ordinary), "varphi" => ('φ', Ordinary), "chi" => ('χ', Ordinary), "psi" => ('ψ', Ordinary),
        "omega" => ('ω', Ordinary),
        "Gamma" => ('Γ', Ordinary), "Delta" => ('Δ', Ordinary), "Theta" => ('Θ', Ordinary), "Lambda" => ('Λ', Ordinary),
        "Xi" => ('Ξ', Ordinary), "Pi" => ('Π', Ordinary), "Sigma" => ('Σ', Ordinary), "Upsilon" => ('Υ', Ordinary),
        "Phi" => ('Φ', Ordinary), "Psi" => ('Ψ', Ordinary), "Omega" => ('Ω', Ordinary),
        "pm" => ('±', Binary), "mp" => ('∓', Binary), "times" => ('×', Binary), "div" => ('÷', Binary), "cdot" => ('⋅', Binary),
        "ast" => ('∗', Binary), "star" => ('⋆', Binary), "circ" => ('∘', Binary), "bullet" => ('∙', Binary),
        "oplus" => ('⊕', Binary), "otimes" => ('⊗', Binary), "cup" => ('∪', Binary), "cap" => ('∩', Binary),
        "setminus" => ('∖', Binary), "wedge" => ('∧', Binary), "land" => ('∧', Binary), "vee" => ('∨', Binary), "lor" => ('∨', Binary),
        "neg" => ('¬', Ordinary), "lnot" => ('¬', Ordinary),
        "leq" => ('≤', Relation), "le" => ('≤', Relation), "geq" => ('≥', Relation), "ge" => ('≥', Relation),
        "neq" => ('≠', Relation), "ne" => ('≠', Relation), "approx" => ('≈', Relation), "equiv" => ('≡', Relation),
        "sim" => ('∼', Relation), "simeq" => ('≃', Relation), "cong" => ('≅', Relation), "propto" => ('∝', Relation),
        "ll" => ('≪', Relation), "gg" => ('≫', Relation), "subset" => ('⊂', Relation), "supset" => ('⊃', Relation),
        "subseteq" => ('⊆', Relation), "supseteq" => ('⊇', Relation), "in" => ('∈', Relation), "notin" => ('∉', Relation),
        "ni" => ('∋', Relation), "perp" => ('⊥', Relation), "parallel" => ('∥', Relation), "mid" => ('∣', Relation),
        "to" => ('→', Relation), "rightarrow" => ('→', Relation), "leftarrow" => ('←', Relation), "gets" => ('←', Relation),
        "leftrightarrow" => ('↔', Relation), "Rightarrow" => ('⇒', Relation), "Leftarrow" => ('⇐', Relation),
        "Leftrightarrow" => ('⇔', Relation), "iff" => ('⟺', Relation), "implies" => ('⟹', Relation), "mapsto" => ('↦', Relation),
        "uparrow" => ('↑', Relation), "downarrow" => ('↓', Relation),
        "infty" => ('∞', Ordinary), "partial" => ('∂', Ordinary), "nabla" => ('∇', Ordinary), "forall" => ('∀', Ordinary),
        "exists" => ('∃', Ordinary), "nexists" => ('∄', Ordinary), "emptyset" => ('∅', Ordinary), "varnothing" => ('∅', Ordinary),
        "hbar" => ('ℏ', Ordinary), "ell" => ('ℓ', Ordinary), "Re" => ('ℜ', Ordinary), "Im" => ('ℑ', Ordinary),
        "aleph" => ('ℵ', Ordinary), "angle" => ('∠', Ordinary), "triangle" => ('△', Ordinary), "prime" => ('′', Ordinary),
        "ldots" => ('…', Ordinary), "dots" => ('…', Ordinary), "cdots" => ('⋯', Ordinary), "vdots" => ('⋮', Ordinary),
        "ddots" => ('⋱', Ordinary), "therefore" => ('∴', Relation), "because" => ('∵', Relation), "square" => ('□', Ordinary),
        "sum" => ('∑', BigOperator), "prod" => ('∏', BigOperator), "coprod" => ('∐', BigOperator), "int" => ('∫', BigOperator),
        "iint" => ('∬', BigOperator), "iiint" => ('∭', BigOperator), "oint" => ('∮', BigOperator),
        "bigcup" => ('⋃', BigOperator), "bigcap" => ('⋂', BigOperator), "bigoplus" => ('⨁', BigOperator),
        "bigotimes" => ('⨂', BigOperator), "bigvee" => ('⋁', BigOperator), "bigwedge" => ('⋀', BigOperator),
        "langle" => ('⟨', Open), "rangle" => ('⟩', Close), "lceil" => ('⌈', Open), "rceil" => ('⌉', Close),
        "lfloor" => ('⌊', Open), "rfloor" => ('⌋', Close), "lbrace" => ('{', Open), "rbrace" => ('}', Close),
        "vert" => ('|', Ordinary), "Vert" => ('‖', Ordinary), "lvert" => ('|', Open), "rvert" => ('|', Close),
        "colon" => (':', Punctuation),
        _ => return None,
    })
}

const FUNCTIONS: &[&str] = &[
    "sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh", "tanh", "coth", "log", "ln", "lg", "exp",
    "lim", "limsup", "liminf", "max", "min", "sup", "inf", "det", "dim", "ker", "gcd", "deg", "arg", "hom", "Pr", "mod",
];
const LIMIT_FUNCTIONS: &[&str] = &["lim", "limsup", "liminf", "max", "min", "sup", "inf", "det", "gcd", "Pr"];

/// `c` written in the math alphabet `font`, when that alphabet has a Unicode form.
pub fn style_char(font: TexFont, c: char) -> char {
    let offset = |base: u32, first: char| -> Option<char> { char::from_u32(base + (c as u32 - first as u32)) };
    match font {
        TexFont::Blackboard => match c {
            'C' => 'ℂ', 'H' => 'ℍ', 'N' => 'ℕ', 'P' => 'ℙ', 'Q' => 'ℚ', 'R' => 'ℝ', 'Z' => 'ℤ',
            'A'..='Z' => offset(0x1D538, 'A').unwrap_or(c),
            'a'..='z' => offset(0x1D552, 'a').unwrap_or(c),
            '0'..='9' => offset(0x1D7D8, '0').unwrap_or(c),
            _ => c,
        },
        TexFont::Calligraphic => match c {
            'B' => 'ℬ', 'E' => 'ℰ', 'F' => 'ℱ', 'H' => 'ℋ', 'I' => 'ℐ', 'L' => 'ℒ', 'M' => 'ℳ', 'R' => 'ℛ',
            'A'..='Z' => offset(0x1D49C, 'A').unwrap_or(c),
            _ => c,
        },
        TexFont::Fraktur => match c {
            'C' => 'ℭ', 'H' => 'ℌ', 'I' => 'ℑ', 'R' => 'ℜ', 'Z' => 'ℨ',
            'A'..='Z' => offset(0x1D504, 'A').unwrap_or(c),
            'a'..='z' => offset(0x1D51E, 'a').unwrap_or(c),
            _ => c,
        },
        _ => c,
    }
}

struct Parser<'a> {
    chars: Vec<char>,
    at: usize,
    depth: usize,
    _source: &'a str,
}

/// What ends the row being parsed.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Until {
    End,
    Brace,
    Right,
    /// `&`, `\\` or `\end` (a matrix cell).
    Cell,
    Bracket,
}

impl Parser<'_> {
    fn error(&self, message: &str) -> TexError {
        TexError { message: message.to_owned(), position: self.at }
    }

    fn peek(&self) -> Option<char> {
        self.chars.get(self.at).copied()
    }

    fn skip_spaces(&mut self) {
        while self.peek().is_some_and(char::is_whitespace) {
            self.at += 1;
        }
    }

    fn command_name(&mut self) -> String {
        // After the backslash: a run of letters, or one other char.
        let Some(first) = self.peek() else { return String::new() };
        if first.is_ascii_alphabetic() {
            let mut name = String::new();
            while let Some(c) = self.peek().filter(char::is_ascii_alphabetic) {
                name.push(c);
                self.at += 1;
            }
            name
        } else {
            self.at += 1;
            first.to_string()
        }
    }

    fn enter(&mut self) -> Result<(), TexError> {
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return Err(self.error("nested too deeply"));
        }
        Ok(())
    }

    fn row(&mut self, until: Until) -> Result<TexNode, TexError> {
        self.enter()?;
        let mut items = Vec::new();
        loop {
            self.skip_spaces();
            match self.peek() {
                None => {
                    if matches!(until, Until::Brace | Until::Right | Until::Bracket) {
                        return Err(self.error("unclosed group"));
                    }
                    break;
                }
                Some('}') if until == Until::Brace => break,
                Some(']') if until == Until::Bracket => break,
                Some('&') if until == Until::Cell => break,
                Some('\\') if until == Until::Cell && matches!(self.chars.get(self.at + 1), Some('\\')) => break,
                Some('\\') if matches!(until, Until::Cell | Until::Right) && self.at_command("end") => break,
                Some('\\') if until == Until::Right && self.at_command("right") => break,
                Some('}') => return Err(self.error("unmatched }")),
                _ => {}
            }
            let atom = self.atom()?;
            let atom = self.scripts(atom)?;
            items.push(atom);
        }
        self.depth -= 1;
        Ok(if items.len() == 1 { items.remove(0) } else { TexNode::Row(items) })
    }

    fn at_command(&self, name: &str) -> bool {
        let mut i = self.at + 1;
        for c in name.chars() {
            if self.chars.get(i) != Some(&c) {
                return false;
            }
            i += 1;
        }
        !self.chars.get(i).is_some_and(char::is_ascii_alphabetic)
    }

    /// `{...}` or a single token, as the argument of a command or script.
    fn argument(&mut self) -> Result<TexNode, TexError> {
        self.skip_spaces();
        match self.peek() {
            Some('{') => {
                self.at += 1;
                let body = self.row(Until::Brace)?;
                self.at += 1;
                Ok(body)
            }
            Some(_) => self.atom(),
            None => Err(self.error("missing argument")),
        }
    }

    fn scripts(&mut self, base: TexNode) -> Result<TexNode, TexError> {
        let (mut sub, mut sup) = (None, None);
        loop {
            self.skip_spaces();
            match self.peek() {
                Some('^') if sup.is_none() => {
                    self.at += 1;
                    sup = Some(Box::new(self.argument()?));
                }
                Some('_') if sub.is_none() => {
                    self.at += 1;
                    sub = Some(Box::new(self.argument()?));
                }
                Some('\'') => {
                    self.at += 1;
                    let prime = TexNode::Symbol { ch: '′', class: SymbolClass::Ordinary };
                    sup = Some(Box::new(match sup.take() {
                        Some(existing) => TexNode::Row(vec![*existing, prime]),
                        None => prime,
                    }));
                }
                _ => break,
            }
        }
        Ok(if sub.is_some() || sup.is_some() { TexNode::Script { base: Box::new(base), sub, sup } } else { base })
    }

    fn atom(&mut self) -> Result<TexNode, TexError> {
        let Some(c) = self.peek() else { return Err(self.error("unexpected end")) };
        self.at += 1;
        use SymbolClass::{Binary, Close, Open, Ordinary, Punctuation, Relation};
        Ok(match c {
            '{' => {
                let body = self.row(Until::Brace)?;
                self.at += 1;
                body
            }
            '\\' => return self.command(),
            '0'..='9' | '.' if c != '.' || self.peek().is_some_and(|n| n.is_ascii_digit()) => {
                let mut number = c.to_string();
                while let Some(n) = self.peek().filter(|n| n.is_ascii_digit() || (*n == '.' && self.chars.get(self.at + 1).is_some_and(char::is_ascii_digit))) {
                    number.push(n);
                    self.at += 1;
                }
                TexNode::Number(number)
            }
            '+' => TexNode::Symbol { ch: '+', class: Binary },
            '-' => TexNode::Symbol { ch: '−', class: Binary },
            '*' => TexNode::Symbol { ch: '∗', class: Binary },
            '=' => TexNode::Symbol { ch: '=', class: Relation },
            '<' => TexNode::Symbol { ch: '<', class: Relation },
            '>' => TexNode::Symbol { ch: '>', class: Relation },
            '(' | '[' => TexNode::Symbol { ch: c, class: Open },
            ')' | ']' => TexNode::Symbol { ch: c, class: Close },
            ',' | ';' => TexNode::Symbol { ch: c, class: Punctuation },
            '|' => TexNode::Symbol { ch: '|', class: Ordinary },
            '!' | '?' | ':' | '/' | '.' => TexNode::Symbol { ch: c, class: Ordinary },
            '~' => TexNode::Space(0.3),
            '&' | '^' | '_' | '#' | '$' | '%' => return Err(TexError { message: format!("unexpected {c}"), position: self.at - 1 }),
            c if c.is_alphabetic() => TexNode::Ident(c.to_string()),
            c => TexNode::Symbol { ch: c, class: Ordinary },
        })
    }

    fn delimiter(&mut self) -> Result<Option<char>, TexError> {
        self.skip_spaces();
        let Some(c) = self.peek() else { return Err(self.error("missing delimiter")) };
        self.at += 1;
        if c == '.' {
            return Ok(None);
        }
        if c == '\\' {
            let name = self.command_name();
            return Ok(match name.as_str() {
                "{" | "lbrace" => Some('{'),
                "}" | "rbrace" => Some('}'),
                "|" | "Vert" => Some('‖'),
                other => symbol(other).map(|(ch, _)| ch),
            });
        }
        Ok(Some(c))
    }

    fn command(&mut self) -> Result<TexNode, TexError> {
        let start = self.at - 1;
        let name = self.command_name();
        let styled = |this: &mut Self, font: TexFont| -> Result<TexNode, TexError> {
            Ok(TexNode::Font { font, body: Box::new(this.argument()?) })
        };
        let accent = |this: &mut Self, kind: AccentKind| -> Result<TexNode, TexError> {
            Ok(TexNode::Accent { kind, body: Box::new(this.argument()?) })
        };
        Ok(match name.as_str() {
            "frac" | "dfrac" | "tfrac" | "cfrac" => {
                let numerator = Box::new(self.argument()?);
                let denominator = Box::new(self.argument()?);
                TexNode::Frac { numerator, denominator, rule: true }
            }
            "binom" | "dbinom" | "tbinom" => {
                let numerator = Box::new(self.argument()?);
                let denominator = Box::new(self.argument()?);
                TexNode::Delimited {
                    open: Some('('),
                    close: Some(')'),
                    body: Box::new(TexNode::Frac { numerator, denominator, rule: false }),
                }
            }
            "sqrt" => {
                self.skip_spaces();
                let index = if self.peek() == Some('[') {
                    self.at += 1;
                    let index = self.row(Until::Bracket)?;
                    self.at += 1;
                    Some(Box::new(index))
                } else {
                    None
                };
                TexNode::Sqrt { index, body: Box::new(self.argument()?) }
            }
            "text" | "textrm" | "mathrm" | "textnormal" | "operatorname" | "mbox" => {
                self.skip_spaces();
                if self.peek() == Some('{') {
                    self.at += 1;
                    let mut text = String::new();
                    let mut depth = 1;
                    while let Some(c) = self.peek() {
                        self.at += 1;
                        match c {
                            '{' => depth += 1,
                            '}' => {
                                depth -= 1;
                                if depth == 0 {
                                    break;
                                }
                            }
                            _ => {}
                        }
                        text.push(c);
                    }
                    if depth != 0 {
                        return Err(self.error("unclosed group"));
                    }
                    TexNode::Text(text)
                } else {
                    TexNode::Font { font: TexFont::Roman, body: Box::new(self.argument()?) }
                }
            }
            "mathbf" | "boldsymbol" | "bm" | "textbf" => styled(self, TexFont::Bold)?,
            "mathit" | "textit" => styled(self, TexFont::Italic)?,
            "mathbb" => styled(self, TexFont::Blackboard)?,
            "mathcal" | "mathscr" => styled(self, TexFont::Calligraphic)?,
            "mathfrak" => styled(self, TexFont::Fraktur)?,
            "mathsf" => styled(self, TexFont::SansSerif)?,
            "mathtt" | "texttt" => styled(self, TexFont::Monospace)?,
            "hat" | "widehat" => accent(self, AccentKind::Hat)?,
            "bar" => accent(self, AccentKind::Bar)?,
            "vec" => accent(self, AccentKind::Vec)?,
            "dot" => accent(self, AccentKind::Dot)?,
            "ddot" => accent(self, AccentKind::DoubleDot)?,
            "tilde" | "widetilde" => accent(self, AccentKind::Tilde)?,
            "overline" => accent(self, AccentKind::Overline)?,
            "underline" => accent(self, AccentKind::Underline)?,
            "left" => {
                let open = self.delimiter()?;
                let body = self.row(Until::Right)?;
                if !self.at_command("right") {
                    return Err(self.error("\\left without \\right"));
                }
                self.at += 1;
                self.command_name();
                let close = self.delimiter()?;
                TexNode::Delimited { open, close, body: Box::new(body) }
            }
            "right" => return Err(TexError { message: "\\right without \\left".into(), position: start }),
            "begin" => return self.environment(),
            "end" => return Err(TexError { message: "\\end without \\begin".into(), position: start }),
            "," | "thinspace" => TexNode::Space(0.17),
            ":" | "medspace" => TexNode::Space(0.22),
            ";" | "thickspace" => TexNode::Space(0.28),
            "!" | "negthinspace" => TexNode::Space(-0.17),
            " " => TexNode::Space(0.3),
            "quad" => TexNode::Space(1.0),
            "qquad" => TexNode::Space(2.0),
            "{" | "}" | "#" | "$" | "%" | "&" | "_" => TexNode::Symbol { ch: name.chars().next().unwrap_or('?'), class: SymbolClass::Ordinary },
            "|" => TexNode::Symbol { ch: '‖', class: SymbolClass::Ordinary },
            "\\" => return Err(TexError { message: "\\\\ outside a matrix".into(), position: start }),
            "limits" | "nolimits" | "displaystyle" | "textstyle" | "scriptstyle" | "big" | "Big" | "bigg" | "Bigg" | "mathop" => TexNode::Row(Vec::new()),
            other => {
                if let Some((ch, class)) = symbol(other) {
                    TexNode::Symbol { ch, class }
                } else if FUNCTIONS.contains(&other) {
                    TexNode::Operator { name: other.to_owned(), limits: LIMIT_FUNCTIONS.contains(&other) }
                } else {
                    // Unknown: shown as written, so a missing symbol is visible rather than an error.
                    TexNode::Text(format!("\\{other}"))
                }
            }
        })
    }

    fn environment(&mut self) -> Result<TexNode, TexError> {
        self.skip_spaces();
        if self.peek() != Some('{') {
            return Err(self.error("expected {environment}"));
        }
        self.at += 1;
        let mut name = String::new();
        while let Some(c) = self.peek() {
            self.at += 1;
            if c == '}' {
                break;
            }
            name.push(c);
        }
        let (open, close, align) = match name.as_str() {
            "matrix" => (None, None, ColumnAlignment::Center),
            "pmatrix" => (Some('('), Some(')'), ColumnAlignment::Center),
            "bmatrix" => (Some('['), Some(']'), ColumnAlignment::Center),
            "Bmatrix" => (Some('{'), Some('}'), ColumnAlignment::Center),
            "vmatrix" => (Some('|'), Some('|'), ColumnAlignment::Center),
            "Vmatrix" => (Some('‖'), Some('‖'), ColumnAlignment::Center),
            "cases" => (Some('{'), None, ColumnAlignment::Left),
            "aligned" | "align" | "align*" | "array" | "split" | "gathered" | "eqnarray" => (None, None, ColumnAlignment::Left),
            other => return Err(self.error(&format!("unknown environment {other}"))),
        };
        if name == "array" {
            // The column spec `{ccc}` carries no information we draw.
            self.skip_spaces();
            if self.peek() == Some('{') {
                while let Some(c) = self.peek() {
                    self.at += 1;
                    if c == '}' {
                        break;
                    }
                }
            }
        }
        self.enter()?;
        let mut rows: Vec<Vec<TexNode>> = Vec::new();
        let mut row: Vec<TexNode> = Vec::new();
        loop {
            let cell = self.row(Until::Cell)?;
            row.push(cell);
            self.skip_spaces();
            match self.peek() {
                Some('&') => self.at += 1,
                Some('\\') if self.chars.get(self.at + 1) == Some(&'\\') => {
                    self.at += 2;
                    rows.push(std::mem::take(&mut row));
                }
                Some('\\') if self.at_command("end") => {
                    self.at += 1;
                    self.command_name();
                    // `{name}`
                    self.skip_spaces();
                    if self.peek() == Some('{') {
                        while let Some(c) = self.peek() {
                            self.at += 1;
                            if c == '}' {
                                break;
                            }
                        }
                    }
                    if !row.iter().all(|cell| *cell == TexNode::Row(Vec::new())) || rows.is_empty() {
                        rows.push(row);
                    }
                    break;
                }
                _ => return Err(self.error("unterminated environment")),
            }
        }
        self.depth -= 1;
        Ok(TexNode::Matrix { rows, open, close, align })
    }
}

/// Parses a formula (without the surrounding `$`).
pub fn parse(source: &str) -> Result<TexNode, TexError> {
    let mut parser = Parser { chars: source.chars().collect(), at: 0, depth: 0, _source: source };
    let node = parser.row(Until::End)?;
    if parser.at < parser.chars.len() {
        return Err(parser.error("unexpected input"));
    }
    Ok(node)
}

// -- Unicode text ----------------------------------------------------------------------------------------------------------------------

fn is_atomic(node: &TexNode) -> bool {
    match node {
        TexNode::Ident(_) | TexNode::Number(_) | TexNode::Symbol { .. } | TexNode::Text(_) | TexNode::Operator { .. } => true,
        TexNode::Font { body, .. } | TexNode::Accent { body, .. } => is_atomic(body),
        TexNode::Row(items) => items.len() <= 1 && items.iter().all(is_atomic),
        _ => false,
    }
}

fn combine(text: &str, mark: char) -> String {
    text.chars().flat_map(|c| [c, mark]).collect()
}

/// The formula as one line of Unicode text: scripts as super/subscript characters where they exist (`x²`, `aᵢ₊₁`), otherwise `^(..)`/`_(..)`;
/// fractions as `a/b`; roots as `√`; accents as combining marks. Meant for running text, where a box layout cannot go.
pub fn to_unicode(node: &TexNode) -> String {
    let mut out = String::new();
    write_unicode(node, &mut out);
    out
}

fn grouped(node: &TexNode) -> String {
    let text = to_unicode(node);
    if is_atomic(node) {
        text
    } else {
        format!("({text})")
    }
}

fn write_unicode(node: &TexNode, out: &mut String) {
    match node {
        TexNode::Row(items) => items.iter().for_each(|item| write_unicode(item, out)),
        TexNode::Ident(text) | TexNode::Number(text) | TexNode::Text(text) => out.push_str(text),
        TexNode::Symbol { ch, class } => match class {
            SymbolClass::Binary | SymbolClass::Relation => {
                if !out.is_empty() && !out.ends_with(' ') {
                    out.push(' ');
                }
                out.push(*ch);
                out.push(' ');
            }
            SymbolClass::Punctuation => {
                out.push(*ch);
                out.push(' ');
            }
            _ => out.push(*ch),
        },
        TexNode::Operator { name, .. } => {
            out.push_str(name);
            out.push('\u{2009}');
        }
        TexNode::Frac { numerator, denominator, rule } => {
            if *rule {
                out.push_str(&format!("{}/{}", grouped(numerator), grouped(denominator)));
            } else {
                out.push_str(&format!("{} {}", to_unicode(numerator), to_unicode(denominator)));
            }
        }
        TexNode::Sqrt { index, body } => {
            match index.as_deref().map(to_unicode).as_deref() {
                None => out.push('√'),
                Some("3") => out.push('∛'),
                Some("4") => out.push('∜'),
                Some(other) => {
                    out.push_str(&super::parse::to_script(other, true).unwrap_or_else(|| format!("({other})")));
                    out.push('√');
                }
            }
            out.push_str(&grouped(body));
        }
        TexNode::Script { base, sub, sup } => {
            write_unicode(base, out);
            for (script, up) in [(sub, false), (sup, true)] {
                let Some(script) = script else { continue };
                let text = to_unicode(script);
                let compact: String = text.chars().filter(|c| !c.is_whitespace()).collect();
                let primes = up && !compact.is_empty() && compact.chars().all(|c| c == '′');
                match super::parse::to_script(&compact, up).or_else(|| primes.then(|| compact.clone())) {
                    Some(mapped) => out.push_str(&mapped),
                    None => {
                        out.push(if up { '^' } else { '_' });
                        out.push_str(&if is_atomic(script) && text.chars().count() == 1 { text } else { format!("({text})") });
                    }
                }
            }
        }
        TexNode::Accent { kind, body } => {
            let text = to_unicode(body);
            let mark = match kind {
                AccentKind::Hat => '\u{302}',
                AccentKind::Bar | AccentKind::Overline => '\u{305}',
                AccentKind::Vec => '\u{20d7}',
                AccentKind::Dot => '\u{307}',
                AccentKind::DoubleDot => '\u{308}',
                AccentKind::Tilde => '\u{303}',
                AccentKind::Underline => '\u{332}',
            };
            out.push_str(&combine(&text, mark));
        }
        TexNode::Delimited { open, close, body } => {
            out.extend(open);
            write_unicode(body, out);
            out.extend(close);
        }
        TexNode::Matrix { rows, open, close, .. } => {
            out.push(open.unwrap_or('['));
            let rows_text: Vec<String> = rows.iter().map(|row| row.iter().map(to_unicode).collect::<Vec<_>>().join(", ")).collect();
            out.push_str(&rows_text.join("; "));
            out.push(close.unwrap_or(']'));
        }
        TexNode::Space(width) => {
            if *width >= 0.9 {
                out.push_str("  ");
            } else if *width > 0.0 {
                out.push('\u{2009}');
            }
        }
        TexNode::Font { font, body } => {
            let mut inner = String::new();
            write_unicode(body, &mut inner);
            out.extend(inner.chars().map(|c| style_char(*font, c)));
        }
    }
}

/// Where a run of a formula sits relative to the baseline.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScriptLevel {
    Normal,
    Superscript,
    Subscript,
}

fn push_run(out: &mut Vec<(String, ScriptLevel)>, text: String, level: ScriptLevel) {
    if text.is_empty() {
        return;
    }
    match out.last_mut() {
        Some((last, last_level)) if *last_level == level => last.push_str(&text),
        _ => out.push((text, level)),
    }
}

/// The formula as runs of text with their level: like [`to_unicode`], but a script on a plain base (`x^2`, `a_{n+1}`, `e^{i\pi}`) is its own
/// raised or lowered run instead of Unicode super/subscript characters, for text blocks that can shift a run from the baseline. Everything
/// else (fractions, roots, accents, scripts inside scripts) is written as in [`to_unicode`] at the level it sits at.
pub fn to_runs(node: &TexNode) -> Vec<(String, ScriptLevel)> {
    let mut out = Vec::new();
    let items: &[TexNode] = match node {
        TexNode::Row(items) => items,
        other => std::slice::from_ref(other),
    };
    let mut buffer = String::new();
    for (index, item) in items.iter().enumerate() {
        let TexNode::Script { base, sub, sup } = item else {
            write_unicode(item, &mut buffer);
            continue;
        };
        write_unicode(base, &mut buffer);
        push_run(&mut out, std::mem::take(&mut buffer), ScriptLevel::Normal);
        for (script, level) in [(sub, ScriptLevel::Subscript), (sup, ScriptLevel::Superscript)] {
            if let Some(script) = script {
                push_run(&mut out, to_unicode(script).trim().to_owned(), level);
            }
        }
        // A binary operator or relation after the script still gets the space before it that `to_unicode` would write.
        if matches!(items.get(index + 1), Some(TexNode::Symbol { class: SymbolClass::Binary | SymbolClass::Relation, .. })) {
            buffer.push(' ');
        }
    }
    push_run(&mut out, buffer, ScriptLevel::Normal);
    out
}

/// [`parse`] then [`to_unicode`]; `None` when the formula does not parse.
pub fn tex_to_unicode(source: &str) -> Option<String> {
    parse(source).ok().map(|node| to_unicode(&node).trim().to_owned())
}

// -- Layout ----------------------------------------------------------------------------------------------------------------------------

/// Fonts, size and colour for [`draw`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TexStyle {
    pub font_size: u16,
    pub color: Vec4,
    /// Variables (letters).
    pub italic_font: u16,
    /// Numbers, operators, text.
    pub roman_font: u16,
    pub bold_font: u16,
    pub mono_font: u16,
    pub rule_thickness: f32,
    /// Script size relative to its parent.
    pub script_scale: f32,
}

impl Default for TexStyle {
    fn default() -> Self {
        Self {
            font_size: 18,
            color: Vec4::new(0.92, 0.93, 0.95, 1.0),
            italic_font: 0,
            roman_font: 0,
            bold_font: 0,
            mono_font: 0,
            rule_thickness: 1.0,
            script_scale: 0.7,
        }
    }
}

/// A rough height in lines of text, to size delimiters.
fn height_em(node: &TexNode) -> f32 {
    match node {
        TexNode::Row(items) => items.iter().map(height_em).fold(1.0, f32::max),
        TexNode::Frac { numerator, denominator, .. } => height_em(numerator) + height_em(denominator) + 0.2,
        TexNode::Sqrt { body, .. } => height_em(body) + 0.2,
        TexNode::Script { base, sub, sup } => {
            let extra = if sub.is_some() || sup.is_some() { 0.6 } else { 0.0 };
            height_em(base) + extra
        }
        TexNode::Matrix { rows, .. } => rows.len() as f32 * 1.3,
        TexNode::Delimited { body, .. } | TexNode::Accent { body, .. } | TexNode::Font { body, .. } => height_em(body),
        _ => 1.0,
    }
}

struct Painter<'s> {
    style: &'s TexStyle,
    font: Option<TexFont>,
}

impl Painter<'_> {
    fn text(&self, ui: &mut Ui<'_>, text: &str, size: u16, font_id: u16) {
        ui.text(text, &TextStyle { color: self.style.color, font_id, font_size: size, ..TextStyle::default() });
    }

    fn leaf_font(&self, italic: bool) -> u16 {
        match self.font {
            Some(TexFont::Bold) => self.style.bold_font,
            Some(TexFont::Monospace) => self.style.mono_font,
            Some(TexFont::Italic) => self.style.italic_font,
            Some(_) => self.style.roman_font,
            None if italic => self.style.italic_font,
            None => self.style.roman_font,
        }
    }

    fn row_element(&self, direction: Direction, align: ChildAlignment) -> ElementDesc {
        ElementDesc::new().width(Sizing::Fit).height(Sizing::Fit).direction(direction).align(align)
    }

    fn scaled(&self, size: u16) -> u16 {
        ((f32::from(size) * self.style.script_scale).round() as u16).max(8)
    }

    fn pad(&self, size: u16, em: f32) -> u16 {
        (f32::from(size) * em).round() as u16
    }

    fn node(&mut self, ui: &mut Ui<'_>, node: &TexNode, size: u16) {
        match node {
            TexNode::Row(items) => {
                let _row = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Center)));
                for item in items {
                    self.node(ui, item, size);
                }
            }
            TexNode::Ident(text) => {
                let shown: String = match self.font {
                    Some(font @ (TexFont::Blackboard | TexFont::Calligraphic | TexFont::Fraktur)) => text.chars().map(|c| style_char(font, c)).collect(),
                    _ => text.clone(),
                };
                self.text(ui, &shown, size, self.leaf_font(true));
            }
            TexNode::Number(text) => self.text(ui, text, size, self.leaf_font(false)),
            TexNode::Text(text) => self.text(ui, text, size, self.style.roman_font),
            TexNode::Operator { name, .. } => {
                let _pad = ui.element(&ElementDesc::new().padding(Padding { right: self.pad(size, 0.17), ..Padding::default() }));
                self.text(ui, name, size, self.style.roman_font);
            }
            TexNode::Symbol { ch, class } => {
                let space = match class {
                    SymbolClass::Binary => 0.22,
                    SymbolClass::Relation => 0.28,
                    SymbolClass::Punctuation => 0.0,
                    _ => 0.0,
                };
                let right = if *class == SymbolClass::Punctuation { 0.17 } else { space };
                let _spaced = ui.element(&ElementDesc::new().padding(Padding { left: self.pad(size, space), right: self.pad(size, right), top: 0, bottom: 0 }));
                self.text(ui, &ch.to_string(), if *class == SymbolClass::BigOperator { size + size / 3 } else { size }, self.style.roman_font);
            }
            TexNode::Space(width) => {
                let _gap = ui.element(&ElementDesc::new().width(Sizing::Fixed((f32::from(size) * width).max(0.0))).height(Sizing::Fixed(1.0)));
            }
            TexNode::Font { font, body } => {
                let previous = self.font.replace(*font);
                self.node(ui, body, size);
                self.font = previous;
            }
            TexNode::Frac { numerator, denominator, rule } => {
                let _frac = ui.element(
                    &self
                        .row_element(Direction::TopToBottom, ChildAlignment::new(AlignX::Center, AlignY::Top))
                        .padding(Padding::symmetric(self.pad(size, 0.15), 0)),
                );
                self.node(ui, numerator, size);
                if *rule {
                    let _rule = ui.element(
                        &ElementDesc::new()
                            .width(Sizing::Grow)
                            .height(Sizing::Fixed(self.style.rule_thickness))
                            .background(self.style.color)
                            .padding(Padding::symmetric(0, 0)),
                    );
                } else {
                    let _gap = ui.element(&ElementDesc::new().width(Sizing::Fixed(1.0)).height(Sizing::Fixed(f32::from(size) * 0.15)));
                }
                self.node(ui, denominator, size);
            }
            TexNode::Sqrt { index, body } => {
                let _root = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Bottom)));
                if let Some(index) = index {
                    self.node(ui, index, self.scaled(size));
                }
                self.text(ui, "√", size + size / 4, self.style.roman_font);
                let _body = ui.element(
                    &self
                        .row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Center))
                        .padding(Padding { left: 1, right: 2, top: 1, bottom: 0 })
                        .border(Border { color: self.style.color, top: self.style.rule_thickness as u16, ..Border::default() }),
                );
                self.node(ui, body, size);
            }
            TexNode::Script { base, sub, sup } => {
                let limits = matches!(&**base, TexNode::Symbol { class: SymbolClass::BigOperator, ch } if !matches!(ch, '∫' | '∬' | '∭' | '∮'))
                    || matches!(&**base, TexNode::Operator { limits: true, .. });
                let small = self.scaled(size);
                if limits {
                    let _stack = ui.element(&self.row_element(Direction::TopToBottom, ChildAlignment::new(AlignX::Center, AlignY::Center)));
                    if let Some(sup) = sup {
                        self.node(ui, sup, small);
                    }
                    self.node(ui, base, size);
                    if let Some(sub) = sub {
                        self.node(ui, sub, small);
                    }
                } else {
                    let _scripted = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Center)));
                    self.node(ui, base, size);
                    let raise = self.pad(size, 0.45);
                    let padding = match (sub.is_some(), sup.is_some()) {
                        (true, false) => Padding { top: raise, ..Padding::default() },
                        (false, true) => Padding { bottom: raise, ..Padding::default() },
                        _ => Padding::default(),
                    };
                    let _column = ui.element(&self.row_element(Direction::TopToBottom, ChildAlignment::new(AlignX::Left, AlignY::Center)).padding(padding));
                    if let Some(sup) = sup {
                        self.node(ui, sup, small);
                    }
                    if let Some(sub) = sub {
                        self.node(ui, sub, small);
                    }
                }
            }
            TexNode::Accent { kind, body } => {
                match kind {
                    AccentKind::Overline | AccentKind::Bar => {
                        let _over = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::default()).border(Border { color: self.style.color, top: 1, ..Border::default() }));
                        self.node(ui, body, size);
                    }
                    AccentKind::Underline => {
                        let _under = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::default()).border(Border { color: self.style.color, bottom: 1, ..Border::default() }));
                        self.node(ui, body, size);
                    }
                    other => {
                        let mark = match other {
                            AccentKind::Hat => "^",
                            AccentKind::Vec => "→",
                            AccentKind::Dot => "˙",
                            AccentKind::DoubleDot => "¨",
                            _ => "~",
                        };
                        let _stack = ui.element(&self.row_element(Direction::TopToBottom, ChildAlignment::new(AlignX::Center, AlignY::Top)));
                        self.text(ui, mark, self.scaled(size), self.style.roman_font);
                        self.node(ui, body, size);
                    }
                }
            }
            TexNode::Delimited { open, close, body } => {
                let scale = height_em(body);
                let big = (f32::from(size) * scale.clamp(1.0, 3.0)).round() as u16;
                let _group = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Center)));
                if let Some(open) = open {
                    self.text(ui, &open.to_string(), big, self.style.roman_font);
                }
                self.node(ui, body, size);
                if let Some(close) = close {
                    self.text(ui, &close.to_string(), big, self.style.roman_font);
                }
            }
            TexNode::Matrix { rows, open, close, align } => {
                let columns = rows.iter().map(Vec::len).max().unwrap_or(0);
                let big = (f32::from(size) * (rows.len() as f32 * 1.2).clamp(1.0, 4.0)).round() as u16;
                let _group = ui.element(&self.row_element(Direction::LeftToRight, ChildAlignment::new(AlignX::Left, AlignY::Center)));
                if let Some(open) = open {
                    self.text(ui, &open.to_string(), big, self.style.roman_font);
                }
                let x = if *align == ColumnAlignment::Left { AlignX::Left } else { AlignX::Center };
                for column in 0..columns {
                    let _column = ui.element(
                        &self
                            .row_element(Direction::TopToBottom, ChildAlignment::new(x, AlignY::Top))
                            .padding(Padding::symmetric(self.pad(size, 0.35), 0)),
                    );
                    for row in rows {
                        match row.get(column) {
                            Some(cell) => self.node(ui, cell, size),
                            None => {
                                let _empty = ui.element(&ElementDesc::new().width(Sizing::Fixed(1.0)).height(Sizing::Fixed(f32::from(size))));
                            }
                        }
                    }
                }
                if let Some(close) = close {
                    self.text(ui, &close.to_string(), big, self.style.roman_font);
                }
            }
        }
    }
}

/// Draws the formula as elements inside the currently open element: fractions with a rule, scripts raised and lowered, roots with an
/// overbar, limits above and below big operators, matrices in columns, delimiters sized to their content. Placement is by nested boxes
/// (centred alignment), so it is an approximation of TeX's baseline layout, good for typical formulas.
pub fn draw(ui: &mut Ui<'_>, node: &TexNode, style: &TexStyle) {
    let mut painter = Painter { style, font: None };
    painter.node(ui, node, style.font_size);
}
