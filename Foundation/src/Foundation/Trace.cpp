#include <Foundation/Trace.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(__EMSCRIPTEN__)
    // No native hooks: the host reports traps itself.
#elif defined(STURDY_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__has_include)
    #if __has_include(<signal.h>) && __has_include(<unistd.h>) && __has_include(<fcntl.h>)
        #include <fcntl.h>
        #include <signal.h>
        #include <unistd.h>
        #define STURDY_TRACE_HAVE_POSIX_SIGNALS 1
    #endif
    #if __has_include(<execinfo.h>) && !defined(__ANDROID__)
        #include <execinfo.h>
        #define STURDY_TRACE_HAVE_EXECINFO 1
    #endif
#endif

namespace SFT::Foundation {

    namespace {

        constexpr usize k_max_scopes = 64;

        /// Plain data on purpose: the crash handler reads it from a signal handler, where nothing with a constructor or an allocator is safe.
        struct ScopeStack {
            const char *names[k_max_scopes];
            u32 depth;
        };

        thread_local ScopeStack t_scopes = {};

        struct UnwoundScopes {
            std::vector<const char *> names;
        };
        thread_local UnwoundScopes t_unwound;
        thread_local std::optional<TraceCapture> t_origin;
        thread_local TraceCapture t_last_error;

        constexpr u8 k_detail_unset = 255;
        std::atomic<u8> g_detail{k_detail_unset};

        [[nodiscard]] u8 default_detail() noexcept {
            if (const char *env = std::getenv("STURDY_TRACE"); env != nullptr) {
                if (std::strcmp(env, "off") == 0 || std::strcmp(env, "0") == 0) {
                    return static_cast<u8>(TraceDetail::Off);
                }
                if (std::strcmp(env, "scopes") == 0) {
                    return static_cast<u8>(TraceDetail::Scopes);
                }
                if (std::strcmp(env, "native") == 0 || std::strcmp(env, "1") == 0) {
                    return static_cast<u8>(TraceDetail::Native);
                }
            }
#if defined(NDEBUG) && !defined(DEBUG)
            return static_cast<u8>(TraceDetail::Scopes);
#else
            return static_cast<u8>(TraceDetail::Native);
#endif
        }

    } // namespace

    TraceDetail trace_detail() noexcept {
        u8 value = g_detail.load(std::memory_order_relaxed);
        if (value == k_detail_unset) {
            // Racing initialisers compute the same value, so a plain store is fine.
            value = default_detail();
            g_detail.store(value, std::memory_order_relaxed);
        }
        return static_cast<TraceDetail>(value);
    }

    void set_trace_detail(TraceDetail detail) noexcept {
        g_detail.store(static_cast<u8>(detail), std::memory_order_relaxed);
    }

    TraceScope::TraceScope(const char *name) noexcept : name_(name), uncaught_(0), active_(trace_detail() != TraceDetail::Off) {
        if (!active_) {
            return;
        }
        uncaught_ = std::uncaught_exceptions();
        if (t_scopes.depth < k_max_scopes) {
            t_scopes.names[t_scopes.depth] = name_;
        }
        t_scopes.depth += 1;
    }

    TraceScope::~TraceScope() {
        if (!active_) {
            return;
        }
        // Leaving because an exception is propagating: remember this scope's name, so the catch site can still see the logical stack the
        // throw happened in (by the time it runs, every scope between it and the throw has been destroyed).
        if (std::uncaught_exceptions() > uncaught_) {
            try {
                if (t_unwound.names.size() < k_max_scopes) {
                    t_unwound.names.push_back(name_);
                }
            } catch (...) {
            }
        }
        if (t_scopes.depth > 0) {
            t_scopes.depth -= 1;
        }
    }

    const char *intern_trace_name(std::string_view name) noexcept {
        static std::mutex mutex;
        // Node-based, so the c_str() of an element never moves when others are inserted.
        static std::unordered_set<std::string> names;
        constexpr usize k_limit = 4096;
        try {
            const std::lock_guard<std::mutex> lock{mutex};
            if (const auto found = names.find(std::string{name}); found != names.end()) {
                return found->c_str();
            }
            if (names.size() >= k_limit) {
                return "<trace name limit reached>";
            }
            return names.emplace(name).first->c_str();
        } catch (...) {
            return "<trace name unavailable>";
        }
    }

