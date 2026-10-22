/// Exercises the stack-trace C ABI (`sturdy_trace_*`, `sturdy_last_error_trace_*`) without an engine: a failing call made with an
/// invalid handle produces a trace carrying the announced foreign call site, reading the trace does not disturb the error state, and
/// the detail setting and crash handler entry points behave.

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "TraceFfiTest: %s\n", description);
            ++failures;
        }
    }

    /// A call that fails inside the engine's ABI layer without needing an engine: the zero handle was never minted.
    SturdyResult fail_with_invalid_handle() {
        SturdyVulkanHandles handles{};
        return sturdy_native_vulkan(SturdyEngine{0}, &handles);
    }

    struct ReadFrame {
        SturdyTraceFrameKind kind;
        std::string function, file, module;
        uint32_t line, column;
    };

    std::vector<ReadFrame> read_trace() {
        std::vector<ReadFrame> out;
        uint32_t count = 0;
        check(sturdy_last_error_trace_frame_count(&count) == STURDY_OK, "the frame count can be read");
        for (uint32_t i = 0; i < count; ++i) {
            SturdyTraceFrame frame{};
            frame.struct_size = sizeof(frame);
            check(sturdy_last_error_trace_frame(i, &frame) == STURDY_OK, "each counted frame can be read");
            check(frame.function != nullptr && frame.file != nullptr && frame.module != nullptr, "frame strings are never null");
            out.push_back(ReadFrame{frame.kind, frame.function, frame.file, frame.module, frame.line, frame.column});
        }
        SturdyTraceFrame past{};
        past.struct_size = sizeof(past);
        check(sturdy_last_error_trace_frame(count, &past) == STURDY_ERROR_OUT_OF_RANGE, "reading past the end is out of range");
        return out;
    }

} // namespace

