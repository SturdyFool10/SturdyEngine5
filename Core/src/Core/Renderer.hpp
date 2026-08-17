#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include "Slang/ShaderDiscovery.hpp"

using SFT::Platform::Windowing::Window;
using std::expected;
using std::span;
using std::string;
using std::vector;

namespace SFT::Core {

    /// Backend-agnostic description of the GPU a backend is rendering on. Deliberately free of any
    /// graphics-API types — every field is a plain string or integer, so the Engine/app layers can
    /// display or log it without linking Vulkan/Metal/etc. Populated by EngineBackend::gpu_info()
    /// after initialize(); an unqueryable/uninitialized backend returns empty strings.
    struct GpuInfo {
        string name;
        string vendor;
        string driver_version;
                                /// bits are already unpacked into this dotted string.
        string api_version;
        string device_type;
        u32 vendor_id = 0;
        u32 device_id = 0;
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

    /// Result of resolve_frames_in_flight() below.
    struct FramesInFlightResolution {
        u32 requested = 0;
        u32 lower_bound = 1;
        u32 upper_bound = 0;
        u32 resolved = 0;
        enum class Adjustment : u8 { Accepted, RaisedToLower, ReducedToUpper };
        Adjustment adjustment = Adjustment::Accepted;
    };

    /// The single place a requested frames-in-flight count is resolved against policy bounds.
    /// RendererCapabilities::max_frames_in_flight is set from this function's result exactly once, at
    /// backend initialization (VulkanBackendDevice.cpp) — every other subsystem that needs the frame-
    /// slot/acquisition-semaphore/command-allocator count must consume that already-resolved value
    /// rather than re-deriving its own "at least 1" interpretation of it.
    ///
    /// `requested == 0` means "no explicit preference" and resolves to `lower_bound` — NOT to
    /// "unbounded". A caller that wants a specific default (e.g. double-buffering) passes it as
    /// `lower_bound`, it isn't inferred from requested==0 alone. `upper_bound == 0` means no known
    /// upper bound (Vulkan's own "maxImageCount == 0 means no explicit maximum" convention, reused
    /// here for the same reason — the real surface-derived upper bound isn't wired in yet).
    ///
    /// Only the bound relationship itself can be invalid (lower_bound > upper_bound, with
    /// upper_bound != 0); that returns an error rather than silently swapping the bounds, ignoring
    /// one of them, or falling back to an unrelated constant. A request merely outside the bounds is
    /// not an error — it's raised/reduced to fit, reported via `FramesInFlightResolution::adjustment`.
    [[nodiscard]] expected<FramesInFlightResolution, string> resolve_frames_in_flight(
        u32 requested, u32 lower_bound, u32 upper_bound) noexcept;

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
        UString message;
    };

    /// Engine-facing presentation intent — what an app/settings menu actually expresses, kept
    /// entirely free of Vulkan present-mode enums (see RHI::PresentStrategy/RHI::PresentMode,
    /// RHI/Swapchain.hpp) so gameplay code and ordinary player settings never have to know what a
    /// "Fifo" or "Mailbox" even is. resolve_present_strategy() below is the one place these four
    /// fields get interpreted together into the backend-agnostic RHI::PresentStrategy the RHI layer
    /// actually resolves against real surface support.
    enum class VSyncMode : u8 {
        Off,
        On,
        Adaptive,


    };

    enum class VariableRefreshMode : u8 {
        Disabled,
        /// Use VRR when the display stack is capable, without the app doing anything special beyond
        /// requesting it — a VRR-capable display/compositor engages it on its own; if it doesn't
        /// engage at all, presentation still behaves like ordinary synchronized Fifo (see
        /// RHI::PresentStrategy::VariableRefresh's own doc comment: VRR is a display/compositor
        /// pacing behavior, not a distinct Vulkan present mode).
        Automatic,
        /// Same as Automatic today — no separate Vulkan-level lever exists to prefer VRR more
        /// aggressively. Kept as its own value so a future platform-specific VRR hint (display mode
        /// selection, refresh-range negotiation) has somewhere to plug in without another public API
        /// change.
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

    /// Which HDR swapchain encoding to request when PresentationSettings::hdr_enabled is set. Both
    /// are gated behind VK_EXT_swapchain_colorspace — see RHI::ColorSpace's own doc comment
    /// (RHI/Swapchain.hpp) for the tradeoff: Hdr10St2084 bakes in a fixed peak-nits assumption via
    /// PQ, ScrgbLinear leaves tone mapping to the OS/compositor. Ignored when hdr_enabled is false.
    enum class HdrColorSpaceMode : u8 {
        Hdr10St2084,
        ScrgbLinear,
        /// ITU-R BT.2100 Hybrid Log-Gamma — no HDR metadata needed at all (backward-compatible,
        /// self-describing scene-referred curve). See RHI::ColorSpace::Hdr10Hlg's own doc comment.
        Hdr10Hlg,
        /// Best-effort only: real Dolby Vision needs per-frame dynamic metadata in Dolby's proprietary
        /// format, which this engine cannot produce. Selecting this is expected to report Unsupported
        /// on the overwhelming majority of real displays — see RHI::ColorSpace::DolbyVision's own doc
        /// comment for the full explanation and what the tonemap shader falls back to.
        DolbyVision,
    };

