#pragma once

#include <Foundation/Diagnostics.hpp>
#include <Foundation/Types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Foundation {

    // Engine-wide stack traces that any language can read.
    //
    // A C++ stack alone is a poor answer to "where did this go wrong?" when the caller is Rust, C#, Java or Python: the interesting frames
    // are the engine's *logical* ones ("rendering the shadow pass, inside submitting frame 812"), the binary may be stripped, and on
    // WebAssembly there is no walkable native stack at all. So a trace here is three layers, merged innermost first:
    //
    //   1. the foreign call site the calling language announced ("src/game/inventory.rs:118 draw_inventory"),
    //   2. named engine scopes (`SFT_TRACE_SCOPE("Renderer::submit_frame")`), which cost a pointer push and work everywhere,
    //   3. the native call stack, as module + offset (+ symbol when the platform has one), which a debugger can symbolize offline.
    //
    // Capturing never throws and never fails the operation being traced. How much is captured is a process-wide setting (`TraceDetail`);
    // `Off` makes every capture a no-op, `Scopes` adds only layers 1-2, `Native` adds layer 3. The default is `Native` in debug builds and
    // `Scopes` otherwise, and `STURDY_TRACE=off|scopes|native` in the environment overrides it before the first capture.

    enum class TraceDetail : u8 {
        Off = 0,
        Scopes = 1,
        Native = 2,
    };

    [[nodiscard]] TraceDetail trace_detail() noexcept;
    void set_trace_detail(TraceDetail detail) noexcept;

    /// Pushes a named engine scope for as long as it lives. `name` must be a string literal (or otherwise outlive the process): only the
    /// pointer is stored. Scopes are per thread, so work running on a worker thread has its own stack. Prefer `SFT_TRACE_SCOPE`.
    class TraceScope {
      public:
        explicit TraceScope(const char *name) noexcept;
        ~TraceScope();
        TraceScope(const TraceScope &) = delete;
        TraceScope &operator=(const TraceScope &) = delete;

      private:
        const char *name_;
        int uncaught_;
        bool active_;
    };

    /// Returns a pointer to a process-lifetime copy of `name`, for scope names that are not string literals (a plug-in's feature name, an
    /// asset label). Equal names return the same pointer. Bounded: past a few thousand distinct names it returns a fixed placeholder, so
    /// a name built from changing data (frame numbers, ids) cannot grow memory without limit -- intern only names from a small set.
    [[nodiscard]] const char *intern_trace_name(std::string_view name) noexcept;

    /// One frame of a resolved trace.
    struct TraceFrame {
        enum class Kind : u32 {
            /// A native (machine) frame: `address`, `module`, `module_offset` and, when known, `function` / `file` / `line`.
            Native = 0,
            /// A named engine scope: only `function` is set (the scope name).
            Scope = 1,
            /// The call site a foreign-language caller announced: `file` / `line` / `column` / `function`, and `module` holds the language tag.
            Foreign = 2,
            /// An engine scope that was left because an exception unwound through it, i.e. the logical stack at the moment of the throw.
            Unwound = 3,
        };
        Kind kind = Kind::Native;
        u64 address = 0;
        u64 module_offset = 0;
        u32 line = 0;
        u32 column = 0;
        std::string function;
        std::string file;
        std::string module;
    };

    /// A captured, not-yet-resolved trace. Cheap to keep around (a few pointers and addresses); `resolve()` does the expensive work.
    class TraceCapture {
      public:
        /// Captures the calling thread's trace at the configured detail. `skip_native` drops that many innermost native frames.
        /// @note This function does not throw exceptions.
        /// With `allow_native` false the native stack is skipped whatever the detail setting says: for failures that are part of ordinary
        /// use, where only the cheap layers are worth paying for.
        [[nodiscard]] static TraceCapture capture(usize skip_native = 0, bool allow_native = true) noexcept;

        [[nodiscard]] bool empty() const noexcept { return !foreign_.valid() && scopes_.empty() && unwound_.empty() && native_.empty(); }

        /// All layers as frames, innermost first: foreign site, unwound scopes, live scopes, native frames.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::vector<TraceFrame> resolve() const noexcept;

        /// `resolve()` rendered as text, one frame per line.
        [[nodiscard]] std::string format() const;

        /// Adds the scopes an in-flight exception has unwound so far (see `take_unwound_scopes`). Used where an exception is caught.
        void adopt_unwound_scopes() noexcept;

      private:
        CallSite foreign_;
        std::vector<const char *> scopes_;
        std::vector<const char *> unwound_;
        Backtrace native_;
    };

    [[nodiscard]] std::string format_trace(const std::vector<TraceFrame> &frames);

    /// The live scope names on the calling thread, innermost first (what `SFT_TRACE_SCOPE` has pushed).
    [[nodiscard]] std::vector<const char *> current_trace_scopes() noexcept;

    // ---- exceptions ---------------------------------------------------------------------------------------------------------------
    //
    // Unwinding destroys the engine's scope stack before any `catch` can look at it. So a scope destroyed *by* an exception records its
    // name instead: the list read at the catch site is the logical stack at the throw, innermost first. The list is cleared when an
    // outermost entry point starts (`reset_unwound_scopes`) so a handled exception cannot leak into a later error's trace.

    [[nodiscard]] std::vector<const char *> take_unwound_scopes() noexcept;
    void reset_unwound_scopes() noexcept;

    // ---- where an error was born ----------------------------------------------------------------------------------------------------
    //
    // An error that travels up as a value (std::expected) has no stack by the time someone reports it. Code that creates the error
    // records the trace *here*, on the calling thread; whoever eventually reports it to a foreign caller picks the trace up instead of
    // capturing its own, shallower one. Entry points call `reset_trace_origin()` first so only errors born during that call are used.

    void note_trace_origin() noexcept;
    [[nodiscard]] std::optional<TraceCapture> take_trace_origin() noexcept;
    void reset_trace_origin() noexcept;

    // ---- the trace a foreign caller can read ------------------------------------------------------------------------------------------

    /// Stores `capture` as the calling thread's most recent error trace (what `sturdy_last_error_trace_*` reads).
    void set_last_error_trace(TraceCapture capture) noexcept;
    void clear_last_error_trace() noexcept;
    /// The calling thread's most recent error trace (empty if none).
    [[nodiscard]] const TraceCapture &last_error_trace() noexcept;

    // ---- crashes ------------------------------------------------------------------------------------------------------------------------

    struct CrashTraceOptions {
        /// Also append the crash report to this file (empty: stderr only).
        std::string log_path;
        /// Write the report to stderr.
        bool write_to_stderr = true;
    };

    /// Installs process-wide handlers (fatal signals on POSIX, the unhandled-exception filter on Windows) that print the crashing thread's
    /// engine scopes and native stack before the process dies. No-op on WebAssembly, where the host reports traps itself. Safe to call
    /// more than once; the last options win. Returns false if the platform has no hook to install.
    /// @note This function does not throw exceptions.
    bool install_crash_trace_handler(const CrashTraceOptions &options = {}) noexcept;

} // namespace SFT::Foundation

#define SFT_TRACE_JOIN_INNER(a, b) a##b
#define SFT_TRACE_JOIN(a, b) SFT_TRACE_JOIN_INNER(a, b)
/// Names the enclosing scope in engine traces. `name` must be a string literal.
#define SFT_TRACE_SCOPE(name) ::SFT::Foundation::TraceScope SFT_TRACE_JOIN(sft_trace_scope_, __LINE__) { "" name }
