/// CPU-only correctness check for `Core::resolve_frame_rate_limit` — the single place every
/// frame-rate-limiting dial (Custom, MatchDisplayRefresh, the variable-refresh auto-cap, the unfocused
/// throttle) combines into one target dispatch rate. No window/renderer needed: it's a pure function of
/// `PresentationSettings` + `FrameRateLimitInputs`.

#include <Core/Renderer.hpp>

#include <cmath>
#include <iostream>
#include <optional>

namespace {

    using namespace SFT::Core;

    int g_failures = 0;

    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    constexpr f64 tolerance = 1e-9;
    bool nearly(f64 a, f64 b) { return std::abs(a - b) < tolerance; }

} // namespace

int main() {
    // Unlimited, no variable refresh, no unfocused throttle: no cap at all.
    {
        PresentationSettings settings{};
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{}), 0.0),
              "Unlimited with nothing else engaged resolves to no cap");
    }

    // Custom applies regardless of focus or display-refresh availability.
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 120.0;
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.focused = true}), 120.0),
              "Custom applies its configured target");
        check(nearly(resolve_frame_rate_limit(
                         settings, FrameRateLimitInputs{.focused = true, .display_refresh_hz = 240.0f}),
                     120.0),
              "Custom is unaffected by an available (but irrelevant) display refresh rate");
    }

    // Custom with a non-positive value behaves like Unlimited.
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 0.0;
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{}), 0.0),
              "Custom with frame_rate_limit_fps <= 0 behaves like Unlimited");
    }

    // MatchDisplayRefresh uses the queried rate when available, falls back to unlimited otherwise.
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::MatchDisplayRefresh;
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = 144.0f}),
                     144.0),
              "MatchDisplayRefresh targets the queried display refresh rate");
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = std::nullopt}),
                     0.0),
              "MatchDisplayRefresh falls back to no cap when the refresh rate can't be queried");
    }

    // Variable refresh auto-caps to (display refresh - margin) even under Unlimited, without the caller
    // having to configure anything beyond enabling it.
    {
        PresentationSettings settings{};
        settings.variable_refresh = VariableRefreshMode::Automatic;
        settings.variable_refresh_margin_fps = 3.0;
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = 165.0f}),
                     162.0),
              "variable_refresh auto-caps to (display refresh - margin) under Unlimited");
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = std::nullopt}),
                     0.0),
              "variable_refresh applies no cap when the refresh rate can't be queried");
    }

    // Variable refresh only ever *tightens* an explicit Custom target, never loosens it.
    {
        PresentationSettings settings{};
        settings.variable_refresh = VariableRefreshMode::Automatic;
        settings.variable_refresh_margin_fps = 3.0;
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 60.0; // well under (165 - 3) = 162
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = 165.0f}), 60.0),
              "a tighter explicit Custom target is not loosened by the variable-refresh auto-cap");
        settings.frame_rate_limit_fps = 200.0; // over (165 - 3) = 162: the auto-cap should win
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.display_refresh_hz = 165.0f}), 162.0),
              "the variable-refresh auto-cap tightens a looser explicit Custom target");
    }

    // The unfocused throttle only tightens, and only while actually unfocused.
    {
        PresentationSettings settings{};
        settings.unfocused_frame_rate_limit_fps = 15.0;
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.focused = true}), 0.0),
              "the unfocused throttle does not apply while focused");
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.focused = false}), 15.0),
              "the unfocused throttle applies its own cap while unfocused with nothing else engaged");

        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 10.0; // tighter than the unfocused throttle
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.focused = false}), 10.0),
              "the unfocused throttle does not loosen a tighter Custom target");
        settings.frame_rate_limit_fps = 240.0; // looser than the unfocused throttle
        check(nearly(resolve_frame_rate_limit(settings, FrameRateLimitInputs{.focused = false}), 15.0),
              "the unfocused throttle tightens a looser Custom target");
    }

    // ---- resolve_frame_pacing: present-mode- and display-aware policy ----
    const auto fixed = [](f64 hz, SFT::RHI::PresentMode mode) {
        return FramePacingInputs{.refresh_duration_seconds = 1.0 / hz,
                                 .variable_refresh_active = false,
                                 .effective_present_mode = mode};
    };
    using SFT::RHI::PresentMode;

    // Blocking vsync with no limit: the display is the clock, the CPU limiter stands down.
    {
        const FramePacingPlan plan = resolve_frame_pacing(PresentationSettings{}, fixed(144.0, PresentMode::Fifo));
        check(plan.clock == FramePacingClock::DisplayVsync && plan.cpu_target_fps == 0.0,
              "Fifo with no limit is paced by vsync alone");
        check(plan.refreshes_per_frame == 1 && nearly(plan.expected_frame_interval, 1.0 / 144.0),
              "vsync pacing expects one refresh per frame");
    }

    // A non-divisor limit under vsync is rounded down to the fastest even cadence: 60 on 144 Hz -> 48 (3 refreshes).
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 60.0;
        const FramePacingPlan plan = resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo));
        check(plan.clock == FramePacingClock::CpuTimer && nearly(plan.cpu_target_fps, 48.0),
              "60 fps on 144 Hz snaps down to 48 fps");
        check(plan.snapped_to_refresh && plan.refreshes_per_frame == 3, "the snap reports 3 refreshes per frame");
        check(nearly(plan.expected_frame_interval, 3.0 / 144.0), "the expected interval is exactly 3 refreshes");

        settings.snap_frame_rate_limit_to_refresh = false;
        const FramePacingPlan unsnapped = resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo));
        check(nearly(unsnapped.cpu_target_fps, 60.0) && !unsnapped.snapped_to_refresh &&
                  unsnapped.refreshes_per_frame == 0,
              "with snapping off the exact (uneven) limit is honoured");
    }

    // An exact divisor is not reported as a snap.
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 60.0;
        const FramePacingPlan plan = resolve_frame_pacing(settings, fixed(120.0, PresentMode::Fifo));
        check(nearly(plan.cpu_target_fps, 60.0) && !plan.snapped_to_refresh && plan.refreshes_per_frame == 2,
              "60 fps on 120 Hz is already even (2 refreshes), not a snap");
    }

    // A limit at or above the refresh rate under vsync leaves vsync as the only clock (no beating).
    {
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 144.0;
        check(resolve_frame_pacing(settings, fixed(60.0, PresentMode::Fifo)).clock == FramePacingClock::DisplayVsync,
              "a limit above the refresh rate under vsync defers to vsync");
        settings.frame_rate_limit_mode = FrameRateLimitMode::MatchDisplayRefresh;
        check(resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo)).clock == FramePacingClock::DisplayVsync,
              "MatchDisplayRefresh under vsync defers to vsync");
    }

    // Non-blocking present modes: unpaced without a limit, CPU-paced (and snapped) with one.
    {
        check(resolve_frame_pacing(PresentationSettings{}, fixed(144.0, PresentMode::Mailbox)).clock ==
                  FramePacingClock::Unpaced,
              "Mailbox with no limit is unpaced");
        PresentationSettings settings{};
        settings.frame_rate_limit_mode = FrameRateLimitMode::Custom;
        settings.frame_rate_limit_fps = 100.0;
        const FramePacingPlan plan = resolve_frame_pacing(settings, fixed(144.0, PresentMode::Mailbox));
        check(plan.clock == FramePacingClock::CpuTimer && nearly(plan.cpu_target_fps, 72.0),
              "Mailbox with a 100 fps limit on 144 Hz paces at 72 fps (2 refreshes)");
    }

    // Variable refresh reported by the presentation engine: cap below the maximum automatically.
    {
        PresentationSettings settings{};
        settings.variable_refresh = VariableRefreshMode::Automatic;
        settings.variable_refresh_margin_fps = 3.0;
        const FramePacingInputs vrr{.refresh_duration_seconds = 1.0 / 165.0,
                                    .variable_refresh_active = true,
                                    .effective_present_mode = PresentMode::Immediate};
        const FramePacingPlan plan = resolve_frame_pacing(settings, vrr);
        check(plan.variable_refresh && plan.clock == FramePacingClock::CpuTimer &&
                  std::abs(plan.cpu_target_fps - 162.0) < 1e-3,
              "VRR paces at the display maximum minus the margin");
        check(plan.refreshes_per_frame == 0 && !plan.snapped_to_refresh, "VRR has no refresh divisor to snap to");

        // Same request, but the engine says the display is fixed-refresh: no margin, no VRR.
        FramePacingInputs frr = vrr;
        frr.variable_refresh_active = false;
        const FramePacingPlan fallback = resolve_frame_pacing(settings, frr);
        check(!fallback.variable_refresh && fallback.clock == FramePacingClock::Unpaced,
              "VRR requested on a fixed-refresh display applies no VRR margin");
    }

    // The presentation engine's refresh duration wins over the window system's display mode.
    {
        FramePacingInputs inputs = fixed(144.0, PresentMode::Fifo);
        inputs.display_refresh_hz = 60.0f;
        check(nearly(resolve_frame_pacing(PresentationSettings{}, inputs).refresh_duration, 1.0 / 144.0),
              "presentation-engine refresh takes priority over the window display mode");
        inputs.refresh_duration_seconds.reset();
        check(std::abs(resolve_frame_pacing(PresentationSettings{}, inputs).refresh_duration - 1.0 / 60.0) < 1e-6,
              "the window display mode is the fallback");
    }

    // Latency modes.
    {
        PresentationSettings settings{};
        check(resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo)).max_queued_presents == 0,
              "Normal latency adds no queue bound");
        settings.latency = LatencyMode::Low;
        check(resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo)).max_queued_presents == 1,
              "Low latency bounds the present queue to one");
        settings.latency = LatencyMode::Ultra;
        const FramePacingPlan ultra = resolve_frame_pacing(settings, fixed(144.0, PresentMode::Fifo));
        check(ultra.max_queued_presents == 1 && ultra.just_in_time_start, "Ultra latency starts frames just in time");
        check(!resolve_frame_pacing(settings, fixed(144.0, PresentMode::Mailbox)).just_in_time_start,
              "just-in-time needs a clock; an unpaced Mailbox loop has no slot to aim for");
    }

    // The unfocused throttle still snaps to an even cadence under vsync (144/15 = 9.6 -> 10 refreshes = 14.4 fps).
    {
        PresentationSettings settings{};
        settings.unfocused_frame_rate_limit_fps = 15.0;
        const FramePacingPlan plan =
            resolve_frame_pacing(settings, FramePacingInputs{.focused = false,
                                                             .refresh_duration_seconds = 1.0 / 144.0,
                                                             .variable_refresh_active = false,
                                                             .effective_present_mode = PresentMode::Fifo});
        check(plan.clock == FramePacingClock::CpuTimer && std::abs(plan.cpu_target_fps - 14.4) < 1e-9,
              "the unfocused throttle is rounded to an even cadence too");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "FrameRateLimitTest passed\n";
    return 0;
}
