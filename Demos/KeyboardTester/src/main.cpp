#include <Foundation/src/Foundation.hpp>

#include <Runtime/Runtime.hpp>
#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

using SFT::Foundation::CliArgs;

namespace {

    [[nodiscard]] SFT::Runtime::RuntimeConfig keyboard_tester_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 Keyboard Tester";
        config.application.primary_window.extent = {1280, 720};
        config.application.primary_window.graphics_api =
            SFT::Platform::Windowing::WindowGraphicsApi::Vulkan;
        config.application.engine.app_name = "Sturdy Engine 5 Keyboard Tester";
        config.application.engine.shaders_directory = "Shaders";
        // Flat-shaded quads only — no raytracing feature requirements, unlike RuntimeDemo.
        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Adaptive;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 Keyboard Tester"};
        return config;
    }

#ifndef SFT_CUSTOM_MAIN
    i32 sturdy_run(const CliArgs &args) {
        return SFT::Runtime::run(
            args,
            keyboard_tester_config(),
            &SFT::KeyboardTester::create_keyboard_tester_game_logic);
    }
#else
    i32 sturdy_run(const CliArgs &args);
#endif

} // namespace

// Each delivered product owns its actual OS entrypoint. Runtime is only the reusable host library;
// this demo executable chooses its GameLogic factory and startup policy explicitly above.
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
