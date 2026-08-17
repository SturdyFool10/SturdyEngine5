#include <ApplicationHost/src/EntryPoint.hpp>


namespace SFT::ApplicationHost {

    /// Runs the requested work.
    ///
    /// @param client `client` value used by the operation.
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    i32 run(
        Engine::ApplicationClient &client,
        const Foundation::CliArgs &args) {
        (void)args;
        Engine::Application app{client};
        if (!app.initialize()) {
            return 1;
        }
        app.run();
        return 0;
    }

} // namespace SFT::ApplicationHost

