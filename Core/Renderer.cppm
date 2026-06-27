module;

export module Sturdy.Core:Renderer;

import Sturdy.Foundation;
import :RenderSurface;

export namespace SFT::Core {

    struct RendererCapabilities {
        b8 multithreaded_command_recording = false;
        b8 async_compute = false;
        b8 raytracing = false;
        b8 mesh_shaders = false;
        b8 bindless = false;
        b8 timeline_semaphores = false;
        u32 max_frames_in_flight = 2;
    };

    // The engine asks for what it wants; the backend grants what it can and reports truth via
    // RendererCapabilities. Requesting raytracing does not guarantee it.
    struct RendererFeatureRequest {
        b8 raytracing = false;
        b8 prefer_async_compute = false;
        u32 desired_frames_in_flight = 2;
    };

    struct RendererCreateInfo {
        RendererFeatureRequest features{};
        const char *app_name = "SturdyEngine";
        // Required at instance-creation time (Vulkan needs WSI extension names before any surface
        // handle exists).
        SurfaceProvider initial_surface_provider = SurfaceProvider::Unknown;
        SurfaceSystem initial_surface_system = SurfaceSystem::Unknown;
    };

    struct RenderSurfaceCreateInfo {
        RenderSurfaceDescriptor descriptor{};
        Extent2D framebuffer_extent{};
        u32 desired_frames_in_flight = 2;
    };

    // Per-frame payload from the engine to the backend. Grows into camera/scene/render-list
    // as the renderer matures; the backend owns everything downstream.
    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
    };

} // namespace SFT::Core
