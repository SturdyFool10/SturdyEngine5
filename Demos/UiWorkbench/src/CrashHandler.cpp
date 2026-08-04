#include "CrashHandler.hpp"

#if defined(STURDY_PLATFORM_WINDOWS)

#include <Foundation/src/Log.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <filesystem>
#include <format>
#include <stacktrace>
#include <string>
#include <system_error>

namespace SFT::UiWorkbench {

    namespace {

        [[nodiscard]] std::filesystem::path crash_dump_path() {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            const auto file_name = std::format(
                "crash-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp",
                time.wYear,
                time.wMonth,
                time.wDay,
                time.wHour,
                time.wMinute,
                time.wSecond);
            return std::filesystem::path("Logs") / file_name;
        }

        // Writes a minidump alongside the log file. std::stacktrace's symbol names are enough to spot
        // an obviously-engine-side bug, but a crash inside a GPU driver module (e.g. nvoglv64.dll,
        // nvwgf2umx.dll) needs a real dump loaded against the driver's own PDBs to say anything useful.
        void write_minidump(EXCEPTION_POINTERS *exception_pointers) {
            const auto path = crash_dump_path();
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            const HANDLE file = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                Foundation::log_error("crash: could not open '{}' for the minidump", path.string());
                return;
            }

            MINIDUMP_EXCEPTION_INFORMATION exception_info{
                .ThreadId = GetCurrentThreadId(),
                .ExceptionPointers = exception_pointers,
                .ClientPointers = FALSE,
            };

            const BOOL wrote = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                file,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory),
                &exception_info,
                nullptr,
                nullptr);
            CloseHandle(file);

            if (wrote) {
                Foundation::log_error("crash: minidump written to '{}'", path.string());
            } else {
                Foundation::log_error(
                    "crash: MiniDumpWriteDump failed for '{}' (GetLastError={})", path.string(), GetLastError());
            }
        }

        void log_stacktrace() {
            const auto trace = std::stacktrace::current();
            Foundation::log_error("crash: stack trace ({} frames):", trace.size());
            for (const auto &frame : trace) {
                if (frame.source_file().empty()) {
                    Foundation::log_error("  {}", frame.description());
                } else {
                    Foundation::log_error(
                        "  {} ({}:{})", frame.description(), frame.source_file(), frame.source_line());
                }
            }
        }

        LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS *exception_pointers) {
            Foundation::log_error(
                "crash: unhandled exception 0x{:08X} at address {}",
                static_cast<unsigned long>(exception_pointers->ExceptionRecord->ExceptionCode),
                static_cast<void *>(exception_pointers->ExceptionRecord->ExceptionAddress));
            log_stacktrace();
            write_minidump(exception_pointers);
            Foundation::flush_logs();
            return EXCEPTION_CONTINUE_SEARCH;
        }

    } // namespace

    void install_crash_handler() { SetUnhandledExceptionFilter(&on_unhandled_exception); }

} // namespace SFT::UiWorkbench

#elif defined(STURDY_PLATFORM_LINUX) || defined(STURDY_PLATFORM_FREEBSD) || defined(STURDY_PLATFORM_MACOS)

#include <Foundation/src/Log.hpp>

#include <cstddef>
#include <execinfo.h>
#include <signal.h>

namespace SFT::UiWorkbench {

    namespace {

        // A fixed size rather than SIGSTKSZ: glibc >= 2.34 defines SIGSTKSZ via sysconf() and it is no
        // longer a compile-time constant there, so it can't size a static array on every target libc.
        constexpr std::size_t kSignalStackSize = 131072;
        alignas(16) std::byte g_signal_stack[kSignalStackSize];

        [[nodiscard]] const char *signal_name(int signal_number) {
            switch (signal_number) {
                case SIGSEGV: return "SIGSEGV";
                case SIGABRT: return "SIGABRT";
                case SIGFPE: return "SIGFPE";
                case SIGILL: return "SIGILL";
                case SIGBUS: return "SIGBUS";
                default: return "unknown signal";
            }
        }

        // backtrace()/backtrace_symbols() aren't guaranteed async-signal-safe by POSIX, same as calling
        // the logger itself here — accepted deliberately, same tradeoff the Windows exception filter
        // makes by logging from inside the fault handler. The process is already on its way down either
        // way; best-effort diagnostics beat none.
        void log_backtrace() {
            void *frames[64];
            const int frame_count = ::backtrace(frames, 64);
            char **symbols = ::backtrace_symbols(frames, frame_count);
            Foundation::log_error("crash: stack trace ({} frames):", frame_count);
            for (int i = 0; i < frame_count; ++i) {
                Foundation::log_error("  {}", symbols != nullptr ? symbols[i] : "<unresolved>");
            }
            // Deliberately not freeing `symbols`: the allocator may itself be the thing that faulted,
            // and the process terminates right after this regardless.
        }

        void on_fatal_signal(int signal_number, siginfo_t *info, void * /*context*/) {
            if (info != nullptr && (signal_number == SIGSEGV || signal_number == SIGBUS)) {
                Foundation::log_error(
                    "crash: unhandled {} at address {}", signal_name(signal_number), info->si_addr);
            } else {
                Foundation::log_error("crash: unhandled {}", signal_name(signal_number));
            }
            log_backtrace();
            Foundation::flush_logs();
            // SA_RESETHANDLER (below) already reset this signal's disposition to default before this
            // handler ran, so simply returning re-raises it against that default — producing the
            // platform's normal core dump / crash report instead of looping back into this handler.
        }

    } // namespace

    void install_crash_handler() {
        stack_t alt_stack{};
        alt_stack.ss_sp = g_signal_stack;
        alt_stack.ss_size = sizeof(g_signal_stack);
        alt_stack.ss_flags = 0;
        if (sigaltstack(&alt_stack, nullptr) != 0) {
            Foundation::log_warn("crash handler: sigaltstack failed; a stack-overflow crash may not be caught");
        }

        struct sigaction action{};
        action.sa_sigaction = &on_fatal_signal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHANDLER;
        sigemptyset(&action.sa_mask);

        for (const int signal_number : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
            if (sigaction(signal_number, &action, nullptr) != 0) {
                Foundation::log_warn(
                    "crash handler: failed to install handler for {}", signal_name(signal_number));
            }
        }
    }

} // namespace SFT::UiWorkbench

#else

// Web (and any future platform without a native crash surface): intentional no-op, the same fallback
// convention Platform/src/Platform/Web/WindowEffectsImpl.cpp uses for OS chrome effects that don't
// exist in a browser sandbox.
namespace SFT::UiWorkbench {

    void install_crash_handler() {}

} // namespace SFT::UiWorkbench

#endif
