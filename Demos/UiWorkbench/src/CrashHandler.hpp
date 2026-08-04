#pragma once

namespace SFT::UiWorkbench {

    // Installs process-wide native-crash diagnostics: on Windows, a SetUnhandledExceptionFilter that
    // logs a std::stacktrace and writes a minidump; on Linux/macOS/FreeBSD, a POSIX fatal-signal handler
    // (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS) that logs a backtrace before falling through to the
    // platform's normal core-dump/crash-report behavior. A no-op on platforms with no native crash
    // surface (Web). Meant to catch hard crashes (e.g. a fault raised from inside a GPU driver module)
    // that a C++ handler can't observe. Call once, before the engine starts.
    void install_crash_handler();

} // namespace SFT::UiWorkbench
