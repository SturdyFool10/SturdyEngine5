#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include "Slang/ShaderDiscovery.hpp"

using SFT::Platform::Windowing::Window;
using std::span;
using std::string;
using std::vector;

namespace SFT::Core {

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

    // Engine-facing presentation intent — what an app/settings menu actually expresses, kept
    // entirely free of Vulkan present-mode enums (see RHI::PresentStrategy/RHI::PresentMode,
    // RHI/Swapchain.hpp) so gameplay code and ordinary player settings never have to know what a
    // "Fifo" or "Mailbox" even is. resolve_present_strategy() below is the one place these four
    // fields get interpreted together into the backend-agnostic RHI::PresentStrategy the RHI layer
    // actually resolves against real surface support.
    enum class VSyncMode : u8 {
        Off,      // Uncapped framerate, lowest latency; tearing is acceptable.
        On,       // Never tear; synchronize to the display refresh.
        Adaptive, // Tear-free while frames arrive on time; tear rather than stall an extra refresh
                  // interval when a frame is late (the classic driver "Adaptive Sync" toggle).
                  // Distinct from VariableRefreshMode below — see its own doc comment.
    };

    enum class VariableRefreshMode : u8 {
        Disabled,
        // Use VRR when the display stack is capable, without the app doing anything special beyond
        // requesting it — a VRR-capable display/compositor engages it on its own; if it doesn't
        // engage at all, presentation still behaves like ordinary synchronized Fifo (see
        // RHI::PresentStrategy::VariableRefresh's own doc comment: VRR is a display/compositor
        // pacing behavior, not a distinct Vulkan present mode).
        Automatic,
        // Same as Automatic today — no separate Vulkan-level lever exists to prefer VRR more
        // aggressively. Kept as its own value so a future platform-specific VRR hint (display mode
        // selection, refresh-range negotiation) has somewhere to plug in without another public API
        // change.
        Preferred,
    };

    enum class LatencyMode : u8 {
        Normal,
        Low,
        Ultra,
    };

    enum class PresentationPreference : u8 {
        Automatic,
        LowestLatency,
        Smoothest,
        PowerEfficient,
    };

    struct PresentationSettings {
        VSyncMode vsync = VSyncMode::On;
        // Defaults to Disabled, not Automatic: resolve_present_strategy() below gives variable
        // refresh priority over vsync/latency/preference entirely (it's a different axis), so
        // defaulting this to anything other than Disabled would silently ignore whatever the app
        // set vsync/latency to. An app opts into VRR-priority behavior explicitly.
        VariableRefreshMode variable_refresh = VariableRefreshMode::Disabled;
        LatencyMode latency = LatencyMode::Normal;
        PresentationPreference preference = PresentationPreference::Automatic;
        // Requests an HDR-capable presentation path. Backends should rebuild the swapchain/device as needed
        // and report Unsupported only when the OS/display/API genuinely cannot expose HDR.
        b8 hdr_enabled = false;
        // 0 = renderer/backend chooses. Non-zero is clamped by the backend/surface capabilities.
        u32 swapchain_image_count = 0;
        // Opt-out, not opt-in: when the device has a compute queue whose family also supports
        // presenting on the surface (checked fresh per swapchain, never assumed), the backend uses it
        // for vkQueuePresentKHR instead of the graphics queue — frees the graphics queue from present's
        // per-frame driver overhead. On hardware/drivers where the compute queue's family doesn't
        // report present support (essentially all of them today — present support is conventionally
        // tied to the graphics/universal family), this has no effect and presentation stays on the
        // graphics queue exactly as before. Set false to force the graphics queue regardless.
        b8 allow_present_from_compute = true;
    };

    // Turns an app's presentation intent into the backend-agnostic RHI::PresentStrategy the RHI
    // layer resolves against real surface support (RHI::choose_present_mode(), RHI/Swapchain.hpp).
    // Variable refresh takes priority over vsync/latency/preference entirely — it's a different
    // axis (display pacing, not present-mode choice), so a VRR request short-circuits the rest.
    [[nodiscard]] inline RHI::PresentStrategy resolve_present_strategy(const PresentationSettings &settings) noexcept {
        if (settings.variable_refresh != VariableRefreshMode::Disabled) {
            return RHI::PresentStrategy::VariableRefresh;
        }
        switch (settings.vsync) {
            case VSyncMode::Off:
                return RHI::PresentStrategy::Unsynchronized;
            case VSyncMode::Adaptive:
                return RHI::PresentStrategy::AdaptiveTearing;
            case VSyncMode::On: {
                // RHI::PresentStrategy::TearFreeLatestReady (the present-timing-refined variant of
                // this same low-latency intent) isn't reachable from here yet: it needs real
                // presentation-timing support (RHI::Feature::PresentTiming/PresentId/PresentWait
                // are named capabilities but nothing queries/enables them anywhere yet) to be worth
                // distinguishing from TearFreeLatest — until then TearFreeLatest's own preference
                // list already tries FifoLatestReady before Mailbox (RHI/Swapchain.hpp), so the
                // "is latest-ready actually supported" question is still answered, just without the
                // extra timing refinement.
                const bool wants_low_latency = settings.latency != LatencyMode::Normal ||
                    settings.preference == PresentationPreference::LowestLatency;
                return wants_low_latency ? RHI::PresentStrategy::TearFreeLatest : RHI::PresentStrategy::TearFreeOrdered;
            }
        }
        return RHI::PresentStrategy::TearFreeOrdered;
    }

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
