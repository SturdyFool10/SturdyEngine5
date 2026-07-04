module;

#pragma region Imports
#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Runtime.Cli;

using std::size_t;
using std::string;
using std::vector;

export namespace SFT::Runtime {

    // The engine's command-line arguments as a platform-neutral list of UTF-8 strings. Element 0 is
    // the program name (mirroring the classic argv layout), so args[1..] are the user-supplied
    // arguments. Every entry point converts its platform-specific input into one of these before
    // handing control to the shared run routine.
    using CliArgs = vector<string>;

    // Build the argument list from a C-style argv, exactly as delivered to main(argc, argv). On
    // POSIX — and for the Windows console entry point — the shell/CRT has already tokenized and
    // unquoted the command line, so this is a faithful copy with no further parsing required.
    [[nodiscard]] inline CliArgs args_from_argv(int argc, char **argv) {
        if (argc <= 0 || argv == nullptr)
            return {};
        return std::span(argv, static_cast<size_t>(argc))
             | std::views::transform([](const char *arg) { return string(arg != nullptr ? arg : ""); })
             | std::ranges::to<CliArgs>();
    }

#if defined(STURDY_PLATFORM_WINDOWS)
    // Build the argument list for the Windows GUI (WinMain) entry point, which only receives a raw,
    // unparsed command-line string. Declared here but *defined* in the implementation unit
    // (CliImpl.cppm) on purpose: the implementation needs <windows.h>, and keeping that out of this
    // interface's global module fragment stops it leaking into importers — otherwise a translation
    // unit that both imports this module and includes <windows.h> itself (like the WinMain entry
    // point in main.cpp) hits redeclaration conflicts. The returned strings are UTF-8; element 0 is
    // the executable path.
    [[nodiscard]] CliArgs args_from_windows_command_line();
#endif

} // namespace SFT::Runtime