    struct PresentationSettings {
        VSyncMode vsync = VSyncMode::On;
        /// Defaults to Disabled, not Automatic: resolve_present_strategy() below gives variable
        /// refresh priority over vsync/latency/preference entirely (it's a different axis), so
        /// defaulting this to anything other than Disabled would silently ignore whatever the app
        /// set vsync/latency to. An app opts into VRR-priority behavior explicitly.
        VariableRefreshMode variable_refresh = VariableRefreshMode::Disabled;
        LatencyMode latency = LatencyMode::Normal;
        PresentationPreference preference = PresentationPreference::Automatic;
        /// Requests an HDR-capable presentation path. Backends should rebuild the swapchain/device as needed
        /// and report Unsupported only when the OS/display/API genuinely cannot expose HDR.
        b8 hdr_enabled = false;
        HdrColorSpaceMode hdr_color_space = HdrColorSpaceMode::Hdr10St2084;
        /// Requests a compositor-visible alpha channel. The renderer asks the RHI for premultiplied
        /// swapchain composition; platforms that require an alpha-capable native window must also
        /// create that window with WindowConfig::transparent.
        b8 transparent_composition = false;
        /// 0 = renderer/backend chooses. Non-zero is clamped by the backend/surface capabilities.
        u32 swapchain_image_count = 0;
        /// Opt-in, not opt-out: presenting from a compute queue moves present's per-frame driver
        /// overhead off the graphics queue, but it also means that queue's own async-compute
        /// workloads now compete with the presentation engine's own scheduling — a real tradeoff that
        /// needs hardware-specific validation, not something to enable silently by default just
        /// because a device happens to report support. When explicitly set true, and the device has a
        /// compute queue whose family also supports presenting on the surface (checked fresh per
        /// swapchain, never assumed), the backend uses it for vkQueuePresentKHR instead of the
        /// graphics queue. On hardware/drivers where the compute queue's family doesn't report present
        /// support (essentially all of them today — present support is conventionally tied to the
        /// graphics/universal family), this has no effect regardless and presentation stays on the
        /// graphics queue.
        b8 allow_present_from_compute = false;
    };

    /// Turns an app's presentation intent into the backend-agnostic RHI::PresentStrategy the RHI
    /// layer resolves against real surface support (RHI::choose_present_mode(), RHI/Swapchain.hpp).
    /// Variable refresh takes priority over vsync/latency/preference entirely — it's a different
    /// axis (display pacing, not present-mode choice), so a VRR request short-circuits the rest.
    [[nodiscard]] RHI::PresentStrategy resolve_present_strategy(const PresentationSettings &settings) noexcept;

    /// The engine asks for what it wants; the backend grants what it can and reports truth via
    /// RendererCapabilities and RHI's FeatureNegotiationReport. Requesting raytracing does not
    /// guarantee it unless the feature is also placed in `required_rhi_features`.
    struct RendererFeatureRequest {
        b8 raytracing = false;
        b8 prefer_async_compute = false;
        RHI::FeatureSet required_rhi_features{};
        RHI::FeatureSet optional_rhi_features{};
        u32 desired_frames_in_flight = 2;
        PresentationSettings presentation{};
        /// Opt-in escape hatch (Vulkan backend: VulkanNativeAccessExtension, see Core/Vulkan/Rhi/) —
        /// exposes the raw VkInstance/VkPhysicalDevice/VkDevice/VkQueue/VkCommandBuffer for callers who
        /// need to interoperate with vendor SDKs (FSR2/DLSS/XeSS) or Vulkan capabilities RHI hasn't
        /// modeled yet. Off by default: using the returned handles bypasses RHI's tracking guarantees
        /// for whatever the caller does with them, so it must be requested explicitly, never implied.
        b8 enable_native_access_extension = false;
    };

    struct RendererCreateInfo {
        /// Backend and physical GPU selected from RHI::GpuInventory. An empty physical_device_id lets
        /// the backend apply its normal preference policy; a non-empty ID must match AdapterInfo exactly.
        RHI::BackendType backend = RHI::BackendType::Vulkan;
        string physical_device_id;
        RendererFeatureRequest features{};
        const char *app_name = "SturdyEngine";
        /// Non-owning pointer to the primary window the backend presents into. Must outlive the
        /// renderer backend (owned by the application/engine layer). The backend uses it to create,
        /// own, resize, and destroy its surfaces internally.
        Window *window = nullptr;
        /// WSI instance extension strings from the windowing backend (e.g. VK_KHR_surface +
        /// VK_KHR_xlib_surface). Pointers must stay valid for the duration of initialize().
        /// SDL3 and GLFW return pointers into their own static storage, so this is safe.
        vector<const char *> wsi_extensions;
        /// Every shader discovered + reflected before the backend came up. The backend owns turning
        /// these into its native format (Vulkan: SPIR-V modules per entry point) during initialize().
        /// Non-owning: the backing storage (the engine's shader list) must outlive initialize().
        span<const Slang::UnCompiledShader> uncompiled_shaders;
        /// Mirrors Engine::EngineConfig::enable_shader_disk_cache — threaded through here so
        /// Renderer::create_material_template_from_source() (which stores the whole
        /// RendererCreateInfo as recovery_create_info_) can read it when constructing each material
        /// template's Slang::ShaderVariantCache.
        bool enable_shader_disk_cache = true;
    };

    /// Per-frame payload from the engine to the backend. Timing and drawable resolution live here so
    /// high-level cameras/post effects can build projection and screen-space constants from the same
    /// framebuffer extent the backend will render into.
    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
        u32 framebuffer_width = 0;
        u32 framebuffer_height = 0;
        /// True only for the coalesced interactive-resize path. Backends may skip a host-side wait for
        /// an already-issued present in this mode, retaining the previous image until the next retry
        /// rather than stalling the window coordinator inside an OS modal resize loop.
        bool live_resize = false;
    };

} // namespace SFT::Core
