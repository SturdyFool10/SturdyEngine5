#include <ApplicationHost/src/EntryPoint.hpp>


namespace SFT::ApplicationHost {

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

