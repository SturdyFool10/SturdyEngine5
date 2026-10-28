#include <FFI/AbiSupport.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFT::Ffi {

    namespace {

        /// The calling thread's most recent error detail.
        ///
        /// Thread-local rather than process-wide because the engine renders windows on dedicated
        /// per-window threads: a shared slot would let one thread's failure overwrite the message
        /// another thread is about to read.
        thread_local std::string g_last_error_message;

        struct HandleEntry {
            HandleKind kind;
            void *pointer;
        };

        /// Guards `g_handles` and `g_next_token`.
        ///
        /// A plain mutex rather than anything lock-free: handles are minted twice per frame per
        /// window, which is nowhere near contended enough to justify the complexity.
        std::mutex g_handle_mutex;
        std::unordered_map<u64, HandleEntry> g_handles;

        /// Next token to hand out. Starts at 1 so 0 is always an invalid token, which makes a
        /// zero-initialized `SturdyEngine`/`SturdyFrame` — the shape a caller gets from `calloc`
        /// or a default-constructed binding struct — reliably rejected instead of ambiguous.
        u64 g_next_token = 1;

    } // namespace

    namespace {

        /// Failures that are part of ordinary use (the size-query loop, end of an enumeration, an entity that died between frames). A
        /// native stack walk on each of those would turn normal control flow into a cost, so they record only the cheap layers.
        [[nodiscard]] bool is_routine_failure(SturdyResult result) noexcept {
            switch (result) {
                case STURDY_ERROR_BUFFER_TOO_SMALL:
                case STURDY_ERROR_OUT_OF_RANGE:
                case STURDY_ERROR_ENTITY_NOT_ALIVE:
                case STURDY_ERROR_COMPONENT_MISSING:
                case STURDY_ERROR_COMPONENT_PRESENT:
                case STURDY_ERROR_NOT_AVAILABLE:
                case STURDY_ERROR_BUSY:
                    return true;
                default:
                    return false;
            }
        }

        /// Builds the trace for a failure: the one recorded where the error was born if there is one, else the stack at this report.
        [[nodiscard]] Foundation::TraceCapture trace_for_failure(SturdyResult result, bool from_exception) noexcept {
            Foundation::TraceCapture trace;
            if (auto origin = Foundation::take_trace_origin()) {
                trace = std::move(*origin);
            } else {
                trace = Foundation::TraceCapture::capture(2, !is_routine_failure(result));
            }
            if (from_exception) {
                trace.adopt_unwound_scopes();
            }
            return trace;
        }

        SturdyResult report_failure(SturdyResult result, std::string_view message, bool from_exception) noexcept {
            try {
                g_last_error_message.assign(message);
            } catch (...) {
                // Reporting the error must never itself fail the call. A caller that gets an empty
                // detail string still gets the correct SturdyResult, which is the part it branches on.
                g_last_error_message.clear();
            }
            Foundation::set_last_error_trace(trace_for_failure(result, from_exception));
            return result;
        }

    } // namespace

    namespace {
        thread_local std::string t_callback_failure_detail;
    } // namespace

    void set_callback_failure_detail(std::string message) noexcept {
        try {
            t_callback_failure_detail = std::move(message);
        } catch (...) {
        }
    }

    std::string take_callback_failure_detail() noexcept {
        try {
            return std::exchange(t_callback_failure_detail, std::string{});
        } catch (...) {
            return {};
        }
    }

    SturdyResult set_error(SturdyResult result, std::string_view message) noexcept {
        return report_failure(result, message, false);
    }

    SturdyResult set_exception_error(SturdyResult result, std::string_view message) noexcept {
        return report_failure(result, message, true);
    }

    void begin_call() noexcept {
        Foundation::reset_trace_origin();
        Foundation::reset_unwound_scopes();
    }

    void clear_error() noexcept {
        g_last_error_message.clear();
        Foundation::clear_last_error_trace();
    }

    u64 mint_handle(HandleKind kind, void *pointer) {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};
        const u64 token = g_next_token++;
        g_handles.emplace(token, HandleEntry{kind, pointer});
        return token;
    }

    void revoke_handle(u64 token) noexcept {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};
        g_handles.erase(token);
    }

    SturdyResult resolve_handle(u64 token, HandleKind kind, void **out_pointer) noexcept {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};

        const auto entry = g_handles.find(token);
        if (entry == g_handles.end()) {
            // Tokens are handed out in increasing order and never reused, so anything below the
            // counter was minted at some point and has since been revoked. That distinction is
            // worth reporting: "expired" tells a caller it kept a handle past its callback, while
            // "invalid" tells it the value never came from this ABI at all.
            if (token != 0 && token < g_next_token) {
                return set_error(STURDY_ERROR_HANDLE_EXPIRED,
                                 "handle used outside the callback that provided it");
            }
            return set_error(STURDY_ERROR_INVALID_HANDLE, "handle was never valid");
        }

        if (entry->second.kind != kind) {
            return set_error(STURDY_ERROR_INVALID_HANDLE, "handle refers to a different object kind");
        }

        *out_pointer = entry->second.pointer;
        return STURDY_OK;
    }

    SturdyResult resolve_engine(SturdyEngine engine, Engine::Engine **out_engine) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(engine.token, HandleKind::Engine, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_engine = static_cast<Engine::Engine *>(pointer);
        return STURDY_OK;
    }

    SturdyResult copy_string_out(std::string_view text,
                                 char *buffer,
                                 usize capacity,
                                 usize *out_length) noexcept {
        const usize required = text.size() + 1;
        if (out_length != nullptr) {
            *out_length = required;
        }
        if (buffer == nullptr || capacity == 0) {
            // A size query is not a failure when the caller asked for one, but a caller that
            // passed a real buffer of zero capacity did not get its string.
            return buffer == nullptr ? STURDY_OK : STURDY_ERROR_BUFFER_TOO_SMALL;
        }

        const usize copied = std::min(text.size(), capacity - 1);
        std::memcpy(buffer, text.data(), copied);
        buffer[copied] = '\0';
        if (copied != text.size()) {
            return set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "the supplied text buffer is too small");
        }
        return STURDY_OK;
    }

} // namespace SFT::Ffi

