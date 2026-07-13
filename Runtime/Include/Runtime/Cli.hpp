#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SFT::Runtime {

    using CliArgs = std::vector<std::string>;

    [[nodiscard]] inline CliArgs args_from_argv(int argc, char **argv) {
        CliArgs args;
        if (argc <= 0 || argv == nullptr) {
            return args;
        }

        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
        return args;
    }

#if defined(STURDY_PLATFORM_WINDOWS)
    [[nodiscard]] CliArgs args_from_windows_command_line();
#endif

} // namespace SFT::Runtime
