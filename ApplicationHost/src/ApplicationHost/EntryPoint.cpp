#include <ApplicationHost/EntryPoint.hpp>


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
#if defined(STURDY_PLATFORM_WEB)
        // On Web, Application::run() registers a persistent requestAnimationFrame-driven main loop
        // callback and returns immediately -- unlike native, where it blocks until the app closes.
        // A stack-allocated `app` would therefore be destroyed (tearing down the WebGPU device and
        // window) the instant this function returns, seconds after startup, rather than staying
        // alive for as long as the browser keeps calling that callback. Heap-allocating and
        // deliberately never freeing it is correct here: there is no equivalent of "process exit"
        // to clean up before on a page the user just navigates away from, and the browser tears
        // down the whole WASM heap/instance itself when that happens -- this is the standard
        // Emscripten idiom for `emscripten_set_main_loop`-driven programs, matching why Emscripten's
        // own EXIT_RUNTIME defaults off rather than running atexit/cleanup when main() returns here.
        auto *app = new Engine::Application(client);
        if (!app->initialize()) {
            return 1;
        }
        app->run();
        return 0;
#else
        Engine::Application app{client};
        if (!app.initialize()) {
            return 1;
        }
        app.run();
        return 0;
#endif
    }

} // namespace SFT::ApplicationHost

