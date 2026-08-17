#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cmath>
#include <numbers>
#pragma endregion

#include "Style.hpp"

/// A bundle of standard named curves (the usual "Penner easing" set) matching UI::EasingFn's
/// signature (Style.hpp) — assign one to any style's `.easing` field, or call one directly. Every
/// function here maps [0,1] -> roughly [0,1] (back/elastic intentionally overshoot outside that
/// range on the way to 1.0 — see EasingFn's own doc comment for why that's allowed on purpose).
///
/// These are plain functions specifically so `EasingFn` can stay a raw function pointer (no capture,
/// no allocation, trivially copyable into a style struct's default member initializer) rather than
/// std::function — every curve here is a pure, stateless expression of `t`, so nothing is lost by
/// that restriction. A caller that needs a parameterized curve (e.g. a custom overshoot amount) just
/// writes their own free function with the same signature.
namespace SFT::UI::Easing {

    [[nodiscard]] f32 linear(f32 t) noexcept;

    [[nodiscard]] f32 quad_in(f32 t) noexcept;
    [[nodiscard]] f32 quad_out(f32 t) noexcept;
    [[nodiscard]] f32 quad_in_out(f32 t) noexcept;

    [[nodiscard]] f32 cubic_in(f32 t) noexcept;
    [[nodiscard]] f32 cubic_out(f32 t) noexcept;
    [[nodiscard]] f32 cubic_in_out(f32 t) noexcept;

    [[nodiscard]] f32 quart_in(f32 t) noexcept;
    [[nodiscard]] f32 quart_out(f32 t) noexcept;
    [[nodiscard]] f32 quart_in_out(f32 t) noexcept;

    [[nodiscard]] f32 quint_in(f32 t) noexcept;
    [[nodiscard]] f32 quint_out(f32 t) noexcept;
    [[nodiscard]] f32 quint_in_out(f32 t) noexcept;

    [[nodiscard]] f32 sine_in(f32 t) noexcept;
    [[nodiscard]] f32 sine_out(f32 t) noexcept;
    [[nodiscard]] f32 sine_in_out(f32 t) noexcept;

    [[nodiscard]] f32 expo_in(f32 t) noexcept;
    [[nodiscard]] f32 expo_out(f32 t) noexcept;
    [[nodiscard]] f32 expo_in_out(f32 t) noexcept;

    [[nodiscard]] f32 circ_in(f32 t) noexcept;
    [[nodiscard]] f32 circ_out(f32 t) noexcept;
    [[nodiscard]] f32 circ_in_out(f32 t) noexcept;

    /// Overshoots past 1.0 before settling (a slight "wind-up" past the target) — see the file doc
    /// comment on why EasingFn deliberately doesn't clamp its output.
    [[nodiscard]] f32 back_in(f32 t) noexcept;
    [[nodiscard]] f32 back_out(f32 t) noexcept;
    [[nodiscard]] f32 back_in_out(f32 t) noexcept;

    /// Springy overshoot/oscillation — the most dramatic curves here, good for playful pop-in
    /// effects, not recommended for frequent state churn (hover/press) since the oscillation reads
    /// as jitter at high transition frequency.
    [[nodiscard]] f32 elastic_in(f32 t) noexcept;
    [[nodiscard]] f32 elastic_out(f32 t) noexcept;
    [[nodiscard]] f32 elastic_in_out(f32 t) noexcept;

    namespace Detail {
        [[nodiscard]] f32 bounce_out(f32 t) noexcept;
    } // namespace Detail

    [[nodiscard]] f32 bounce_out(f32 t) noexcept;
    [[nodiscard]] f32 bounce_in(f32 t) noexcept;
    [[nodiscard]] f32 bounce_in_out(f32 t) noexcept;

} // namespace SFT::UI::Easing
