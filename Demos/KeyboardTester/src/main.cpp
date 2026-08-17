#include <Foundation/src/Foundation.hpp>

#include <Runtime/Runtime.hpp>
#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

using SFT::Foundation::CliArgs;

namespace {

    /// Returns the current or globally available keyboard tester config value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SFT::Runtime::RuntimeConfig keyboard_tester_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 Keyboard Tester";
        config.application.primary_window.extent = {1280, 720};
        config.application.primary_window.graphics_api =
            SFT::Platform::Windowing::WindowGraphicsApi::Vulkan;
        config.application.engine.app_name = "Sturdy Engine 5 Keyboard Tester";
        config.application.engine.shaders_directory = "Shaders";

        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Adaptive;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 Keyboard Tester"};
        return config;
    }

#ifndef SFT_CUSTOM_MAIN
    /// Runs the Sturdy application entry point using the supplied command-line arguments.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the process/application exit status; zero conventionally indicates successful completion.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    i32 sturdy_run(const CliArgs &args) {
        return SFT::Runtime::run(
            args,
            keyboard_tester_config(),
            &SFT::KeyboardTester::create_keyboard_tester_game_logic);
    }
#else
    /// Runs the Sturdy application entry point using the supplied command-line arguments.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the process/application exit status; zero conventionally indicates successful completion.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    i32 sturdy_run(const CliArgs &args);
#endif

} // namespace


#if defined(STURDY_PLATFORM_WINDOWS) && (defined(DIST) || defined(SFT_USE_WINMAIN))

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
i32 WINAPI WinMain(HINSTANCE             , HINSTANCE                  , LPSTR             , int             ) {
    return sturdy_run(SFT::Foundation::args_from_windows_command_line());
}

#else

/// Runs the executable entry point and returns its process exit status.
///
/// @param argc `argc` value used by the operation.
/// @param argv `argv` value used by the operation.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
i32 main(int argc, char **argv) {
    return sturdy_run(SFT::Foundation::args_from_argv(argc, argv));
}

#endif