    std::vector<const char *> current_trace_scopes() noexcept {
        std::vector<const char *> out;
        try {
            const u32 stored = t_scopes.depth < k_max_scopes ? t_scopes.depth : static_cast<u32>(k_max_scopes);
            out.reserve(stored);
            for (u32 i = stored; i > 0; --i) {
                out.push_back(t_scopes.names[i - 1]);
            }
        } catch (...) {
            out.clear();
        }
        return out;
    }

    std::vector<const char *> take_unwound_scopes() noexcept {
        std::vector<const char *> out;
        try {
            out = std::move(t_unwound.names);
        } catch (...) {
        }
        t_unwound.names.clear();
        return out;
    }

    void reset_unwound_scopes() noexcept {
        t_unwound.names.clear();
    }

    TraceCapture TraceCapture::capture(usize skip_native, bool allow_native) noexcept {
        TraceCapture result;
        const TraceDetail detail = trace_detail();
        if (detail == TraceDetail::Off) {
            return result;
        }
        try {
            result.foreign_ = foreign_call_site();
            result.scopes_ = current_trace_scopes();
            if (detail == TraceDetail::Native && allow_native) {
                // +1 drops this function's own frame as well as Backtrace::capture's.
                result.native_ = Backtrace::capture(skip_native + 1);
            }
        } catch (...) {
        }
        return result;
    }

    void TraceCapture::adopt_unwound_scopes() noexcept {
        if (trace_detail() == TraceDetail::Off) {
            return;
        }
        unwound_ = take_unwound_scopes();
    }

    std::vector<TraceFrame> TraceCapture::resolve() const noexcept {
        std::vector<TraceFrame> frames;
        try {
            if (foreign_.valid()) {
                TraceFrame frame;
                frame.kind = TraceFrame::Kind::Foreign;
                frame.module = foreign_.language;
                frame.file = foreign_.file;
                frame.line = foreign_.line;
                frame.column = foreign_.column;
                frame.function = foreign_.function;
                frames.push_back(std::move(frame));
            }
            for (const char *name : unwound_) {
                TraceFrame frame;
                frame.kind = TraceFrame::Kind::Unwound;
                frame.function = name != nullptr ? name : "";
                frames.push_back(std::move(frame));
            }
            for (const char *name : scopes_) {
                TraceFrame frame;
                frame.kind = TraceFrame::Kind::Scope;
                frame.function = name != nullptr ? name : "";
                frames.push_back(std::move(frame));
            }
            for (BacktraceFrame &native : native_.resolve()) {
                TraceFrame frame;
                frame.kind = TraceFrame::Kind::Native;
                frame.address = native.address;
                frame.module_offset = native.module_offset;
                frame.line = native.line;
                frame.function = std::move(native.symbol);
                frame.file = std::move(native.file);
                frame.module = std::move(native.module);
                frames.push_back(std::move(frame));
            }
        } catch (...) {
        }
        return frames;
    }

    std::string format_trace(const std::vector<TraceFrame> &frames) {
        std::string out;
        try {
            usize native_index = 0;
            for (const TraceFrame &frame : frames) {
                switch (frame.kind) {
                    case TraceFrame::Kind::Foreign:
                        out += std::format("  at [{}] {}", frame.module.empty() ? "foreign" : frame.module, frame.file.empty() ? "<unknown file>" : frame.file);
                        if (frame.line != 0) {
                            out += std::format(":{}", frame.line);
                            if (frame.column != 0) {
                                out += std::format(":{}", frame.column);
                            }
                        }
                        if (!frame.function.empty()) {
                            out += std::format(" ({})", frame.function);
                        }
                        out += '\n';
                        break;
                    case TraceFrame::Kind::Unwound:
                        out += std::format("  thrown through {}\n", frame.function);
                        break;
                    case TraceFrame::Kind::Scope:
                        out += std::format("  in {}\n", frame.function);
                        break;
                    case TraceFrame::Kind::Native:
                        if (frame.address == 0 && !frame.function.empty()) {
                            out += std::format("  #{} {}\n", native_index++, frame.function);
                            break;
                        }
                        out += std::format("  #{} 0x{:x}", native_index++, frame.address);
                        if (!frame.module.empty()) {
                            out += std::format(" {}+0x{:x}", frame.module, frame.module_offset);
                        }
                        if (!frame.function.empty()) {
                            out += ' ';
                            out += frame.function;
                        }
                        if (!frame.file.empty()) {
                            out += std::format(" ({}:{})", frame.file, frame.line);
                        }
                        out += '\n';
                        break;
                }
            }
        } catch (...) {
        }
        return out;
    }

