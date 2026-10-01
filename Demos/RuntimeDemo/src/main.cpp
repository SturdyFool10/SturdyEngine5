#include <Foundation/Foundation.hpp>

#include <cstdlib>
#include <string_view>

#include <Runtime/Runtime.hpp>
#include <RuntimeDemoGameLogic/RuntimeDemoGameLogic.hpp>

using SFT::Foundation::CliArgs;

namespace {

    /// Runs demo config.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SFT::Runtime::RuntimeConfig runtime_demo_config() {
        SFT::Runtime::RuntimeConfig config{};
        config.application.primary_window.title = "Sturdy Engine 5 Runtime Demo";
        config.application.primary_window.extent = {1280, 720};
#if defined(STURDY_PLATFORM_WEB)
        // WebGPU (reached through the browser's own canvas, not a native surface) is Web's only
        // graphics API -- Core/Vulkan is excluded from the Web build entirely (see
        // Core/CMakeLists.txt and EngineBackend.cpp's create_engine_backend()), and Emscripten's
        // SDL3 video driver has no "vulkan" driver to satisfy SDL_WINDOW_VULKAN in the first place.
        config.application.primary_window.graphics_api =
            SFT::WindowManager::WindowGraphicsApi::WebGPU;
        config.application.engine.graphics_backend = SFT::RHI::BackendType::WebGpu;
#else
        config.application.primary_window.graphics_api =
            SFT::WindowManager::WindowGraphicsApi::Vulkan;
#endif
        config.application.engine.app_name = "Sturdy Engine 5 Runtime Demo";
        config.application.engine.shaders_directory = "Shaders";
        config.application.engine.features.raytracing = true;
        config.application.engine.features.presentation.vsync = SFT::Core::VSyncMode::Adaptive;
        config.application.engine.features.presentation.variable_refresh = SFT::Core::VariableRefreshMode::Preferred;
        config.application.engine.features.presentation.latency = SFT::Core::LatencyMode::Ultra;
        config.application.engine.features.presentation.preference = SFT::Core::PresentationPreference::LowestLatency;
        config.application.primary_window_title_update_interval_seconds = 0.25;
        config.primary_window_title = UString{"SturdyEngine 5 Runtime Demo"};
#if !defined(STURDY_PLATFORM_WEB)
        // Development override, same as the UI workbench's: reproduce a backend's behaviour from the first frame.
        // Unset in normal use.
        if (const char *requested = std::getenv("STURDY_GRAPHICS_BACKEND"); requested != nullptr) {
            const std::string_view name{requested};
            if (name == "webgpu") {
                config.application.engine.graphics_backend = SFT::RHI::BackendType::WebGpu;
            } else if (name == "vulkan") {
                config.application.engine.graphics_backend = SFT::RHI::BackendType::Vulkan;
            } else if (name == "d3d12") {
                config.application.engine.graphics_backend = SFT::RHI::BackendType::D3D12;
            } else {
                SFT::Foundation::log_warn(
                    "STURDY_GRAPHICS_BACKEND='{}' names no known backend; expected one of vulkan, webgpu, d3d12. "
                    "Using the default instead.",
                    name);
            }
        }
#endif
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
            runtime_demo_config(),
            &SFT::Runtime::create_runtime_demo_game_logic);
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
