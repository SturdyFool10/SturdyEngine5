#include <Foundation/src/Foundation.hpp>

#include <Runtime/Runtime.hpp>
#include <UiWorkbenchGameLogic/UiWorkbenchGameLogic.hpp>

#include "CrashHandler.hpp"

using SFT::Foundation::CliArgs;

namespace {

    /// Returns the current or globally available UI workbench config value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SFT::Runtime::RuntimeConfig ui_workbench_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 UI Workbench";
        config.application.primary_window.extent = {1440, 900};


        config.application.primary_window.transparent = true;
        config.application.primary_window.graphics_api =
            SFT::Platform::Windowing::WindowGraphicsApi::Vulkan;
        config.application.enable_runtime_window_management = true;
        config.application.engine.app_name = "Sturdy Engine 5 UI Workbench";
        config.application.engine.shaders_directory = "Shaders";
        config.application.engine.features.presentation.hdr_enabled = false;


        config.application.engine.features.presentation.hdr_color_space =
            SFT::Core::HdrColorSpaceMode::ScrgbLinear;
        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Off;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 UI Workbench"};
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
        SFT::UiWorkbench::install_crash_handler();
        SFT::Foundation::init_file_logging("Logs/UiWorkbench.log");
        return SFT::Runtime::run(
            args,
            ui_workbench_config(),
            &SFT::UiWorkbench::create_ui_workbench_game_logic);
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