    std::string TraceCapture::format() const {
        return format_trace(resolve());
    }

    void note_trace_origin() noexcept {
        if (trace_detail() == TraceDetail::Off) {
            return;
        }
        try {
            // Skip this function so the first native frame is the code that found the error.
            TraceCapture capture = TraceCapture::capture(1);
            capture.adopt_unwound_scopes();
            t_origin = std::move(capture);
        } catch (...) {
            t_origin.reset();
        }
    }

    std::optional<TraceCapture> take_trace_origin() noexcept {
        std::optional<TraceCapture> out;
        try {
            out = std::move(t_origin);
        } catch (...) {
        }
        t_origin.reset();
        return out;
    }

    void reset_trace_origin() noexcept {
        t_origin.reset();
    }

    void set_last_error_trace(TraceCapture capture) noexcept {
        try {
            t_last_error = std::move(capture);
        } catch (...) {
            t_last_error = TraceCapture{};
        }
    }

    void clear_last_error_trace() noexcept {
        // Called on every successful ABI call, so the common case (nothing to clear) must be nearly free.
        if (!t_last_error.empty()) {
            t_last_error = TraceCapture{};
        }
    }

    const TraceCapture &last_error_trace() noexcept {
        return t_last_error;
    }

    // ---- crash handler --------------------------------------------------------------------------------------------------------------

#if defined(STURDY_TRACE_HAVE_POSIX_SIGNALS)

    namespace {

        char g_crash_log_path[512] = {};
        volatile sig_atomic_t g_crash_to_stderr = 1;
        volatile sig_atomic_t g_crash_entered = 0;
        alignas(16) char g_alt_stack[64 * 1024];

        void write_all(int fd, const char *data, usize length) noexcept {
            while (length > 0) {
                const ssize_t written = ::write(fd, data, length);
                if (written <= 0) {
                    return;
                }
                data += written;
                length -= static_cast<usize>(written);
            }
        }

        void write_text(int fd, const char *text) noexcept {
            write_all(fd, text, std::strlen(text));
        }

        void write_unsigned(int fd, u64 value) noexcept {
            char buffer[24];
            usize index = sizeof(buffer);
            do {
                buffer[--index] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            write_all(fd, buffer + index, sizeof(buffer) - index);
        }

        const char *signal_name(int sig) noexcept {
            switch (sig) {
                case SIGSEGV: return "SIGSEGV (invalid memory access)";
                case SIGBUS: return "SIGBUS (bad memory access)";
                case SIGILL: return "SIGILL (illegal instruction)";
                case SIGFPE: return "SIGFPE (arithmetic error)";
                case SIGABRT: return "SIGABRT (abort / failed assertion / uncaught exception)";
                default: return "fatal signal";
            }
        }

        void write_report(int fd, int sig) noexcept {
            write_text(fd, "=== Sturdy engine crash: ");
            write_text(fd, signal_name(sig));
            write_text(fd, " ===\nengine scopes (innermost first):\n");
            const u32 stored = t_scopes.depth < k_max_scopes ? t_scopes.depth : static_cast<u32>(k_max_scopes);
            if (stored == 0) {
                write_text(fd, "  (none)\n");
            }
            for (u32 i = stored; i > 0; --i) {
                write_text(fd, "  in ");
                write_text(fd, t_scopes.names[i - 1] != nullptr ? t_scopes.names[i - 1] : "?");
                write_text(fd, "\n");
            }
            if (t_scopes.depth > k_max_scopes) {
                write_text(fd, "  (+");
                write_unsigned(fd, t_scopes.depth - k_max_scopes);
                write_text(fd, " outer scopes not recorded)\n");
            }
#if defined(STURDY_TRACE_HAVE_EXECINFO)
            write_text(fd, "native stack (module(symbol+offset) [address]):\n");
            void *frames[64];
            const int count = ::backtrace(frames, 64);
            // Drop this function and the handler; the rest starts at the faulting code.
            const int skip = count > 2 ? 2 : 0;
            ::backtrace_symbols_fd(frames + skip, count - skip, fd);
#else
            write_text(fd, "native stack unavailable on this platform\n");
#endif
        }

        void crash_handler(int sig, siginfo_t *, void *) {
            if (g_crash_entered != 0) {
                ::_exit(128 + sig);
            }
            g_crash_entered = 1;
            if (g_crash_to_stderr != 0) {
                write_report(2, sig);
            }
            if (g_crash_log_path[0] != '\0') {
                const int fd = ::open(g_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd >= 0) {
                    write_report(fd, sig);
                    ::close(fd);
                }
            }
            // SA_RESETHAND restored the default action; re-raise so the process still dies with the right status (and core dump).
            ::raise(sig);
        }

    } // namespace

