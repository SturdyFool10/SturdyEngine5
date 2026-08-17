#include <Core/src/Core/Renderer.hpp>


namespace SFT::Core {

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

} // namespace SFT::Core

