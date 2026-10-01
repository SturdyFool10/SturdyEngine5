#include <Core/Renderer.hpp>

#include <algorithm>
#include <cmath>


namespace SFT::Core {

    /// Resolves frames in flight into the concrete value used by the engine.
    ///
    /// @param requested `requested` value used by the operation.
    /// @param lower_bound `lower_bound` value used by the operation.
    /// @param upper_bound `upper_bound` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<FramesInFlightResolution, string> resolve_frames_in_flight(
        u32 requested, u32 lower_bound, u32 upper_bound) noexcept {
        if (lower_bound == 0) {
            lower_bound = 1;
        }
        if (upper_bound != 0 && lower_bound > upper_bound) {
            return std::unexpected(
                "invalid frames-in-flight bounds: lower_bound (" + std::to_string(lower_bound) +
                ") exceeds upper_bound (" + std::to_string(upper_bound) + ")");
        }

        FramesInFlightResolution result{
            .requested = requested,
            .lower_bound = lower_bound,
            .upper_bound = upper_bound,
        };
        u32 resolved = requested == 0 ? lower_bound : requested;
        if (resolved < lower_bound) {
            resolved = lower_bound;
            result.adjustment = FramesInFlightResolution::Adjustment::RaisedToLower;
        } else if (upper_bound != 0 && resolved > upper_bound) {
            resolved = upper_bound;
            result.adjustment = FramesInFlightResolution::Adjustment::ReducedToUpper;
        }
        result.resolved = resolved;
        return result;
    }

    /// Resolves present strategy into the concrete value used by the engine.
    ///
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RHI::PresentStrategy resolve_present_strategy(const PresentationSettings &settings) noexcept {
        if (settings.variable_refresh != VariableRefreshMode::Disabled) {
            return RHI::PresentStrategy::VariableRefresh;
        }
        switch (settings.vsync) {
            case VSyncMode::Off:
                return RHI::PresentStrategy::Unsynchronized;
            case VSyncMode::Adaptive:
                return RHI::PresentStrategy::AdaptiveTearing;
            case VSyncMode::On: {


                const bool wants_low_latency = settings.latency != LatencyMode::Normal ||
                    settings.preference == PresentationPreference::LowestLatency;
                return wants_low_latency ? RHI::PresentStrategy::TearFreeLatest : RHI::PresentStrategy::TearFreeOrdered;
            }
        }
        return RHI::PresentStrategy::TearFreeOrdered;
    }

    /// Resolves every frame-rate-limiting dial into the one target dispatch rate that applies this frame.
    ///
    /// @param settings The surface's presentation policy.
    /// @param inputs State only the window/render-loop layer knows (focus, the display's current refresh rate).
    ///
    /// @return Returns the target frames per second, or `0.0` for "no cap".
    /// @note This function does not throw exceptions.
    f64 resolve_frame_rate_limit(const PresentationSettings &settings, const FrameRateLimitInputs &inputs) noexcept {
        f64 target_fps = 0.0; // 0.0 means "no cap so far"

        switch (settings.frame_rate_limit_mode) {
            case FrameRateLimitMode::Custom:
                if (settings.frame_rate_limit_fps > 0.0) {
                    target_fps = settings.frame_rate_limit_fps;
                }
                break;
            case FrameRateLimitMode::MatchDisplayRefresh:
                if (inputs.display_refresh_hz && *inputs.display_refresh_hz > 0.0f) {
                    target_fps = static_cast<f64>(*inputs.display_refresh_hz);
                }
                break;
            case FrameRateLimitMode::Unlimited:
                break;
        }

        // Variable refresh needs delivery to stay under the display's own ceiling to keep pacing itself.
        // Applied on top of whatever the switch above picked -- including "still uncapped" -- so enabling
        // adaptive sync alone is enough to get a safe cap without the caller also having to compute one.
        if (settings.variable_refresh != VariableRefreshMode::Disabled && inputs.display_refresh_hz &&
            *inputs.display_refresh_hz > 0.0f) {
            const f64 vrr_target =
                std::max(1.0, static_cast<f64>(*inputs.display_refresh_hz) - settings.variable_refresh_margin_fps);
            target_fps = target_fps > 0.0 ? std::min(target_fps, vrr_target) : vrr_target;
        }

        // The unfocused throttle only ever tightens an existing cap (or introduces one of its own); it
        // never loosens whatever the fields above already decided.
        if (!inputs.focused && settings.unfocused_frame_rate_limit_fps > 0.0) {
            target_fps = target_fps > 0.0 ? std::min(target_fps, settings.unfocused_frame_rate_limit_fps)
                                          : settings.unfocused_frame_rate_limit_fps;
        }

        return target_fps;
    }

    namespace {

        /// Present modes whose presentation engine blocks the producer on the display's vblank, making the
        /// display itself the loop's clock. Mailbox/FifoLatestReady replace queued images instead of waiting for
        /// them, and Immediate never waits, so none of those pace a loop on their own.
        [[nodiscard]] constexpr bool present_mode_blocks_on_vblank(RHI::PresentMode mode) noexcept {
            return mode == RHI::PresentMode::Fifo || mode == RHI::PresentMode::FifoRelaxed;
        }

        /// A limit this close to the refresh rate is "the refresh rate": a CPU clock at the same nominal rate as
        /// a blocking vsync would only drift in phase against it and periodically cost a whole refresh.
        constexpr f64 same_rate_tolerance = 0.995;

    } // namespace

    FramePacingPlan resolve_frame_pacing(const PresentationSettings &settings, const FramePacingInputs &inputs) noexcept {
        FramePacingPlan plan{};

        // Refresh: the presentation engine describes the swapchain actually presenting; the window system's
        // display mode is a fallback (it can be rounded, or describe a different mode than the one in use).
        f64 refresh = 0.0;
        if (inputs.refresh_duration_seconds && *inputs.refresh_duration_seconds > 0.0) {
            refresh = *inputs.refresh_duration_seconds;
        } else if (inputs.display_refresh_hz && *inputs.display_refresh_hz > 0.0f) {
            refresh = 1.0 / static_cast<f64>(*inputs.display_refresh_hz);
        }
        plan.refresh_duration = refresh;
        const f64 refresh_hz = refresh > 0.0 ? 1.0 / refresh : 0.0;

        // Variable refresh is only as real as the presentation engine says it is: a fixed-refresh display with
        // VRR requested gets no headroom margin (it would only produce a rolling tear line). Without a report,
        // trust the request.
        const bool vrr_requested = settings.variable_refresh != VariableRefreshMode::Disabled;
        const bool vrr = vrr_requested && inputs.variable_refresh_active.value_or(true);
        plan.variable_refresh = vrr;

        PresentationSettings limit_settings = settings;
        if (!vrr) {
            limit_settings.variable_refresh = VariableRefreshMode::Disabled;
        }
        FrameRateLimitInputs limit_inputs{.focused = inputs.focused};
        if (refresh_hz > 0.0) {
            limit_inputs.display_refresh_hz = static_cast<f32>(refresh_hz);
        }
        f64 requested = resolve_frame_rate_limit(limit_settings, limit_inputs);

        const bool blocking = inputs.effective_present_mode && present_mode_blocks_on_vblank(*inputs.effective_present_mode);

        if (!vrr && refresh_hz > 0.0) {
            // Fixed refresh: only `refresh / n` can be shown evenly. A limit is a ceiling, so round down to the
            // fastest divisor rate that does not exceed it (144 Hz with a 60 fps limit -> 48 fps, every frame on
            // screen for exactly 3 refreshes), never up past what was asked for.
            if (requested > 0.0 && requested < refresh_hz * same_rate_tolerance && settings.snap_frame_rate_limit_to_refresh) {
                const u32 n = std::max(1u, static_cast<u32>(std::ceil(refresh_hz / requested - 0.005)));
                const f64 snapped = refresh_hz / static_cast<f64>(n);
                plan.snapped_to_refresh = std::abs(snapped - requested) > requested * 1.0e-6;
                plan.refreshes_per_frame = n;
                requested = snapped;
            }

            if (blocking && (requested <= 0.0 || requested >= refresh_hz * same_rate_tolerance)) {
                plan.clock = FramePacingClock::DisplayVsync;
                plan.refreshes_per_frame = 1;
                plan.expected_frame_interval = refresh;
            } else if (requested > 0.0) {
                plan.clock = FramePacingClock::CpuTimer;
                plan.cpu_target_fps = requested;
                plan.expected_frame_interval =
                    plan.refreshes_per_frame > 0 ? refresh * static_cast<f64>(plan.refreshes_per_frame) : 1.0 / requested;
            }
        } else if (requested > 0.0) {
            // Variable refresh, or a refresh rate nobody could report: the CPU limiter is the only clock we trust.
            plan.clock = FramePacingClock::CpuTimer;
            plan.cpu_target_fps = requested;
            plan.expected_frame_interval = 1.0 / requested;
        } else if (blocking) {
            plan.clock = FramePacingClock::DisplayVsync;
        }

        switch (settings.latency) {
            case LatencyMode::Normal:
                break;
            case LatencyMode::Low:
                plan.max_queued_presents = 1;
                break;
            case LatencyMode::Ultra:
                plan.max_queued_presents = 1;
                // Aiming for "just before the present slot" needs a slot: with no clock there is none, and an
                // unpaced non-blocking present mode already shows the newest frame as soon as it can.
                plan.just_in_time_start = plan.clock != FramePacingClock::Unpaced;
                break;
        }
        if (settings.preference == PresentationPreference::LowestLatency && plan.max_queued_presents == 0) {
            plan.max_queued_presents = 1;
        }
        return plan;
    }

} // namespace SFT::Core

