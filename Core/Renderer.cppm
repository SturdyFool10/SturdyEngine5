module;
#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Core:Renderer;

import Sturdy.Platform;
import Sturdy.RHI;
import :ShaderDiscovery;

using SFT::Platform::Windowing::Window;
using std::span;
using std::string;
using std::vector;

export namespace SFT::Core {

    // Backend-agnostic description of the GPU a backend is rendering on. Deliberately free of any
    // graphics-API types — every field is a plain string or integer, so the Engine/app layers can
    // display or log it without linking Vulkan/Metal/etc. Populated by EngineBackend::gpu_info()
    // after initialize(); an unqueryable/uninitialized backend returns empty strings.
    struct GpuInfo {
        string name;            // Marketing name, e.g. "AMD Radeon RX 9070".
        string vendor;          // Human-readable vendor, e.g. "AMD" / "NVIDIA" / "Intel".
        string driver_version;  // Decoded driver version, e.g. "32.0.12010" — vendor-encoded raw
                                // bits are already unpacked into this dotted string.
        string api_version;     // Graphics API version the device supports, e.g. "1.4.303".
        string device_type;     // "Discrete" / "Integrated" / "Virtual" / "CPU" / "Other".
        u32 vendor_id = 0;      // Raw PCI vendor ID (not API-specific; handy for exact matching).
        u32 device_id = 0;      // Raw PCI device ID.
    };

    struct RendererCapabilities {
        b8 multithreaded_command_recording = false;
        b8 async_compute = false;
        b8 raytracing = false;
        b8 mesh_shaders = false;
        b8 bindless = false;
        b8 timeline_semaphores = false;
        u32 max_frames_in_flight = 2;
    };

    enum class RuntimeSettingApplyMode : u8 {
        NoChange,
        HotApplied,
        SurfaceRecreated,
        DeviceRecreated,
        BackendRecreated,
        Unsupported,
    };

    struct RuntimeSettingsChangeResult {
        RuntimeSettingApplyMode mode = RuntimeSettingApplyMode::NoChange;
        string message;
    };

    struct PresentationSettings {
        // True prefers non-tearing presentation. Mailbox is the default because it keeps vsync while
        // dropping stale frames, which gives smoother resize/latency than strict FIFO when supported.
        b8 vsync = true;
        RHI::PresentMode present_mode = RHI::PresentMode::Mailbox;
        // Requests an HDR-capable presentation path. Backends should rebuild the swapchain/device as needed
        // and report Unsupported only when the OS/display/API genuinely cannot expose HDR.
        b8 hdr_enabled = false;
        // 0 = renderer/backend chooses. Non-zero is clamped by the backend/surface capabilities.
        u32 swapchain_image_count = 0;
    };

    // The engine asks for what it wants; the backend grants what it can and reports truth via
    // RendererCapabilities and RHI's FeatureNegotiationReport. Requesting raytracing does not
    // guarantee it unless the feature is also placed in `required_rhi_features`.
    struct RendererFeatureRequest {
        b8 raytracing = false;
        b8 prefer_async_compute = false;
        RHI::FeatureSet required_rhi_features{};
        RHI::FeatureSet optional_rhi_features{};
        u32 desired_frames_in_flight = 2;
        PresentationSettings presentation{};
        // Opt-in escape hatch (Vulkan backend: VulkanNativeAccessExtension, see Core/Vulkan/Rhi/) —
        // exposes the raw VkInstance/VkPhysicalDevice/VkDevice/VkQueue/VkCommandBuffer for callers who
        // need to interoperate with vendor SDKs (FSR2/DLSS/XeSS) or Vulkan capabilities RHI hasn't
        // modeled yet. Off by default: using the returned handles bypasses RHI's tracking guarantees
        // for whatever the caller does with them, so it must be requested explicitly, never implied.
        b8 enable_native_access_extension = false;
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
        // Every shader discovered + reflected before the backend came up. The backend owns turning
        // these into its native format (Vulkan: SPIR-V modules per entry point) during initialize().
        // Non-owning: the backing storage (the engine's shader list) must outlive initialize().
        span<const Slang::UnCompiledShader> uncompiled_shaders;
    };

    // Per-frame payload from the engine to the backend. Timing and drawable resolution live here so
    // high-level cameras/post effects can build projection and screen-space constants from the same
    // framebuffer extent the backend will render into.
    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
        u32 framebuffer_width = 0;
        u32 framebuffer_height = 0;
    };

} // namespace SFT::Core
