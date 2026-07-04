#pragma region Imports
import Sturdy.Engine;
import Sturdy.Foundation;
import Sturdy.Runtime.Cli;
#pragma endregion

using SFT::Engine::Application;
using SFT::Runtime::CliArgs;

namespace {
    // Shared body for both entry points below — take the already-parsed arguments, then construct,
    // initialize, and run the application. Each entry point is responsible only for turning its
    // platform-specific input into a CliArgs (see Sturdy.Runtime.Cli).
    #ifndef SFT_CUSTOM_MAIN
    i32 sturdy_run(const CliArgs &args) {
        //SFT::Foundation::log_info("Sturdy Engine 5 starting with {} argument(s).", args.size());
        //for (CliArgs::size_type i = 0; i < args.size(); ++i) {
        //    SFT::Foundation::log_debug("  arg[{}] = \"{}\"", i, args[i]);
        //}
        Application app;
        if (app.initialize()) {
            app.run();
        }
        return 0;
    }
    #endif
} // namespace

// The Windows GUI entry point (WinMain) is used when building a shipping binary — a Dist build, or
// any build that sets SFT_USE_WINMAIN — so the process launches without a console window. Every
// other build (Debug / RelWithDebInfo / Release, and all non-Windows platforms) keeps the standard
// console main(). Runtime/CMakeLists.txt links the WINDOWS subsystem under the same condition.
#if defined(STURDY_PLATFORM_WINDOWS) && (defined(DIST) || defined(SFT_USE_WINMAIN))

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

i32 WINAPI WinMain(HINSTANCE /*instance*/, HINSTANCE /*prev_instance*/, LPSTR /*cmd_line*/, int /*show_cmd*/) {
    // WinMain's lpCmdLine is a raw, unparsed string, so pull and tokenize the full command line
    // ourselves rather than trusting the (ANSI, program-name-less) argument handed in.
    return sturdy_run(SFT::Runtime::args_from_windows_command_line());
}

#else

i32 main(int argc, char **argv) {
    return sturdy_run(SFT::Runtime::args_from_argv(argc, argv));
}

#endif
