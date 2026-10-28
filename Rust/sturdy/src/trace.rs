//! Engine stack traces: where inside the C++ engine something went wrong, readable from Rust.
//!
//! An engine call that fails returns an `Err` with a message. The message says *what*; the trace says
//! *where*, across the language boundary. It has up to three layers, innermost first:
//!
//! * **Foreign** - the line of Rust you announced with [`announce_call_site`] or [`trace_here!`](crate::trace_here);
//! * **Scope** - named engine scopes (`Renderer::submit_frame`), the engine's own logical stack. They need no debug
//!   symbols, survive stripped builds, and exist on WebAssembly where a native stack cannot be walked;
//! * **Unwound** - engine scopes a C++ exception travelled through, i.e. the logical stack at the throw;
//! * **Native** - machine frames as module + offset (+ symbol when the platform has one). The offset is stable across
//!   runs, so `addr2line -e <module> <offset>` or `llvm-symbolizer` resolves it offline.
//!
//! Traces are per thread: read one on the thread that made the failing call, straight after the `Err`.
//!
//! ```ignore
//! if let Err(error) = engine.some_call() {
//!     eprintln!("{error}\n{}", sturdy::trace::last_error_trace());
//! }
//! ```
//!
//! How much is captured is process-wide ([`set_detail`]); `STURDY_TRACE=off|scopes|native` sets the initial value.
//! Debug builds default to native, release builds to scopes.

use core::fmt;
use std::path::Path;

use sturdy_sys::diagnostics::ffi;

/// How much future failures capture.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TraceDetail {
    /// Nothing.
    Off = 0,
    /// The announced Rust call site and engine scopes. Cheap enough for release builds.
    Scopes = 1,
    /// Scopes plus the native call stack.
    Native = 2,
}

/// Which layer a [`TraceFrame`] came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TraceFrameKind {
    /// A machine frame: `address`, `module`, `module_offset`, and `function`/`file`/`line` when known.
    Native,
    /// A named engine scope; `function` is its name.
    Scope,
    /// The call site announced from Rust; `module` holds the language tag.
    Foreign,
    /// An engine scope an exception unwound through; `function` is its name.
    Unwound,
}

/// One frame of an engine trace.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraceFrame {
    pub kind: TraceFrameKind,
    /// Absolute instruction address (native frames).
    pub address: u64,
    /// Address relative to the load base of `module`; stable across runs.
    pub module_offset: u64,
    pub line: u32,
    pub column: u32,
    pub function: String,
    pub file: String,
    /// Native frames: the executable or library path. Foreign frames: the language tag.
    pub module: String,
}

impl fmt::Display for TraceFrame {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.kind {
            TraceFrameKind::Foreign => {
                let language: &str = if self.module.is_empty() { "foreign" } else { self.module.as_str() };
                write!(f, "at [{language}] {}", self.file)?;
                if self.line != 0 {
                    write!(f, ":{}", self.line)?;
                    if self.column != 0 {
                        write!(f, ":{}", self.column)?;
                    }
                }
                if !self.function.is_empty() {
                    write!(f, " ({})", self.function)?;
                }
                Ok(())
            }
            TraceFrameKind::Scope => write!(f, "in {}", self.function),
            TraceFrameKind::Unwound => write!(f, "thrown through {}", self.function),
            TraceFrameKind::Native => {
                write!(f, "0x{:x}", self.address)?;
                if !self.module.is_empty() {
                    write!(f, " {}+0x{:x}", self.module, self.module_offset)?;
                }
                if !self.function.is_empty() {
                    write!(f, " {}", self.function)?;
                }
                if !self.file.is_empty() {
                    write!(f, " ({}:{})", self.file, self.line)?;
                }
                Ok(())
            }
        }
    }
}

/// A resolved trace, innermost frame first. `Display` prints one frame per line.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EngineTrace {
    pub frames: Vec<TraceFrame>,
}

impl EngineTrace {
    /// Whether nothing was captured (tracing off, or no failure yet on this thread).
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }

    /// Only the engine's named scopes (and unwound ones): the logical stack, without machine frames.
    pub fn scopes(&self) -> impl Iterator<Item = &TraceFrame> {
        self.frames.iter().filter(|f| matches!(f.kind, TraceFrameKind::Scope | TraceFrameKind::Unwound))
    }

    /// Only the native machine frames.
    pub fn native_frames(&self) -> impl Iterator<Item = &TraceFrame> {
        self.frames.iter().filter(|f| f.kind == TraceFrameKind::Native)
    }
}

impl fmt::Display for EngineTrace {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.frames.is_empty() {
            return f.write_str("  (no engine trace: tracing is off or nothing failed on this thread)");
        }
        for (index, frame) in self.frames.iter().enumerate() {
            if index > 0 {
                f.write_str("\n")?;
            }
            write!(f, "  {frame}")?;
        }
        Ok(())
    }
}

fn convert(frame: ffi::TraceFrame) -> TraceFrame {
    TraceFrame {
        kind: match frame.kind {
            0 => TraceFrameKind::Native,
            1 => TraceFrameKind::Scope,
            2 => TraceFrameKind::Foreign,
            _ => TraceFrameKind::Unwound,
        },
        address: frame.address,
        module_offset: frame.module_offset,
        line: frame.line,
        column: frame.column,
        function: frame.function,
        file: frame.file,
        module: frame.module,
    }
}

/// Chooses how much future failures capture, for the whole process.
pub fn set_detail(detail: TraceDetail) {
    ffi::trace_set_detail(detail as u8);
}

/// The current capture detail.
pub fn detail() -> TraceDetail {
    match ffi::trace_detail() {
        0 => TraceDetail::Off,
        1 => TraceDetail::Scopes,
        _ => TraceDetail::Native,
    }
}

