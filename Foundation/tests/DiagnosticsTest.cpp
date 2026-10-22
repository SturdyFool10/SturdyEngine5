/// Checks for `Foundation::Backtrace` and the foreign call site: capture works where the platform allows it, resolution produces
/// module-relative offsets a foreign debugger can use, and the call site is per-thread and cheap to re-announce.

#include <Foundation/Diagnostics.hpp>

#include <iostream>
#include <thread>

namespace {

    using namespace SFT;

    int g_failures = 0;

    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[gnu::noinline]] Foundation::Backtrace capture_here() { return Foundation::Backtrace::capture(); }

} // namespace

int main() {
    // Backtrace: where the platform supports it, a capture from inside two frames has frames, and they resolve with module offsets.
    {
        const Foundation::Backtrace trace = capture_here();
        if (Foundation::Backtrace::supported()) {
            check(!trace.empty(), "a supported platform captures at least one frame");
            const auto frames = trace.resolve();
            check(frames.size() == trace.size(), "resolve returns one frame per captured address (or text line)");
            bool any_module = false;
            for (const auto &frame : frames) {
                if (!frame.module.empty() || !frame.symbol.empty()) {
                    any_module = true;
                }
                if (!frame.module.empty()) {
                    check(frame.module_offset != 0 && frame.module_offset <= frame.address,
                          "a frame with a module has a module-relative offset no larger than its address");
                }
            }
            check(any_module, "at least one frame resolves to a module or symbol");
            check(!Foundation::format_backtrace(frames).empty(), "formatting yields text");
        } else {
            check(trace.empty(), "an unsupported platform captures nothing, without failing");
        }
        const auto none = Foundation::Backtrace::capture(0, 0);
        check(none.empty(), "a zero frame limit captures nothing");
    }

    // Call sites: from std::source_location, and announced by a foreign caller.
    {
        const Foundation::CallSite cpp = Foundation::call_site_from(std::source_location::current());
        check(cpp.language == "c++" && cpp.line != 0 && !cpp.file.empty(), "a C++ call site carries file and line");
        check(cpp.to_string().find(':') != std::string::npos, "a call site formats with a line");

        check(!Foundation::foreign_call_site().valid(), "no foreign site until one is announced");
        Foundation::set_foreign_call_site("rust", "src/app.rs", 42, 9, "draw_panel");
        const Foundation::CallSite rust = Foundation::foreign_call_site();
        check(rust.valid() && rust.language == "rust" && rust.file == "src/app.rs" && rust.line == 42 && rust.column == 9 &&
                  rust.function == "draw_panel",
              "the announced site reads back exactly");
        check(rust.to_string() == "[rust] src/app.rs:42:9 (draw_panel)", "foreign sites format with their language tag");

        // Per thread: another thread sees none, and its own announcement does not disturb ours.
        bool other_thread_started_empty = false;
        bool other_thread_kept_its_own = false;
        std::thread other([&] {
            other_thread_started_empty = !Foundation::foreign_call_site().valid();
            Foundation::set_foreign_call_site("csharp", "Game.cs", 7, 1, "Update");
            other_thread_kept_its_own = Foundation::foreign_call_site().file == "Game.cs";
        });
        other.join();
        check(other_thread_started_empty, "a new thread starts with no foreign site");
        check(other_thread_kept_its_own, "a thread's announcement reads back on that thread");
        check(Foundation::foreign_call_site().file == "src/app.rs", "another thread's announcement does not leak into this one");

        Foundation::clear_foreign_call_site();
        check(!Foundation::foreign_call_site().valid(), "clearing forgets the site");
        Foundation::set_foreign_call_site("python", "", 0, 0, "");
        check(!Foundation::foreign_call_site().valid() || Foundation::foreign_call_site().language == "python",
              "a site with neither file nor function is not reported as a location");
        Foundation::clear_foreign_call_site();
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "DiagnosticsTest: all checks passed.\n";
    return 0;
}
