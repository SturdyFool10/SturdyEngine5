#include <Foundation/src/Cli.hpp>


namespace SFT::Foundation {

    /// Performs the args from argv operation for `Foundation` using the supplied arguments.
    ///
    /// @param argc `argc` value used by the operation.
    /// @param argv `argv` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    CliArgs args_from_argv(int argc, char **argv) {
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

} // namespace SFT::Foundation

