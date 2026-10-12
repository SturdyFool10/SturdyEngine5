// Rust <-> SturdyEngine 5 shim for device-free RHI helpers. See src/rhi_util.rs.
#pragma once

#include <array>
#include <cstdint>

#include <rust/cxx.h>

namespace sturdy_rs::rhi_util {

    struct NegotiationDesc;

    std::uint8_t format_flags(std::uint8_t format) noexcept;
    NegotiationDesc negotiate_features(const rust::Vec<rust::String> &supported, const rust::Vec<rust::String> &required,
                                       const rust::Vec<rust::String> &optional);
    std::int64_t score_adapter(std::uint8_t device_type, std::uint8_t power) noexcept;
    bool name_contains_ci(rust::Str haystack, rust::Str needle) noexcept;
    std::uint8_t choose_render_threading_mode(bool dedicated, bool parallel, bool platform_threads) noexcept;
    std::array<std::uint8_t, 4> present_mode_preference(std::uint8_t strategy) noexcept;
    std::uint8_t choose_present_mode(rust::Slice<const std::uint8_t> supported, std::uint8_t strategy) noexcept;
    rust::String present_mode_name(std::uint8_t mode);
    rust::String present_strategy_name(std::uint8_t strategy);
    std::array<std::uint8_t, 3> transparent_composite_alpha_preference() noexcept;

} // namespace sturdy_rs::rhi_util
