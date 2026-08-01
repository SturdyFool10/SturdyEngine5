#include <Foundation/src/Foundation.hpp>

#include <Runtime/Runtime.hpp>
#include <RuntimeDemoGameLogic/RuntimeDemoGameLogic.hpp>

using SFT::Foundation::CliArgs;

namespace {

    [[nodiscard]] SFT::Runtime::RuntimeConfig runtime_demo_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 Runtime Demo";
        config.application.primary_window.extent = {1280, 720};
        config.application.primary_window.graphics_api =
            SFT::Platform::Windowing::WindowGraphicsApi::Vulkan;
        config.application.engine.app_name = "Sturdy Engine 5 Runtime Demo";
        config.application.engine.shaders_directory = "Shaders";
        config.application.engine.features.raytracing = true;
        config.application.engine.features.required_rhi_features
            .set(SFT::RHI::Feature::RayQuery)
            .set(SFT::RHI::Feature::AccelerationStructures)
            .set(SFT::RHI::Feature::BufferDeviceAddress)
            .set(SFT::RHI::Feature::BindlessResources);
        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Adaptive;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 Runtime Demo"};
        return config;
    }

#ifndef SFT_CUSTOM_MAIN
    i32 sturdy_run(const CliArgs &args) {
        return SFT::Runtime::run(
            args,
            runtime_demo_config(),
            &SFT::Runtime::create_runtime_demo_game_logic);
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
