#pragma once

// Plain-data types for launching the engine and driving frames. bindgen-safe: <cstddef>/<cstdint>
// and other Types/ headers only (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// Mirrors `WindowManager::WindowMode`.
    enum class WindowMode : std::uint32_t {
        Windowed = 0,
        BorderlessFullscreen = 1,
        ExclusiveFullscreen = 2,
    };

    /// Mirrors `Core::VSyncMode`.
    enum class VSync : std::uint32_t {
        Off = 0,
        On = 1,
        Adaptive = 2,
    };

    /// Mirrors `Core::VariableRefreshMode`.
    enum class VariableRefresh : std::uint32_t {
        Disabled = 0,
        Automatic = 1,
        Preferred = 2,
    };

    /// Mirrors `Core::LatencyMode`.
    enum class LatencyMode : std::uint32_t {
        Normal = 0,
        Low = 1,
        Ultra = 2,
    };

    /// Mirrors `Core::PresentationPreference`.
    enum class PresentationPreference : std::uint32_t {
        Automatic = 0,
        LowestLatency = 1,
        Smoothest = 2,
        PowerEfficient = 3,
    };

    /// Mirrors `Core::HdrColorSpaceMode`.
    enum class HdrColorSpace : std::uint32_t {
        Hdr10St2084 = 0,
        ScrgbLinear = 1,
        Hdr10Hlg = 2,
        DolbyVision = 3,
    };

    /// Mirrors `Core::FrameRateLimitMode`.
    enum class FrameRateLimit : std::uint32_t {
        Unlimited = 0,
        Custom = 1,
        MatchDisplayRefresh = 2,
    };

    /// Mirrors `RHI::BackendType`.
    enum class GraphicsBackend : std::uint32_t {
        Vulkan = 0,
        D3D12 = 1,
        Metal = 2,
        WebGpu = 3,
    };

    /// Every field of `Core::PresentationSettings`.
    struct PresentationOptions {
        VSync vsync = VSync::On;
        VariableRefresh variable_refresh = VariableRefresh::Disabled;
        LatencyMode latency = LatencyMode::Normal;
        PresentationPreference preference = PresentationPreference::Automatic;
        bool hdr_enabled = false;
        HdrColorSpace hdr_color_space = HdrColorSpace::Hdr10St2084;
        bool transparent_composition = false;
        /// 0 lets the engine choose.
        std::uint32_t swapchain_image_count = 0;
        bool allow_present_from_compute = false;
        FrameRateLimit frame_rate_limit = FrameRateLimit::Unlimited;
        double frame_rate_limit_fps = 0.0;
        double variable_refresh_margin_fps = 3.0;
        /// 0 disables the unfocused cap.
        double unfocused_frame_rate_limit_fps = 0.0;
        bool snap_frame_rate_limit_to_refresh = true;
    };

    /// The numeric part of a runtime configuration. Strings (window title, app name, shader
    /// directory, device id) and RHI feature names are set on `RuntimeConfig` directly.
    struct RuntimeOptions {
        /// Primary window client-area size in physical pixels.
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool resizable = true;
        bool decorated = true;
        bool high_dpi = true;
        WindowMode window_mode = WindowMode::Windowed;
        bool window_transparent = false;
        bool window_visible = true;
        bool has_window_position = false;
        std::int32_t window_x = 0;
        std::int32_t window_y = 0;
        /// Seconds between window-title refreshes (FPS readout); <= 0 disables.
        double title_update_interval_seconds = 0.0;
        bool runtime_window_management = false;
        GraphicsBackend graphics_backend = GraphicsBackend::Vulkan;
        bool raytracing = false;
        bool prefer_async_compute = false;
        /// 0 keeps the engine default.
        std::uint32_t desired_frames_in_flight = 0;
        /// Enables the native-access RHI extension (raw Vulkan/D3D12 handles).
        bool enable_native_access = false;
        bool enable_shader_disk_cache = true;
        PresentationOptions presentation{};
    };

    /// Per-frame input handed to `GameLogicCallbacks::request_render_frame`.
    struct FrameInfo {
        double delta_seconds = 0.0;
        std::uint64_t frame_index = 0;
        std::uint32_t framebuffer_width = 0;
        std::uint32_t framebuffer_height = 0;
        bool live_resize = false;
        std::uint64_t window_id = 0;
    };

    /// Scene-wide lighting for one frame (`Engine::SceneLighting`).
    struct SceneLightingDesc {
        float ambient_radiance[3] = {0.02f, 0.02f, 0.02f};
        float exposure = 1.0f;
    };

    /// Numeric identity of the active GPU. The strings (name, vendor, driver, API version, device
    /// type) are read with `engine_gpu_string`.
    struct GpuDescription {
        std::uint32_t vendor_id = 0;
        std::uint32_t device_id = 0;
    };

    /// Selects one of the GPU's descriptive strings.
    enum class GpuString : std::uint32_t {
        Name = 0,
        Vendor = 1,
        DriverVersion = 2,
        ApiVersion = 3,
        DeviceType = 4,
    };

    /// `Core::RendererCapabilities`.
    struct RendererCapabilities {
        bool multithreaded_command_recording = false;
        bool async_compute = false;
        bool raytracing = false;
        bool mesh_shaders = false;
        bool bindless = false;
        bool timeline_semaphores = false;
        std::uint32_t max_frames_in_flight = 2;
    };

} // namespace SFT::CxxApi
