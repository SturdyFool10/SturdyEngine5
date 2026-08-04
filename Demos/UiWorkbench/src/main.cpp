#include <Foundation/src/Foundation.hpp>

#include <Runtime/Runtime.hpp>
#include <UiWorkbenchGameLogic/UiWorkbenchGameLogic.hpp>

#include "CrashHandler.hpp"

using SFT::Foundation::CliArgs;

namespace {

    [[nodiscard]] SFT::Runtime::RuntimeConfig ui_workbench_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 UI Workbench";
        config.application.primary_window.extent = {1440, 900};
        config.application.primary_window.graphics_api =
            SFT::Platform::Windowing::WindowGraphicsApi::Vulkan;
        config.application.enable_runtime_window_management = true;
        config.application.engine.app_name = "Sturdy Engine 5 UI Workbench";
        config.application.engine.shaders_directory = "Shaders";
        config.application.engine.features.presentation.hdr_enabled = false;
        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Off;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 UI Workbench"};
        return config;
    }

#ifndef SFT_CUSTOM_MAIN
    i32 sturdy_run(const CliArgs &args) {
        SFT::UiWorkbench::install_crash_handler();
        SFT::Foundation::init_file_logging("Logs/UiWorkbench.log");
        return SFT::Runtime::run(
            args,
            ui_workbench_config(),
            &SFT::UiWorkbench::create_ui_workbench_game_logic);
    }
#else
    i32 sturdy_run(const CliArgs &args);
#endif

} // namespace

#if defined(STURDY_PLATFORM_WINDOWS) && (defined(DIST) || defined(SFT_USE_WINMAIN))

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

i32 WINAPI WinMain(HINSTANCE /*instance*/, HINSTANCE /*prev_instance*/, LPSTR /*cmd_line*/, int /*show_cmd*/) {
    return sturdy_run(SFT::Foundation::args_from_windows_command_line());
}

#else

i32 main(int argc, char **argv) {
    return sturdy_run(SFT::Foundation::args_from_argv(argc, argv));
}

#endif
