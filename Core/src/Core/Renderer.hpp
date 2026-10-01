#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <optional>
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

    /// The "FPS Limit" dial: how the render loop caps its own dispatch rate, independent of whatever the
    /// swapchain's present mode is already doing. The two compose rather than conflict — a limit is a
    /// ceiling on top of the present mode's own pacing, never a replacement for it (see
    /// `resolve_frame_rate_limit` for exactly how every dial below combines into one target).
    enum class FrameRateLimitMode : u8 {
        /// No engine-imposed cap: dispatch as fast as the resolved present mode allows. Correct and
        /// intentional with a vsync-blocking present mode (Fifo/FifoLatestReady); with a non-blocking one
        /// (Mailbox/Immediate/FifoRelaxed) this really does mean uncapped, unpaced dispatch — an explicit
        /// choice the caller must opt into by leaving this as the default, not something forced on it.
        /// (`variable_refresh` still applies its own cap even under `Unlimited` — see its own field.)
        Unlimited,

        /// Cap dispatch to `PresentationSettings::frame_rate_limit_fps`, regardless of present mode.
        Custom,

        /// Cap dispatch to the display's current refresh rate (queried per frame, since a window can move
        /// between displays with different rates). Falls back to `Unlimited`'s behaviour if the platform
        /// or window can't report one.
        MatchDisplayRefresh,
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

        /// See `FrameRateLimitMode`.
        FrameRateLimitMode frame_rate_limit_mode = FrameRateLimitMode::Unlimited;
        /// Target frames per second; meaningful only when `frame_rate_limit_mode == Custom`. A value
        /// `<= 0` is treated the same as `FrameRateLimitMode::Unlimited`.
        f64 frame_rate_limit_fps = 0.0;

        /// Headroom kept below the display's refresh rate while `variable_refresh` is engaged, in fps.
        /// Real adaptive-sync hardware (G-Sync/FreeSync) needs frame delivery to stay under the display's
        /// ceiling to keep pacing itself; this is applied automatically whenever the display's current
        /// refresh rate can be queried, on top of (never loosening) whatever `frame_rate_limit_mode`
        /// otherwise picked — including `Unlimited`, so enabling variable refresh alone is enough to get a
        /// sane cap without also having to compute one by hand. Ignored if the refresh rate can't be
        /// queried (no display-refresh support on this platform/window yet always queries).
        f64 variable_refresh_margin_fps = 3.0;

        /// A separate, usually lower cap applied while the window is not focused (alt-tabbed away, in the
        /// background, ...), to save power/heat rather than rendering a window nobody is looking at as
        /// fast as the foreground one. `<= 0` disables this (no separate background cap; the normal
        /// `frame_rate_limit_mode` result still applies). Only ever tightens the cap, never loosens one
        /// already chosen by the fields above.
        f64 unfocused_frame_rate_limit_fps = 0.0;

        /// On a fixed-refresh display, round the frame-rate limit to the nearest `refresh / n`. Any other cap
        /// cannot be displayed evenly (60 fps on 144 Hz shows frames for 2,3,2,3... refreshes — judder), so this
        /// is on by default; turn it off only to hit an exact non-divisor rate on purpose (e.g. for benchmarking).
        /// Has no effect on a variable-refresh display, which has no fixed cadence to align to.
        b8 snap_frame_rate_limit_to_refresh = true;
    };


    /// Resolves present strategy into the concrete value used by the engine.
    ///
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::PresentStrategy resolve_present_strategy(const PresentationSettings &settings) noexcept;

    /// What `resolve_frame_rate_limit` needs beyond `PresentationSettings` itself: state only the window/
    /// render-loop layer knows, not something `PresentationSettings` (a pure policy value) should carry.
    struct FrameRateLimitInputs {
        /// Whether the window is currently focused; see `PresentationSettings::unfocused_frame_rate_limit_fps`.
        bool focused = true;
        /// The display's current refresh rate in Hz, queried fresh this frame (a window can move between
        /// displays with different rates) — `std::nullopt` if it couldn't be queried (no platform support,
        /// or the query failed), in which case `MatchDisplayRefresh` and the `variable_refresh` auto-cap
        /// both fall back to not capping on that basis.
        std::optional<f32> display_refresh_hz;
    };

    /// Resolves every frame-rate-limiting dial in `PresentationSettings` into the one target dispatch
    /// rate that actually applies this frame — the single place `Custom`, `MatchDisplayRefresh`, the
    /// `variable_refresh` auto-cap, and the unfocused throttle combine, so a render loop (see
    /// `Application::render_managed_window`) just hands this straight to a `Foundation::FramePacer`
    /// without re-deriving the policy itself.
    ///
    /// @return The target frames per second, or `0.0` for "no cap" (feed straight to
    ///         `FramePacer::wait_for_frame_time`, which already treats `<= 0` as unlimited).
    /// @note This function does not throw exceptions.
    [[nodiscard]] f64 resolve_frame_rate_limit(const PresentationSettings &settings,
                                               const FrameRateLimitInputs &inputs) noexcept;

    /// What paces a render loop once a `FramePacingPlan` is applied.
    enum class FramePacingClock : u8 {
        /// Nothing: dispatch runs as fast as the resolved present mode allows.
        Unpaced,
        /// The CPU frame limiter (`Foundation::FramePacer`) is the clock.
        CpuTimer,
        /// A blocking present mode (Fifo / FifoRelaxed) is the clock; the CPU limiter stands down so the two
        /// clocks cannot beat against each other.
        DisplayVsync,
    };

    /// Everything the pacing policy needs beyond `PresentationSettings`: state only the platform/window layer
    /// knows. Presentation-engine values (from `VK_EXT_present_timing` or DXGI frame statistics) take priority
    /// over the window system's display mode, because they describe the swapchain that is actually presenting.
    struct FramePacingInputs {
        bool focused = true;
        /// The window's current display mode refresh rate (window system; may be stale or rounded).
        std::optional<f32> display_refresh_hz;
        /// Refresh cycle duration reported by the presentation engine, in seconds (the *minimum* cycle under VRR).
        std::optional<f64> refresh_duration_seconds;
        /// Whether the presentation engine reports variable-refresh operation; `std::nullopt` when unknown.
        std::optional<bool> variable_refresh_active;
        /// The present mode the swapchain actually resolved to; `std::nullopt` when unknown.
        std::optional<RHI::PresentMode> effective_present_mode;
    };

    /// The resolved frame-pacing decision for one frame.
    struct FramePacingPlan {
        FramePacingClock clock = FramePacingClock::Unpaced;
        /// Target for `Foundation::FramePacer::pace`; `0` when the CPU limiter should not wait.
        f64 cpu_target_fps = 0.0;
        /// How long each frame is expected to stay on screen, in seconds; `0` when unknown (unpaced). This is
        /// what a simulation should advance by to avoid judder when no measured display time is available.
        f64 expected_frame_interval = 0.0;
        /// Refresh duration the plan was built against, in seconds (`0` when unknown).
        f64 refresh_duration = 0.0;
        /// On a fixed-refresh display: each frame is meant to be shown for this many refreshes (`0` = n/a).
        u32 refreshes_per_frame = 0;
        /// True when the requested limit was rounded to a refresh divisor.
        bool snapped_to_refresh = false;
        /// True when variable refresh is (believed to be) active for this plan.
        bool variable_refresh = false;
        /// Latency bound: presents allowed to be outstanding (submitted, not yet on screen) when a new frame
        /// starts. `0` = no explicit bound beyond frames-in-flight.
        u32 max_queued_presents = 0;
        /// Ultra latency: delay the start of each frame so it finishes just before its present slot.
        bool just_in_time_start = false;
    };

    /// Resolves every frame-pacing dial plus what the platform reports into one plan (a pure function; see
    /// plans/frame-pacing.md for the reasoning behind each rule):
    ///
    /// * Fixed refresh + blocking present mode: the display is the clock. A limit at or above the refresh rate
    ///   (or none) leaves pacing to vsync alone; a lower limit is rounded to `refresh / n` (when snapping is on)
    ///   and paced by the CPU limiter.
    /// * Fixed refresh + non-blocking present mode (Mailbox/Immediate/FifoLatestReady): the CPU limiter paces,
    ///   again rounded to a refresh divisor when snapping is on.
    /// * Variable refresh: the CPU limiter paces below the display's maximum (see `variable_refresh_margin_fps`).
    /// * `LatencyMode::Low` bounds the present queue to one; `Ultra` also starts frames just in time.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] FramePacingPlan resolve_frame_pacing(const PresentationSettings &settings,
                                                       const FramePacingInputs &inputs) noexcept;

    /// What the presentation engine has reported about one surface's presents (see plans/frame-pacing.md). All
    /// fields are "unknown" (`available == false`) on backends without present timing.
    struct PresentTimingFeedback {
        /// The backend reports when frames reach the display for this surface.
        bool available = false;
        /// Present mode the surface actually presents with.
        std::optional<RHI::PresentMode> effective_present_mode;
        /// The presentation engine's refresh cycle (the minimum cycle under VRR), in seconds.
        std::optional<f64> refresh_duration_seconds;
        /// Fixed (`false`) or variable (`true`) refresh as reported by the presentation engine.
        std::optional<bool> variable_refresh_active;
        /// Displayed-frame interval statistics over the recent window, in seconds.
        u32 sample_count = 0;
        f64 mean_display_interval = 0.0;
        f64 display_interval_jitter = 0.0; // standard deviation
        f64 max_display_interval = 0.0;
        /// Frames that stayed on screen more than 1.5 refresh cycles longer than intended on a fixed-refresh
        /// display (a missed vsync), since the swapchain was created.
        u64 missed_refreshes = 0;
        u64 presents_reported = 0;
        /// Most recent present id handed to the presentation engine (`0` = none yet).
        u64 last_present_id = 0;
        /// `std::chrono::steady_clock` time (ns since its epoch) the newest reported present reached the display;
        /// `0` = none yet.
        u64 last_display_time_ns = 0;
        /// Frame start (render thread picks the frame up) to display, seconds: mean + 2 sigma over the recent
        /// window. Includes any time the frame waited in the present queue, so it shrinks toward the true frame
        /// cost as just-in-time starts remove the queueing, and grows again if a slot is missed.
        f64 frame_to_display_estimate = 0.0;
    };


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
