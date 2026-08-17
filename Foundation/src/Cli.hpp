#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SFT::Foundation {

    using CliArgs = std::vector<std::string>;

    /// Performs the args from argv operation using the supplied arguments.
    ///
    /// @param argc `argc` value used by the operation.
    /// @param argv `argv` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] CliArgs args_from_argv(int argc, char **argv);

#if defined(STURDY_PLATFORM_WINDOWS)
    /// Returns the current or globally available args from windows command line value.
    ///
    /// @return Returns the current args from windows command line value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] CliArgs args_from_windows_command_line();
#endif

} // namespace SFT::Foundation
