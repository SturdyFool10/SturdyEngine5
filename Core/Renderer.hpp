#pragma once

#include "Core/RenderSurface.hpp"
#include "Foundation/Types.hpp"

#include <expected>
#include <string>
#include <utility>

using std::expected;
using std::string;
using std::unexpected;

namespace SFT::Core {

    enum class RendererErrorCode {
        InitializationFailed,
        DeviceLost,
        SurfaceLost,
        OutOfMemory,
        Unsupported,
        OperationFailed,
    };

    // Mirrors the Platform WindowError convention so error handling looks the same across layers.
    struct RendererError {
        RendererErrorCode code = RendererErrorCode::OperationFailed;
        string message;
    };

    using RendererResult = expected<void, RendererError>;

    template <typename Value>
    using RendererExpected = expected<Value, RendererError>;

    [[nodiscard]] inline unexpected<RendererError> renderer_error(RendererErrorCode code, string message) {
        return unexpected(RendererError{code, std::move(message)});
    }

    // What a backend can actually do. The backend fills this in at initialize() time and the
    // engine/game queries it to decide behavior. A future WebGPU backend reports
    // multithreaded_command_recording = false; Vulkan reports true. This is the seam that lets
    // each renderer "take advantage of what it can" without the upper layers hard-coding API
    // assumptions.
    struct RendererCapabilities {
        b8 multithreaded_command_recording = false;
        b8 async_compute = false;
        b8 raytracing = false;
        b8 mesh_shaders = false;
        b8 bindless = false;
        b8 timeline_semaphores = false;
        u32 max_frames_in_flight = 2;
    };

    // What the engine asks for globally. The backend grants what it can and reports the truth via
    // RendererCapabilities (requesting raytracing does not guarantee it). desired_frames_in_flight
    // is also used as the default for new surfaces unless a surface overrides it.
    struct RendererFeatureRequest {
        b8 raytracing = false;
        b8 prefer_async_compute = false;
        u32 desired_frames_in_flight = 2;
    };

    struct RendererCreateInfo {
        RendererFeatureRequest features{};
        const char *app_name = "SturdyEngine";
        SurfaceProvider initial_surface_provider = SurfaceProvider::Unknown;
        SurfaceSystem initial_surface_system = SurfaceSystem::Unknown;
    };

    struct RenderSurfaceCreateInfo {
        RenderSurfaceDescriptor descriptor{};
        Extent2D framebuffer_extent{};
        u32 desired_frames_in_flight = 2;
    };

    // Per-frame input handed from the glue to the backend. Grows into camera/scene/render-list
    // as the renderer matures; the backend owns everything downstream (threading, passes).
    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
    };

} // namespace SFT::Core