    bool install_crash_trace_handler(const CrashTraceOptions &options) noexcept {
        std::strncpy(g_crash_log_path, options.log_path.c_str(), sizeof(g_crash_log_path) - 1);
        g_crash_log_path[sizeof(g_crash_log_path) - 1] = '\0';
        g_crash_to_stderr = options.write_to_stderr ? 1 : 0;
        g_crash_entered = 0;
#if defined(STURDY_TRACE_HAVE_EXECINFO)
        // The first backtrace() call may allocate while it loads the unwinder; do that now, not inside a crash.
        void *prime[2];
        (void)::backtrace(prime, 2);
#endif
        stack_t alt{};
        alt.ss_sp = g_alt_stack;
        alt.ss_size = sizeof(g_alt_stack);
        alt.ss_flags = 0;
        (void)::sigaltstack(&alt, nullptr);

        struct sigaction action{};
        action.sa_sigaction = &crash_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        bool ok = true;
        for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
            ok = (::sigaction(sig, &action, nullptr) == 0) && ok;
        }
        return ok;
    }

#elif defined(STURDY_PLATFORM_WINDOWS)

    namespace {

        char g_crash_log_path[512] = {};
        bool g_crash_to_stderr = true;

        void windows_write_report(std::FILE *file, DWORD code) {
            std::fprintf(file, "=== Sturdy engine crash: exception 0x%08lx ===\nengine scopes (innermost first):\n", static_cast<unsigned long>(code));
            const u32 stored = t_scopes.depth < k_max_scopes ? t_scopes.depth : static_cast<u32>(k_max_scopes);
            if (stored == 0) {
                std::fputs("  (none)\n", file);
            }
            for (u32 i = stored; i > 0; --i) {
                std::fprintf(file, "  in %s\n", t_scopes.names[i - 1] != nullptr ? t_scopes.names[i - 1] : "?");
            }
            std::fputs("native stack (module+offset):\n", file);
            void *frames[64];
            const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                HMODULE module = nullptr;
                char path[MAX_PATH] = {};
                u64 offset = static_cast<u64>(reinterpret_cast<std::uintptr_t>(frames[i]));
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(frames[i]), &module) &&
                    module != nullptr) {
                    GetModuleFileNameA(module, path, MAX_PATH);
                    offset -= static_cast<u64>(reinterpret_cast<std::uintptr_t>(module));
                }
                std::fprintf(file, "  #%u %s+0x%llx\n", static_cast<unsigned>(i), path[0] != '\0' ? path : "?", static_cast<unsigned long long>(offset));
            }
            std::fflush(file);
        }

        LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
            const DWORD code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
            if (g_crash_to_stderr) {
                windows_write_report(stderr, code);
            }
            if (g_crash_log_path[0] != '\0') {
                std::FILE *file = nullptr;
                if (fopen_s(&file, g_crash_log_path, "ab") == 0 && file != nullptr) {
                    windows_write_report(file, code);
                    std::fclose(file);
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

    } // namespace

    bool install_crash_trace_handler(const CrashTraceOptions &options) noexcept {
        std::strncpy(g_crash_log_path, options.log_path.c_str(), sizeof(g_crash_log_path) - 1);
        g_crash_log_path[sizeof(g_crash_log_path) - 1] = '\0';
        g_crash_to_stderr = options.write_to_stderr;
        SetUnhandledExceptionFilter(&crash_filter);
        return true;
    }

#else

    bool install_crash_trace_handler(const CrashTraceOptions &) noexcept {
        return false;
    }

#endif

} // namespace SFT::Foundation
