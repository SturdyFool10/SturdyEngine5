#pragma once
#include <Foundation/src/Foundation.hpp>

namespace SFT::RHI {

    // Compile-time policy knobs. Games/ports can define these before building the engine to force the
    // global renderer threading envelope down to a safe subset. Runtime backend selection still gets the
    // final say through `RenderThreadingCapabilities`.
#if defined(STURDY_RHI_FORCE_SINGLE_THREADED)
    inline constexpr bool compile_time_rhi_multithreading_allowed = false;
#elif defined(STURDY_PLATFORM_WEB)
    inline constexpr bool compile_time_rhi_multithreading_allowed = false;
#else
    inline constexpr bool compile_time_rhi_multithreading_allowed = true;
#endif

#if defined(STURDY_RHI_ALLOW_PARALLEL_COMMAND_RECORDING)
    inline constexpr bool compile_time_parallel_command_recording_allowed = compile_time_rhi_multithreading_allowed;
#else
    inline constexpr bool compile_time_parallel_command_recording_allowed = false;
#endif

    enum class RenderThreadingMode : u8 {
        // Everything, including event pumping and graphics calls, runs on the caller/main thread.
        SingleThreaded,
        // A dedicated render owner thread executes all RHI/backend calls. Other threads may prepare CPU data.
        DedicatedRenderThread,
        // Multiple workers may record command work in parallel, subject to backend object ownership rules.
        ParallelCommandRecording,
    };

    struct RenderThreadingCapabilities {
        bool backend_allows_dedicated_render_thread = false;
        bool backend_allows_parallel_command_recording = false;
        bool platform_allows_threads = compile_time_rhi_multithreading_allowed;
        bool requires_graphics_calls_on_owner_thread = true;
        RenderThreadingMode recommended_mode = RenderThreadingMode::SingleThreaded;
    };

    [[nodiscard]] constexpr RenderThreadingMode choose_render_threading_mode(RenderThreadingCapabilities caps) noexcept {
        if (!compile_time_rhi_multithreading_allowed || !caps.platform_allows_threads) {
            return RenderThreadingMode::SingleThreaded;
        }
        if (compile_time_parallel_command_recording_allowed && caps.backend_allows_parallel_command_recording) {
            return RenderThreadingMode::ParallelCommandRecording;
        }
        if (caps.backend_allows_dedicated_render_thread) {
            return RenderThreadingMode::DedicatedRenderThread;
        }
        return RenderThreadingMode::SingleThreaded;
    }

} // namespace SFT::RHI
