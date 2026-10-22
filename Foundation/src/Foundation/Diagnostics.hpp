#pragma once

#include <Foundation/Types.hpp>

#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Foundation {

    // Error-reporting building blocks that work the same on every target and never throw: a portable native backtrace, and a
    // "call site" that a foreign-language caller (Rust, C#, Java, Python, ...) can announce so an error raised deep inside C++
    // can be traced back to the line of foreign code that triggered it.
    //
    // Nothing here depends on exceptions or on `std::stacktrace` (which needs an extra library on libstdc++ and does not exist on
    // libc++). Capture is a few cheap address reads; turning addresses into names is separate and lazy, because the common case is
    // that nobody ever looks at a given backtrace.

    /// One resolved frame of a native backtrace.
    struct BacktraceFrame {
        /// Absolute instruction address in this process.
        u64 address = 0;
        /// Address relative to the load base of `module`. This is the value to feed `addr2line`, `llvm-symbolizer` or a debugger
        /// together with the module's debug info, and it is stable across runs (ASLR does not change it) -- so a foreign-language
        /// caller can symbolize a frame offline even when this process had no symbols.
        u64 module_offset = 0;
        /// Path or name of the executable or shared library containing `address`; empty if unknown.
        std::string module;
        /// Demangled function name; empty if unknown.
        std::string symbol;
        /// Source file and line if the platform resolver provides them (most do not without a debug-info reader); otherwise empty / 0.
        std::string file;
        u32 line = 0;
    };

    /// A captured native call stack. Cheap to capture and to copy; resolve lazily with `resolve()`.
    class Backtrace {
      public:
        /// Default maximum number of frames recorded.
        static constexpr usize default_max_frames = 32;

        /// Captures the calling thread's stack.
        ///
        /// @param skip_frames How many of the innermost frames to drop (`capture` itself is always dropped).
        /// @param max_frames Upper bound on recorded frames.
        /// @return The capture; empty on a platform with no unwinder.
        /// @note This function does not throw exceptions (allocation failure yields an empty capture).
        [[nodiscard]] static Backtrace capture(usize skip_frames = 0, usize max_frames = default_max_frames) noexcept;

        /// Whether this build can capture native stacks at all. False on targets without an unwinder (some WebAssembly builds).
        [[nodiscard]] static bool supported() noexcept;

        [[nodiscard]] bool empty() const noexcept { return addresses_.empty() && text_.empty(); }
        [[nodiscard]] usize size() const noexcept { return addresses_.size() + text_.size(); }
        [[nodiscard]] const std::vector<u64> &addresses() const noexcept { return addresses_; }

        /// Turns the recorded addresses into frames (module, offset, demangled symbol). Safe to call repeatedly; each call re-resolves.
        /// Targets that can only produce text (WebAssembly) return one frame per line with the line in `symbol`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::vector<BacktraceFrame> resolve() const noexcept;

      private:
        std::vector<u64> addresses_;
        /// Pre-rendered frames for targets that cannot give addresses (WebAssembly's JS-visible call stack).
        std::vector<std::string> text_;
    };

    /// Renders frames as one line each: `#0 0x... module+0x... symbol (file:line)`.
    [[nodiscard]] std::string format_backtrace(const std::vector<BacktraceFrame> &frames);

    /// Where something happened, in whichever language it happened.
    struct CallSite {
        /// Language tag chosen by whoever reports the site ("c++", "rust", "csharp", "java", "python", ...). Empty = unknown.
        std::string language;
        std::string file;
        u32 line = 0;
        u32 column = 0;
        std::string function;

        [[nodiscard]] bool valid() const noexcept { return !file.empty() || !function.empty(); }
        /// `file:line:column (function)`, omitting parts that are unknown.
        [[nodiscard]] std::string to_string() const;
    };

    /// Builds a C++ call site from a `std::source_location`.
    [[nodiscard]] CallSite call_site_from(const std::source_location &location);

    // ---- foreign-language call site ---------------------------------------------------------------------------------------------
    //
    // A foreign caller announces where it is before calling into the engine (Rust passes `#[track_caller]`'s `Location`); any error the
    // engine reports on this thread afterwards carries it. The site is sticky until replaced or cleared, so wrapping a whole
    // frame's UI in one announcement is as valid as announcing before every call. It is per thread: engine frames run on their own
    // threads, and a process-wide slot would let one thread's site be attributed to another thread's error.

    /// Announces the foreign call site for the calling thread. Strings are copied (not retained), so they may be temporaries.
    /// Re-announcing the same site is cheap (no allocation).
    /// @note This function does not throw exceptions.
    void set_foreign_call_site(std::string_view language, std::string_view file, u32 line, u32 column,
                               std::string_view function) noexcept;

    /// Forgets the calling thread's foreign call site.
    void clear_foreign_call_site() noexcept;

    /// The calling thread's foreign call site, or an invalid (empty) one.
    [[nodiscard]] CallSite foreign_call_site();

} // namespace SFT::Foundation
