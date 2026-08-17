#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SFT::Foundation {

    using CliArgs = std::vector<std::string>;

    [[nodiscard]] CliArgs args_from_argv(int argc, char **argv);

#if defined(STURDY_PLATFORM_WINDOWS)
    [[nodiscard]] CliArgs args_from_windows_command_line();
#endif

} // namespace SFT::Foundation
