#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <WindowManager/WindowManager.hpp>
#include <RHI/RHI.hpp>
#include <Core/Slang/ShaderDiscovery.hpp>

using SFT::WindowManager::Window;
using std::expected;
using std::span;
using std::string;
using std::vector;

namespace SFT::Core {


    struct GpuInfo {
        string name;
        string vendor;
        string driver_version;

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


    struct FramesInFlightResolution {
        u32 requested = 0;
        u32 lower_bound = 1;
        u32 upper_bound = 0;
        u32 resolved = 0;
        enum class Adjustment : u8 { Accepted, RaisedToLower, ReducedToUpper };
        Adjustment adjustment = Adjustment::Accepted;
    };


    /// Resolves frames in flight into the concrete value used by the engine.
    ///
    /// @param requested `requested` value used by the operation.
    /// @param lower_bound `lower_bound` value used by the operation.
    /// @param upper_bound `upper_bound` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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


    enum class VSyncMode : u8 {
        Off,
        On,
        Adaptive,


    };

    enum class VariableRefreshMode : u8 {
        Disabled,


        Automatic,


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


    enum class HdrColorSpaceMode : u8 {
        Hdr10St2084,
        ScrgbLinear,


        Hdr10Hlg,


        DolbyVision,
    };

    struct PresentationSettings {
        VSyncMode vsync = VSyncMode::On;


        VariableRefreshMode variable_refresh = VariableRefreshMode::Disabled;
        LatencyMode latency = LatencyMode::Normal;
        PresentationPreference preference = PresentationPreference::Automatic;


        b8 hdr_enabled = false;
        HdrColorSpaceMode hdr_color_space = HdrColorSpaceMode::Hdr10St2084;


        b8 transparent_composition = false;

        u32 swapchain_image_count = 0;


        b8 allow_present_from_compute = false;
    };


    /// Resolves present strategy into the concrete value used by the engine.
    ///
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::PresentStrategy resolve_present_strategy(const PresentationSettings &settings) noexcept;


    struct RendererFeatureRequest {
        b8 raytracing = false;
        b8 prefer_async_compute = false;
        RHI::FeatureSet required_rhi_features{};
        RHI::FeatureSet optional_rhi_features{};
        u32 desired_frames_in_flight = 2;
        PresentationSettings presentation{};


        b8 enable_native_access_extension = false;
    };

    struct RendererCreateInfo {


        RHI::BackendType backend = RHI::BackendType::Vulkan;
        string physical_device_id;
        RendererFeatureRequest features{};
        const char *app_name = "SturdyEngine";


        Window *window = nullptr;



        span<const Slang::UnCompiledShader> uncompiled_shaders;


        bool enable_shader_disk_cache = true;
    };


    struct FrameInput {
        f64 delta_seconds = 0.0;
        u64 frame_index = 0;
        u32 framebuffer_width = 0;
        u32 framebuffer_height = 0;


        bool live_resize = false;
    };

} // namespace SFT::Core
