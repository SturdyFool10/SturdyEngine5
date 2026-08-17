#include <Foundation/src/Cli.hpp>


namespace SFT::Foundation {

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