int main() {
    check(STURDY_ABI_VERSION_MINOR >= 37u, "the trace entry points arrived in ABI 0.37");

    // Detail round-trips and rejects nonsense.
    SturdyTraceDetail original = STURDY_TRACE_DETAIL_OFF;
    check(sturdy_trace_get_detail(&original) == STURDY_OK, "the detail can be read");
    check(sturdy_trace_get_detail(nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "reading the detail into null is rejected");
    check(sturdy_trace_set_detail(STURDY_TRACE_DETAIL_FORCE_U32) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown detail is rejected");
    check(sturdy_trace_set_detail(STURDY_TRACE_DETAIL_NATIVE) == STURDY_OK, "native detail can be set");
    SturdyTraceDetail now = STURDY_TRACE_DETAIL_OFF;
    check(sturdy_trace_get_detail(&now) == STURDY_OK && now == STURDY_TRACE_DETAIL_NATIVE, "the detail reads back");

    // A failing call records a trace that carries the announced foreign call site, innermost first.
    {
        const char language[] = "test-lang";
        const char file[] = "scripts/inventory.test";
        const char function[] = "draw_inventory";
        check(sturdy_trace_set_foreign_call_site(language, sizeof(language) - 1, file, sizeof(file) - 1, 118, 13, function,
                                                 sizeof(function) - 1) == STURDY_OK,
              "a foreign call site can be announced (not null-terminated: lengths are explicit)");

        const SturdyResult result = fail_with_invalid_handle();
        check(result == STURDY_ERROR_INVALID_HANDLE, "a never-minted handle is rejected");
        const std::string message = sturdy_last_error_message();
        check(!message.empty(), "the failure has a message");

        const std::vector<ReadFrame> frames = read_trace();
        check(!frames.empty(), "the failure recorded a trace");
        check(!frames.empty() && frames.front().kind == STURDY_TRACE_FRAME_FOREIGN && frames.front().file == file && frames.front().line == 118 &&
                  frames.front().column == 13 && frames.front().module == language && frames.front().function == function,
              "the announced foreign call site is the first frame");
        bool any_native = false;
        for (const ReadFrame &frame : frames) {
            any_native = any_native || frame.kind == STURDY_TRACE_FRAME_NATIVE;
        }
        if (!any_native) {
            // Not a failure: a platform with no native unwinder (some libcs, WebAssembly) legitimately has none.
            (void)std::puts("TraceFfiTest: note: this platform recorded no native frames");
        }

        // Reading did not disturb the error state, and the text form agrees with the structured one.
        check(std::string{sturdy_last_error_message()} == message, "reading the trace leaves the error message alone");
        uint64_t length = 0;
        check(sturdy_last_error_trace_format(nullptr, 0, &length) == STURDY_OK && length > 1, "the text size can be queried");
        std::vector<char> text(static_cast<size_t>(length));
        check(sturdy_last_error_trace_format(text.data(), text.size(), &length) == STURDY_OK, "the text can be read");
        check(std::string{text.data()}.find("scripts/inventory.test:118:13") != std::string::npos, "the text names the foreign call site");
        std::vector<char> tiny(4);
        check(sturdy_last_error_trace_format(tiny.data(), tiny.size(), &length) == STURDY_ERROR_BUFFER_TOO_SMALL && tiny.back() == '\0',
              "a short buffer is truncated, terminated and reported");
        check(std::string{sturdy_last_error_message()} == message, "even a short-buffer read leaves the error message alone");
        check(!read_trace().empty(), "and the trace is still there afterwards");
    }

    // Scopes-only detail drops the native frames; off records nothing but the failure is still reported.
    {
        sturdy_trace_set_detail(STURDY_TRACE_DETAIL_SCOPES);
        check(fail_with_invalid_handle() == STURDY_ERROR_INVALID_HANDLE, "still rejected at scopes detail");
        for (const ReadFrame &frame : read_trace()) {
            check(frame.kind != STURDY_TRACE_FRAME_NATIVE, "scopes detail records no native frames");
        }

        sturdy_trace_set_detail(STURDY_TRACE_DETAIL_OFF);
        check(fail_with_invalid_handle() == STURDY_ERROR_INVALID_HANDLE, "still rejected with tracing off");
        check(read_trace().empty(), "off records no trace");
        check(!std::string{sturdy_last_error_message()}.empty(), "but the message is still reported");
        sturdy_trace_set_detail(STURDY_TRACE_DETAIL_NATIVE);
    }

    // On-demand capture, per-thread isolation, and clearing the call site.
    {
        check(sturdy_trace_capture() == STURDY_OK, "a trace can be captured on demand");
        check(!read_trace().empty(), "the captured trace has frames");

        bool other_thread_clean = false;
        std::thread other([&] {
            uint32_t count = 99;
            other_thread_clean = sturdy_last_error_trace_frame_count(&count) == STURDY_OK && count == 0;
        });
        other.join();
        check(other_thread_clean, "another thread starts with no trace");

        check(sturdy_trace_clear_foreign_call_site() == STURDY_OK, "the call site can be cleared");
        check(fail_with_invalid_handle() == STURDY_ERROR_INVALID_HANDLE, "rejected again");
        const std::vector<ReadFrame> frames = read_trace();
        check(frames.empty() || frames.front().kind != STURDY_TRACE_FRAME_FOREIGN, "a cleared site no longer heads the trace");
    }

    // The crash handler can be installed (and reinstalled) without effect until a crash.
    {
        const SturdyResult installed = sturdy_trace_install_crash_handler(nullptr);
        check(installed == STURDY_OK || installed == STURDY_ERROR_NOT_AVAILABLE, "the crash handler installs or reports unavailable");
        check(sturdy_trace_install_crash_handler("") == installed, "reinstalling with an empty path behaves the same");
    }

    sturdy_trace_set_detail(original);
    if (failures != 0) {
        (void)std::fprintf(stderr, "TraceFfiTest: %d check(s) failed\n", failures);
        return 1;
    }
    (void)std::puts("TraceFfiTest: all checks passed.");
    return 0;
}
