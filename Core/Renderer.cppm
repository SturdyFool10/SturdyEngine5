module;

#include <vector>

export module Sturdy.Core:Renderer;

import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Platform::Windowing::Window;
using std::vector;

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
        // Non-owning pointer to the primary window the backend presents into. Must outlive the
        // renderer backend (owned by the application/engine layer). The backend uses it to create,
        // own, resize, and destroy its surfaces internally.
        Window *window = nullptr;
        // WSI instance extension strings from the windowing backend (e.g. VK_KHR_surface +
        // VK_KHR_xlib_surface). Pointers must stay valid for the duration of initialize().
        // SDL3 and GLFW return pointers into their own static storage, so this is safe.
        vector<const char *> wsi_extensions;
    };

    // Per-frame payload from the engine to the backend. Grows into camera/scene/render-list
    // as the renderer matures; the backend owns everything downstream.
    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
    };

} // namespace SFT::Core