extern "C" {

const char *STURDY_ABI_CALL sturdy_last_error_message(void) {
    // Never null, even before any failure: `std::string::c_str()` on an empty string returns a
    // valid pointer to a terminator, which is exactly the "empty means no error" contract.
    return SFT::Ffi::g_last_error_message.c_str();
}

// ---- stack traces -----------------------------------------------------------------------------------------------------------------
//
// None of these touch the error slot (the message or the trace being read), even when they reject an argument: a foreign caller reads a
// trace after a failure, and reading must not change what it reads.

} // extern "C" (reopened below the helpers: C++-typed helpers must not sit inside a C linkage block)

namespace {

    /// The resolved form of the calling thread's trace, kept so `sturdy_last_error_trace_frame` can hand out pointers into it. Rebuilt by
    /// `sturdy_last_error_trace_frame_count`; valid until the next trace call on this thread.
    thread_local std::vector<SFT::Foundation::TraceFrame> g_resolved_frames;

    [[nodiscard]] std::string_view view_of(const char *data, uint32_t length) noexcept {
        return data != nullptr ? std::string_view{data, length} : std::string_view{};
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_trace_set_detail(SturdyTraceDetail detail) {
    switch (detail) {
        case STURDY_TRACE_DETAIL_OFF: SFT::Foundation::set_trace_detail(SFT::Foundation::TraceDetail::Off); return STURDY_OK;
        case STURDY_TRACE_DETAIL_SCOPES: SFT::Foundation::set_trace_detail(SFT::Foundation::TraceDetail::Scopes); return STURDY_OK;
        case STURDY_TRACE_DETAIL_NATIVE: SFT::Foundation::set_trace_detail(SFT::Foundation::TraceDetail::Native); return STURDY_OK;
        case STURDY_TRACE_DETAIL_FORCE_U32: break;
    }
    return STURDY_ERROR_INVALID_ARGUMENT;
}

SturdyResult STURDY_ABI_CALL sturdy_trace_get_detail(SturdyTraceDetail *out_detail) {
    if (out_detail == nullptr) {
        return STURDY_ERROR_INVALID_ARGUMENT;
    }
    switch (SFT::Foundation::trace_detail()) {
        case SFT::Foundation::TraceDetail::Off: *out_detail = STURDY_TRACE_DETAIL_OFF; break;
        case SFT::Foundation::TraceDetail::Scopes: *out_detail = STURDY_TRACE_DETAIL_SCOPES; break;
        case SFT::Foundation::TraceDetail::Native: *out_detail = STURDY_TRACE_DETAIL_NATIVE; break;
    }
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_trace_set_foreign_call_site(const char *language,
                                                                uint32_t language_length,
                                                                const char *file,
                                                                uint32_t file_length,
                                                                uint32_t line,
                                                                uint32_t column,
                                                                const char *function,
                                                                uint32_t function_length) {
    SFT::Foundation::set_foreign_call_site(view_of(language, language_length), view_of(file, file_length), line, column,
                                           view_of(function, function_length));
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_trace_clear_foreign_call_site(void) {
    SFT::Foundation::clear_foreign_call_site();
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_trace_capture(void) {
    SFT::Foundation::set_last_error_trace(SFT::Foundation::TraceCapture::capture(1));
    g_resolved_frames.clear();
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_frame_count(uint32_t *out_count) {
    if (out_count == nullptr) {
        return STURDY_ERROR_INVALID_ARGUMENT;
    }
    try {
        g_resolved_frames = SFT::Foundation::last_error_trace().resolve();
    } catch (...) {
        g_resolved_frames.clear();
        return STURDY_ERROR_OUT_OF_MEMORY;
    }
    *out_count = static_cast<uint32_t>(g_resolved_frames.size());
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_frame(uint32_t index, SturdyTraceFrame *out_frame) {
    if (out_frame == nullptr || out_frame->struct_size < sizeof(SturdyTraceFrame)) {
        return STURDY_ERROR_INVALID_ARGUMENT;
    }
    if (g_resolved_frames.empty()) {
        // The caller skipped the count call (or the trace changed since); resolve now so the frame it asks for still exists.
        try {
            g_resolved_frames = SFT::Foundation::last_error_trace().resolve();
        } catch (...) {
            return STURDY_ERROR_OUT_OF_MEMORY;
        }
    }
    if (index >= g_resolved_frames.size()) {
        return STURDY_ERROR_OUT_OF_RANGE;
    }
    const SFT::Foundation::TraceFrame &frame = g_resolved_frames[index];
    switch (frame.kind) {
        case SFT::Foundation::TraceFrame::Kind::Native: out_frame->kind = STURDY_TRACE_FRAME_NATIVE; break;
        case SFT::Foundation::TraceFrame::Kind::Scope: out_frame->kind = STURDY_TRACE_FRAME_SCOPE; break;
        case SFT::Foundation::TraceFrame::Kind::Foreign: out_frame->kind = STURDY_TRACE_FRAME_FOREIGN; break;
        case SFT::Foundation::TraceFrame::Kind::Unwound: out_frame->kind = STURDY_TRACE_FRAME_UNWOUND; break;
    }
    out_frame->address = frame.address;
    out_frame->module_offset = frame.module_offset;
    out_frame->line = frame.line;
    out_frame->column = frame.column;
    out_frame->function = frame.function.c_str();
    out_frame->file = frame.file.c_str();
    out_frame->module = frame.module.c_str();
    return STURDY_OK;
}

SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_format(char *buffer, uint64_t capacity, uint64_t *out_length) {
    try {
        usize length = 0;
        // copy_string_out sets the error slot when the buffer is too small; save and restore it so reading the trace leaves the error
        // state exactly as the failing call left it.
        const std::string saved_message = SFT::Ffi::g_last_error_message;
        const SFT::Foundation::TraceCapture saved_trace = SFT::Foundation::last_error_trace();
        const std::string text = saved_trace.format();
        const SturdyResult result = SFT::Ffi::copy_string_out(text, buffer, static_cast<usize>(capacity), &length);
        SFT::Ffi::g_last_error_message = saved_message;
        SFT::Foundation::set_last_error_trace(saved_trace);
        if (out_length != nullptr) {
            *out_length = static_cast<uint64_t>(length);
        }
        return result;
    } catch (...) {
        return STURDY_ERROR_OUT_OF_MEMORY;
    }
}

SturdyResult STURDY_ABI_CALL sturdy_trace_install_crash_handler(const char *log_path) {
    SFT::Foundation::CrashTraceOptions options;
    if (log_path != nullptr) {
        options.log_path = log_path;
    }
    return SFT::Foundation::install_crash_trace_handler(options) ? STURDY_OK : STURDY_ERROR_NOT_AVAILABLE;
}

uint32_t STURDY_ABI_CALL sturdy_abi_version_major(void) {
    return STURDY_ABI_VERSION_MAJOR;
}

uint32_t STURDY_ABI_CALL sturdy_abi_version_minor(void) {
    return STURDY_ABI_VERSION_MINOR;
}

} // extern "C"
