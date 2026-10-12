#include "sturdy_rs/rhi_util.hpp"

#include <string_view>
#include <vector>

#include <RHI/RHI.hpp>
#include <RHI/Selection.hpp>
#include <RHI/Swapchain.hpp>
#include <RHI/Threading.hpp>

// Generated from src/rhi_util.rs.
#include "sturdy_rs/rhi_util_bridge.h"

namespace sturdy_rs::rhi_util {
    namespace {

        namespace R = SFT::RHI;

        R::FeatureSet to_set(const rust::Vec<rust::String> &names) {
            R::FeatureSet set;
            for (const rust::String &name : names) {
                for (std::size_t i = 0; i < R::feature_count; ++i) {
                    const char *candidate = R::feature_name(static_cast<R::Feature>(i));
                    if (candidate != nullptr && std::string_view{candidate} == std::string_view{name.data(), name.size()}) {
                        set.set(static_cast<R::Feature>(i));
                        break;
                    }
                }
            }
            return set;
        }

        rust::Vec<rust::String> to_names(const R::FeatureSet &set) {
            rust::Vec<rust::String> out;
            set.for_each([&](R::Feature feature) {
                const char *name = R::feature_name(feature);
                out.push_back(rust::String::lossy(name != nullptr ? name : ""));
            });
            return out;
        }

    } // namespace

    std::uint8_t format_flags(std::uint8_t format) noexcept {
        const auto f = static_cast<R::Format>(format);
        return static_cast<std::uint8_t>((R::format_is_block_compressed(f) ? 1u : 0u) | (R::format_has_depth(f) ? 2u : 0u) |
                                         (R::format_has_stencil(f) ? 4u : 0u));
    }

    NegotiationDesc negotiate_features(const rust::Vec<rust::String> &supported, const rust::Vec<rust::String> &required,
                                       const rust::Vec<rust::String> &optional) {
        const R::FeatureNegotiationReport report =
            R::negotiate_features(to_set(supported), to_set(required), to_set(optional));
        NegotiationDesc out{};
        out.supported = to_names(report.supported_features);
        out.enabled_required = to_names(report.enabled_required_features);
        out.enabled_optional = to_names(report.enabled_optional_features);
        out.missing_required = to_names(report.missing_required_features);
        out.unavailable_optional = to_names(report.unavailable_optional_features);
        return out;
    }

    std::int64_t score_adapter(std::uint8_t device_type, std::uint8_t power) noexcept {
        return R::score_adapter(static_cast<R::DeviceType>(device_type), static_cast<R::PowerPreference>(power));
    }

    bool name_contains_ci(rust::Str haystack, rust::Str needle) noexcept {
        return R::name_contains_ci(std::string_view{haystack.data(), haystack.size()},
                                   std::string_view{needle.data(), needle.size()});
    }

    std::uint8_t choose_render_threading_mode(bool dedicated, bool parallel, bool platform_threads) noexcept {
        R::RenderThreadingCapabilities caps{};
        caps.backend_allows_dedicated_render_thread = dedicated;
        caps.backend_allows_parallel_command_recording = parallel;
        caps.platform_allows_threads = platform_threads;
        return static_cast<std::uint8_t>(R::choose_render_threading_mode(caps));
    }

    std::array<std::uint8_t, 4> present_mode_preference(std::uint8_t strategy) noexcept {
        const auto modes = R::present_mode_preference(static_cast<R::PresentStrategy>(strategy));
        return {static_cast<std::uint8_t>(modes[0]), static_cast<std::uint8_t>(modes[1]),
                static_cast<std::uint8_t>(modes[2]), static_cast<std::uint8_t>(modes[3])};
    }

    std::uint8_t choose_present_mode(rust::Slice<const std::uint8_t> supported, std::uint8_t strategy) noexcept {
        std::vector<R::PresentMode> modes;
        for (std::uint8_t m : supported) {
            modes.push_back(static_cast<R::PresentMode>(m));
        }
        return static_cast<std::uint8_t>(
            R::choose_present_mode(std::span<const R::PresentMode>{modes}, static_cast<R::PresentStrategy>(strategy)));
    }

    rust::String present_mode_name(std::uint8_t mode) {
        return rust::String(std::string{R::present_mode_name(static_cast<R::PresentMode>(mode))});
    }
    rust::String present_strategy_name(std::uint8_t strategy) {
        return rust::String(std::string{R::present_strategy_name(static_cast<R::PresentStrategy>(strategy))});
    }

    std::array<std::uint8_t, 3> transparent_composite_alpha_preference() noexcept {
        const auto modes = R::transparent_composite_alpha_preference();
        return {static_cast<std::uint8_t>(modes[0]), static_cast<std::uint8_t>(modes[1]), static_cast<std::uint8_t>(modes[2])};
    }

} // namespace sturdy_rs::rhi_util
