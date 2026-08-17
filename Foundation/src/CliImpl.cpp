#include <Foundation/src/Cli.hpp>

#include <cstddef>
#include <string>
#include <utility>

#if defined(STURDY_PLATFORM_WINDOWS)
#include <cwchar>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#if defined(STURDY_PLATFORM_WINDOWS)

namespace SFT::Foundation {

    /// Returns the current or globally available args from windows command line value.
    ///
    /// @return Returns the current args from windows command line value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    CliArgs args_from_windows_command_line() {
        CliArgs args;


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

} // namespace SFT::Foundation

#endif // STURDY_PLATFORM_WINDOWS