/// The calling thread's most recent trace: the last engine failure's, or the last [`capture`]. Reading does
/// not change it.
pub fn last_error_trace() -> EngineTrace {
    EngineTrace { frames: ffi::last_error_trace().into_iter().map(convert).collect() }
}

/// The same trace as text, one frame per line.
pub fn last_error_trace_text() -> String {
    ffi::last_error_trace_text()
}

/// Captures the engine's side of the stack right now (announced call site, engine scopes, native stack per
/// [`detail`]) and returns it. Use it where you want a trace without a failure, such as a panic hook.
pub fn capture() -> EngineTrace {
    ffi::trace_capture();
    last_error_trace()
}

/// Tells the engine where this thread's Rust code is, so a failure raised deep in C++ can be traced back to
/// this line. Sticky until replaced or [`clear_call_site`]d, so announcing once per frame or per widget is
/// as good as before every call. Re-announcing the same site is cheap.
#[track_caller]
pub fn announce_call_site() {
    let location = core::panic::Location::caller();
    announce_call_site_at(location.file(), location.line(), location.column(), "");
}

/// Like [`announce_call_site`] with the site given explicitly (what [`trace_here!`](crate::trace_here) expands to).
pub fn announce_call_site_at(file: &str, line: u32, column: u32, function: &str) {
    ffi::trace_set_call_site("rust", file, line, column, function);
}

/// Forgets this thread's announced call site.
pub fn clear_call_site() {
    ffi::trace_clear_call_site();
}

/// Installs crash handlers that print the crashing thread's engine scopes and native stack before the
/// process dies (fatal signals on Unix, the unhandled-exception filter on Windows). `log_path` also appends
/// the report to a file. Returns `false` where the platform has no hook (WebAssembly).
pub fn install_crash_handler(log_path: Option<&Path>) -> bool {
    let path = log_path.map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
    ffi::trace_install_crash_handler(&path)
}

/// Chains a panic hook that prints the engine's side of the stack (announced call site, engine scopes, native
/// stack) before the previous hook runs, so a Rust panic inside an engine callback shows both languages.
pub fn install_panic_hook() {
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let trace = capture();
        eprintln!("sturdy: engine stack at panic:\n{trace}");
        previous(info);
    }));
}

/// `message` followed by the calling thread's last engine trace, for logging an error in one go.
pub fn describe(message: &dyn fmt::Display) -> String {
    format!("{message}\nengine trace:\n{}", last_error_trace())
}

/// Announces the current Rust source line to the engine (see [`announce_call_site`]), with the module path as
/// the function. Put it at the top of a frame or widget builder.
#[macro_export]
macro_rules! trace_here {
    () => {
        $crate::trace::announce_call_site_at(file!(), line!(), column!(), module_path!())
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    /// The trace detail is process-wide, so tests that change it must not overlap.
    static SERIAL: Mutex<()> = Mutex::new(());

    #[test]
    fn announced_call_site_leads_a_captured_trace() {
        let _guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        set_detail(TraceDetail::Native);
        announce_call_site();
        let line = line!() - 1;
        let trace = capture();
        let first = trace.frames.first().expect("a capture has frames");
        assert_eq!(first.kind, TraceFrameKind::Foreign);
        assert_eq!(first.module, "rust");
        assert!(first.file.ends_with("trace.rs"), "file was {}", first.file);
        assert_eq!(first.line, line);
        assert!(first.column > 0);
        let text = trace.to_string();
        assert!(text.contains("[rust]") && text.contains("trace.rs"), "text was {text}");

        // Reading does not change it; clearing the site removes the leading frame from the next capture.
        assert_eq!(last_error_trace(), trace);
        clear_call_site();
        let after = capture();
        assert!(after.frames.first().map_or(true, |f| f.kind != TraceFrameKind::Foreign));
        // The engine's own text rendering of the same trace has one line per frame.
        let engine_text = last_error_trace_text();
        assert_eq!(engine_text.lines().count(), after.frames.len(), "text was {engine_text}");
    }

    /// With `export-symbols` the test binary exports its symbols (sturdy/build.rs), so the engine can name at least some native frames
    /// at runtime instead of printing only module + offset.
    #[cfg(all(feature = "export-symbols", any(target_os = "linux", target_os = "macos", target_os = "freebsd")))]
    #[test]
    fn export_symbols_lets_native_frames_carry_function_names() {
        let _guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        set_detail(TraceDetail::Native);
        clear_call_site();
        let trace = capture();
        let named = trace.native_frames().filter(|frame| !frame.function.is_empty()).count();
        let native = trace.native_frames().count();
        assert!(native > 0, "a native capture has machine frames");
        assert!(named > 0, "no native frame was named out of {native}; trace was:\n{trace}");
    }

    #[test]
    fn detail_round_trips_and_off_captures_nothing() {
        let _guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        set_detail(TraceDetail::Scopes);
        assert_eq!(detail(), TraceDetail::Scopes);
        set_detail(TraceDetail::Off);
        assert_eq!(detail(), TraceDetail::Off);
        announce_call_site();
        assert!(capture().is_empty(), "off captures nothing");
        set_detail(TraceDetail::Native);
        clear_call_site();
    }

    #[test]
    fn describe_appends_the_trace_and_empty_traces_say_so() {
        let _guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        set_detail(TraceDetail::Off);
        let _ = capture();
        let text = describe(&"something failed");
        assert!(text.starts_with("something failed\nengine trace:\n"));
        assert!(text.contains("no engine trace"));
        set_detail(TraceDetail::Native);
    }
}
