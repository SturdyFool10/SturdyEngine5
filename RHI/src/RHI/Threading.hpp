#pragma once
#include <Foundation/Foundation.hpp>

namespace SFT::RHI {


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

        SingleThreaded,

        DedicatedRenderThread,

        ParallelCommandRecording,
    };

    struct RenderThreadingCapabilities {
        bool backend_allows_dedicated_render_thread = false;
        bool backend_allows_parallel_command_recording = false;
        // Whether `RhiDevice::present` may run on a thread of the renderer's choosing while the
        // render thread carries on into the next frame.
        //
        // False for a backend whose presentation shares mutable state with ordinary submission --
        // WebGPU's single queue is the case in point, where presenting records and submits through
        // the same queue the render thread is still using. Presentation then happens inline on the
        // render thread instead, which costs the overlap a dedicated present thread would buy and
        // is the only correct option when the two cannot run at once.
        bool backend_allows_async_presentation = true;
        bool platform_allows_threads = compile_time_rhi_multithreading_allowed;
        bool requires_graphics_calls_on_owner_thread = true;
        RenderThreadingMode recommended_mode = RenderThreadingMode::SingleThreaded;
    };

    /// Selects render threading mode that best satisfies the supplied requirements.
    ///
    /// @param caps `caps` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
