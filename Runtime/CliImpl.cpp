module;

#include <cstddef>
#include <string>
#include <utility>

#if defined(STURDY_PLATFORM_WINDOWS)
#include <cwchar>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#endif

// Module implementation unit for Sturdy.Runtime.Cli. Kept as a .cpp (a plain implementation unit,
// "module X;" with no export) so it is compiled as a normal translation unit — the .cppm extension
// is mapped to a module *interface* by the build and would reject this. Keeping <windows.h> here,
// out of the interface's global module fragment, prevents it leaking into importers such as main.cpp.
module Sturdy.Runtime.Cli;

#if defined(STURDY_PLATFORM_WINDOWS)

namespace SFT::Runtime {

    CliArgs args_from_windows_command_line() {
        CliArgs args;

        // WinMain only hands us a raw command line, so we must tokenize it — honoring Windows'
        // quoting rules (double-quoted spans keep their spaces, "" is an escaped quote, backslashes
        // are only special immediately before a quote, ...). Rather than re-implement those subtle,
        // easy-to-get-wrong rules, defer to CommandLineToArgvW on the full process command line,
        // which applies the exact same parsing the CRT uses to populate argv.
        int wide_argc = 0;
        LPWSTR *wide_argv = ::CommandLineToArgvW(::GetCommandLineW(), &wide_argc);
        if (wide_argv == nullptr) {
            return args;
        }

        args.reserve(static_cast<std::size_t>(wide_argc));
        for (int i = 0; i < wide_argc; ++i) {
            const int wide_len = static_cast<int>(std::wcslen(wide_argv[i]));
            if (wide_len == 0) {
                args.emplace_back();
                continue;
            }

            // Two-call WideCharToMultiByte: first to size the UTF-8 buffer, then to fill it. Passing
            // the exact length (not -1) keeps the terminating null out of the produced string.
            const int utf8_len = ::WideCharToMultiByte(
                CP_UTF8, 0, wide_argv[i], wide_len, nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
            ::WideCharToMultiByte(
                CP_UTF8, 0, wide_argv[i], wide_len, utf8.data(), utf8_len, nullptr, nullptr);
            args.push_back(std::move(utf8));
        }

        ::LocalFree(wide_argv);
        return args;
    }

} // namespace SFT::Runtime

#endif // STURDY_PLATFORM_WINDOWS
