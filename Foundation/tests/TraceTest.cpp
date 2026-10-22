/// Checks for the engine-wide trace layer: scopes nest and unwind, the logical stack survives an exception, an error's origin is
/// recorded where it was found, detail levels gate what is captured, and the foreign call site rides along per thread.

#include <Foundation/Trace.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
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

    bool has_frame(const std::vector<Foundation::TraceFrame> &frames, Foundation::TraceFrame::Kind kind, const std::string &function) {
        for (const auto &frame : frames) {
            if (frame.kind == kind && frame.function == function) {
                return true;
            }
        }
        return false;
    }

    [[gnu::noinline]] void throw_through_scopes() {
        SFT_TRACE_SCOPE("Test::outer");
        SFT_TRACE_SCOPE("Test::inner");
        throw std::runtime_error("boom");
    }

    [[gnu::noinline]] Foundation::TraceCapture capture_in_scopes() {
        SFT_TRACE_SCOPE("Test::render");
        SFT_TRACE_SCOPE("Test::shadow_pass");
        return Foundation::TraceCapture::capture();
    }

} // namespace

int main() {
    Foundation::set_trace_detail(Foundation::TraceDetail::Native);
    Foundation::clear_foreign_call_site();
    Foundation::reset_unwound_scopes();
    Foundation::reset_trace_origin();
    Foundation::clear_last_error_trace();

    // Scopes nest, are reported innermost first, and pop when left.
    {
        check(Foundation::current_trace_scopes().empty(), "no scopes before any are entered");
        const Foundation::TraceCapture inside = capture_in_scopes();
        const auto frames = inside.resolve();
        usize first_scope = frames.size(), second_scope = frames.size();
        for (usize i = 0; i < frames.size(); ++i) {
            if (frames[i].kind == Foundation::TraceFrame::Kind::Scope) {
                if (first_scope == frames.size()) {
                    first_scope = i;
                } else if (second_scope == frames.size()) {
                    second_scope = i;
                }
            }
        }
        check(first_scope < second_scope && frames[first_scope].function == "Test::shadow_pass" && frames[second_scope].function == "Test::render",
              "scopes come out innermost first");
        check(Foundation::current_trace_scopes().empty(), "scopes pop when their block ends");
        check(!Foundation::format_trace(frames).empty() && inside.format().find("Test::shadow_pass") != std::string::npos,
              "formatted traces name the scopes");
        if (Foundation::Backtrace::supported()) {
            bool any_native = false;
            for (const auto &frame : frames) {
                any_native = any_native || frame.kind == Foundation::TraceFrame::Kind::Native;
            }
            check(any_native, "native detail adds machine frames on a platform that can walk the stack");
        }
    }

    // Detail levels gate the capture.
    {
        Foundation::set_trace_detail(Foundation::TraceDetail::Scopes);
        const auto frames = capture_in_scopes().resolve();
        bool any_native = false;
        for (const auto &frame : frames) {
            any_native = any_native || frame.kind == Foundation::TraceFrame::Kind::Native;
        }
        check(!any_native && has_frame(frames, Foundation::TraceFrame::Kind::Scope, "Test::render"), "scopes detail captures scopes but no native frames");

        Foundation::set_trace_detail(Foundation::TraceDetail::Off);
        check(capture_in_scopes().empty(), "off captures nothing");
        {
            SFT_TRACE_SCOPE("Test::while_off");
            check(Foundation::current_trace_scopes().empty(), "scopes entered while off are not recorded");
        }
        Foundation::set_trace_detail(Foundation::TraceDetail::Native);

        const auto without_native = Foundation::TraceCapture::capture(0, false).resolve();
        for (const auto &frame : without_native) {
            check(frame.kind != Foundation::TraceFrame::Kind::Native, "allow_native=false skips the native stack");
        }
    }

    // The logical stack at a throw survives the unwinding that destroys the live scopes.
    {
        Foundation::reset_unwound_scopes();
        try {
            throw_through_scopes();
        } catch (const std::exception &) {
            Foundation::TraceCapture caught = Foundation::TraceCapture::capture();
            check(Foundation::current_trace_scopes().empty(), "the live scope stack is empty by the time the catch runs");
            caught.adopt_unwound_scopes();
            const auto frames = caught.resolve();
            usize inner = frames.size(), outer = frames.size();
            for (usize i = 0; i < frames.size(); ++i) {
                if (frames[i].kind == Foundation::TraceFrame::Kind::Unwound) {
                    (frames[i].function == "Test::inner" ? inner : outer) = i;
                }
            }
            check(inner < outer && frames[outer].function == "Test::outer", "unwound scopes record the throw path, innermost first");
        }
        check(Foundation::take_unwound_scopes().empty(), "adopting the unwound scopes consumed them");

        // A scope left normally while no exception is propagating is not recorded.
        { SFT_TRACE_SCOPE("Test::calm"); }
        check(Foundation::take_unwound_scopes().empty(), "scopes left normally are not recorded as unwound");
    }

    // The origin of an error is recorded where it was found, and only used once.
    {
        Foundation::reset_trace_origin();
        check(!Foundation::take_trace_origin().has_value(), "no origin until one is noted");
        {
            SFT_TRACE_SCOPE("Test::found_here");
            Foundation::note_trace_origin();
        }
        auto origin = Foundation::take_trace_origin();
        check(origin.has_value() && has_frame(origin->resolve(), Foundation::TraceFrame::Kind::Scope, "Test::found_here"),
              "the origin keeps the scopes that were live where the error was found");
        check(!Foundation::take_trace_origin().has_value(), "taking the origin consumes it");
        Foundation::note_trace_origin();
        Foundation::reset_trace_origin();
        check(!Foundation::take_trace_origin().has_value(), "reset forgets an origin");
    }

    // The foreign call site rides along, per thread, and the last error trace is settable and clearable.
    {
        Foundation::set_foreign_call_site("rust", "src/main.rs", 12, 5, "draw");
        const auto frames = capture_in_scopes().resolve();
        check(!frames.empty() && frames.front().kind == Foundation::TraceFrame::Kind::Foreign && frames.front().file == "src/main.rs" &&
                  frames.front().line == 12 && frames.front().module == "rust",
              "the foreign call site is the first (innermost) frame");

        Foundation::set_last_error_trace(capture_in_scopes());
        check(!Foundation::last_error_trace().empty(), "the last error trace is stored");
        Foundation::clear_last_error_trace();
        check(Foundation::last_error_trace().empty(), "and cleared");

        bool other_thread_has_no_site = false;
        bool other_thread_has_no_scopes = false;
        std::thread other([&] {
            other_thread_has_no_site = !Foundation::TraceCapture::capture().resolve().empty()
                                           ? Foundation::TraceCapture::capture().resolve().front().kind != Foundation::TraceFrame::Kind::Foreign
                                           : true;
            other_thread_has_no_scopes = Foundation::current_trace_scopes().empty();
        });
        other.join();
        check(other_thread_has_no_site, "another thread does not inherit this thread's foreign call site");
        check(other_thread_has_no_scopes, "another thread has its own (empty) scope stack");
        Foundation::clear_foreign_call_site();
    }

    // Interned names are stable and shared.
    {
        const char *a = Foundation::intern_trace_name("feature:bloom");
        const char *b = Foundation::intern_trace_name(std::string{"feature:"} + "bloom");
        check(a == b && std::string{a} == "feature:bloom", "equal names intern to the same stable pointer");
        check(Foundation::intern_trace_name("feature:ssr") != a, "different names differ");
    }

    // Deep nesting beyond the recorded depth stays balanced.
    {
        struct Deep {
            static void go(int depth) {
                SFT_TRACE_SCOPE("Test::deep");
                if (depth > 0) {
                    go(depth - 1);
                }
            }
        };
        Deep::go(200);
        check(Foundation::current_trace_scopes().empty(), "scope depth beyond the recorded limit still pops back to empty");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "TraceTest: all checks passed.\n";
    return 0;
}
