#pragma once

#include <Foundation/Foundation.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::UI {

    // Error reporting for the immediate-mode UI. Nothing here throws: a problem in the UI never unwinds through the caller's frame loop (which
    // matters across a language boundary -- an exception cannot cross the C ABI). Instead every problem becomes a `UiErrorReport` the host can
    // inspect, count, forward or ignore, and the UI keeps producing the best frame it can.
    //
    // Repeats of the same problem (the usual case: a bug in a per-frame builder fires every frame) collapse into one report with a counter, so
    // the list stays short and the interesting first occurrence is never pushed out by its own repetitions.

    /// What went wrong. The numeric values are part of the C ABI (`SturdyUiErrorCode`) and never change.
    enum class UiErrorCode : u32 {
        None = 0,
        /// More elements were declared than the layout buffers hold; the ones past the limit were left out of the frame.
        ElementLimit = 1,
        /// More element configurations (backgrounds, borders, clips, ...) than the buffer holds; some elements lack some of them.
        ElementConfigLimit = 2,
        /// More wrapped text lines than the buffer holds; the rest of the frame's text was not drawn.
        TextLineLimit = 3,
        /// More draw commands than the buffer holds; the end of the frame's draw list was dropped.
        DrawCommandLimit = 4,
        /// The text measurement cache ran out of entries or words; the affected strings measured as empty.
        TextCacheLimit = 5,
        /// A fixed-size context cannot hold what the layout needs.
        FixedArenaTooSmall = 6,
        /// The memory limit configured for the context stops it from growing to fit the layout. The layout is truncated.
        MemoryLimitReached = 7,
        /// The allocator refused memory. The context keeps its current size and retries.
        AllocationFailed = 8,
        /// An element was closed that was never opened (or closed twice). The extra close was ignored.
        UnbalancedClose = 9,
        /// Text or another call was given an argument the UI cannot use (no open parent, missing config). The call was ignored.
        InvalidArgument = 10,
        /// Two elements in one frame share an id; hover, bounds and scroll state follow only one of them.
        DuplicateId = 11,
        /// A floating element names a parent id that has not been declared (yet) in this frame.
        FloatingParentMissing = 12,
        /// A percentage size above 1.0 was given (100% is 1.0, not 100).
        PercentageOverOne = 13,
        /// The context has no text measuring function, so text measures as empty.
        MeasureFunctionMissing = 14,
        /// The UI caught an out-of-bounds access in its own bookkeeping and avoided it. A bug in the UI or in how it was driven.
        InternalBoundsViolation = 15,
        /// An internal traversal stack overflowed; part of the layout was skipped.
        TraversalOverflow = 16,
        /// A setting was changed at a time it cannot take effect; the change was ignored.
        SettingIgnored = 17,
        /// Dynamic debug-view strings ran out.
        DynamicStringLimit = 18,
        /// The context resized itself (informational).
        Resized = 19,
        Other = 20,
    };

    enum class UiSeverity : u32 {
        /// Informational: the UI adapted (a resize); nothing needs fixing.
        Info = 0,
        /// Something was degraded or ignored but the frame is still correct enough to show.
        Warning = 1,
        /// The frame is visibly wrong (truncated), or the UI was misused in a way that points at a bug.
        Error = 2,
    };

    [[nodiscard]] std::string_view to_string(UiErrorCode code) noexcept;
    [[nodiscard]] std::string_view to_string(UiSeverity severity) noexcept;

    /// One distinct problem, with everything needed to find it.
    struct UiErrorReport {
        UiErrorCode code = UiErrorCode::None;
        UiSeverity severity = UiSeverity::Error;
        /// What happened and what the UI did about it, in plain language.
        std::string message;
        /// For capacity problems: what the frame wanted and what the buffer holds. Otherwise 0.
        i64 value = 0;
        i64 limit = 0;
        /// Layout frame counters of the first and most recent occurrence, and how many times it happened.
        u32 first_frame = 0;
        u32 last_frame = 0;
        u64 occurrences = 0;
        /// Where inside the UI layout engine the check that failed lives: `file:line (function)`.
        std::string engine_source;
        /// The UI API call that was running (the last element/text/... call), as C++ reports it.
        Foundation::CallSite api_call_site;
        /// The call site a foreign-language caller announced (Rust, C#, ...), or an invalid one. This is how an error raised inside the engine
        /// is traced back to a line of foreign code.
        Foundation::CallSite foreign_call_site;
        /// The chain of open elements, outermost first, by the names they were given (or `<anonymous child N>`).
        std::vector<std::string> element_path;
        /// The engine trace at the first occurrence: foreign call site, engine scopes and (per the trace detail) the native stack.
        /// Addresses only until resolved; see `trace_frames`.
        Foundation::TraceCapture trace;
        /// Identity of the failing check inside the layout engine (static string pointer + line); lets repeats be recognised without
        /// building a new report. Not meaningful outside this process.
        const char *source_key_file = nullptr;
        i32 source_key_line = 0;
        /// Hash of what distinguishes two problems raised by the same check: the chain of open elements, and for problems that are about one
        /// specific id (a duplicate, a missing floating parent) that id. Without it two different problems that trip the same check would be
        /// counted as one once the fast repeat path takes over.
        u64 key_hash = 0;

        /// The trace as frames (foreign site, engine scopes, native frames with module-relative offsets a foreign debugger can symbolize).
        [[nodiscard]] std::vector<Foundation::TraceFrame> trace_frames() const { return trace.resolve(); }
    };

    /// Renders a report as multi-line text: headline, numbers, where it happened in each language, element path and (optionally) native stack.
    [[nodiscard]] std::string format_report(const UiErrorReport &report, bool include_backtrace = true);

    /// Keeps the distinct problems a UI context has hit. One per context; it lives on the heap so the context can move freely.
    class UiDiagnostics {
      public:
        using Callback = std::function<void(const UiErrorReport &)>;

        /// Upper bound on distinct reports kept; further distinct problems are counted in `dropped_reports()` only.
        static constexpr usize max_reports = 128;

        /// Records a problem (merging it into an existing identical one). Never throws; allocation failure only drops the report.
        void record(UiErrorReport &&report) noexcept;

        /// Cheap path for a problem that keeps repeating (a per-frame bug): if the same failing check has already been seen at least twice,
        /// counts this occurrence and returns true so the caller can skip building the path and stack again. Returns false when the report
        /// must be built (first and second sighting, or a check not seen before).
        [[nodiscard]] bool note_repeat(UiErrorCode code, const char *source_file, i32 source_line, u64 key_hash, u32 frame, i64 value,
                                       i64 limit) noexcept;

        /// Replaces the numbers of the report for `code` that was last seen in `frame`. Capacity problems are noticed part-way through a
        /// frame, when only a lower bound of what the frame needs is known; the context calls this once the frame is finished with the
        /// real totals. Does nothing if there is no such report.
        void update_numbers(UiErrorCode code, u32 frame, i64 value, i64 limit) noexcept;

        /// Distinct problems so far, oldest first.
        [[nodiscard]] const std::vector<UiErrorReport> &reports() const noexcept { return reports_; }
        /// Moves the reports out and starts afresh (the hosts that poll once per frame use this).
        [[nodiscard]] std::vector<UiErrorReport> take_reports() noexcept;
        void clear() noexcept;

        /// Total occurrences of problems at or above `minimum`, including repeats and dropped ones.
        [[nodiscard]] u64 occurrences(UiSeverity minimum = UiSeverity::Info) const noexcept;
        [[nodiscard]] u64 dropped_reports() const noexcept { return dropped_; }
        /// Whether any problem at `Warning` or above has been recorded since the last `clear`/`take_reports`.
        [[nodiscard]] bool has_problems() const noexcept { return occurrences(UiSeverity::Warning) != 0; }

        /// Called when a distinct problem first appears, and again when a repeat count reaches a power of two (2, 4, 8, ...), so a host sees
        /// escalation without a call per frame. Runs on the thread that hit the problem, inside the UI call; it may read the context but must
        /// not call back into the UI.
        void set_callback(Callback callback) { callback_ = std::move(callback); }

        /// Reports below this severity are kept but not written to the log. Default: Warning.
        void set_log_severity(UiSeverity minimum) noexcept { log_minimum_ = minimum; }
        /// Whether the native call stack is captured for each distinct problem. Costs a few microseconds per distinct problem, never per repeat.
        void set_capture_backtraces(bool capture) noexcept { capture_backtraces_ = capture; }
        [[nodiscard]] bool capture_backtraces() const noexcept { return capture_backtraces_; }

      private:
        std::vector<UiErrorReport> reports_;
        u64 dropped_ = 0;
        u64 totals_[3] = {0, 0, 0};
        Callback callback_;
        UiSeverity log_minimum_ = UiSeverity::Warning;
        bool capture_backtraces_ = true;
        bool in_callback_ = false;
    };

} // namespace SFT::UI
