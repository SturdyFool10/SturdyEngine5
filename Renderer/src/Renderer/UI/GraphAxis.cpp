#include <Renderer/UI/GraphAxis.hpp>

#pragma region Imports
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#pragma endregion

namespace SFT::UI {

    namespace {

        /// Rounds `range` to a "nice" number (1, 2, or 5 times a power of ten) — the classic tick-step
        /// selection algorithm shared by every linear-region tick generator below.
        ///
        /// @param range Requested or available size for the operation.
        /// @param round Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 nice_number(f64 range, bool round) noexcept {
            if (range <= 0.0 || !std::isfinite(range)) {
                return 1.0;
            }
            const f64 exponent = std::floor(std::log10(range));
            const f64 fraction = range / std::pow(10.0, exponent);
            f64 nice_fraction;
            if (round) {
                if (fraction < 1.5) {
                    nice_fraction = 1.0;
                } else if (fraction < 3.0) {
                    nice_fraction = 2.0;
                } else if (fraction < 7.0) {
                    nice_fraction = 5.0;
                } else {
                    nice_fraction = 10.0;
                }
            } else {
                if (fraction <= 1.0) {
                    nice_fraction = 1.0;
                } else if (fraction <= 2.0) {
                    nice_fraction = 2.0;
                } else if (fraction <= 5.0) {
                    nice_fraction = 5.0;
                } else {
                    nice_fraction = 10.0;
                }
            }
            return nice_fraction * std::pow(10.0, exponent);
        }

        /// Generates linear "nice number" ticks across `[lo, hi]`, appended to `out`.
        ///
        /// @param lo Requested or available size for the operation.
        /// @param hi Requested or available size for the operation.
        /// @param target_tick_count Requested or available size for the operation.
        /// @param axis `axis` value used by the operation.
        /// @param out `out` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void append_linear_ticks(f64 lo, f64 hi, u32 target_tick_count, const AxisConfig &axis,
                                 vector<TickSpec> &out);

        /// Formats one tick's label, using `axis.tick_label_formatter` when set.
        ///
        /// @param axis `axis` value used by the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string format_tick_label(const AxisConfig &axis, f64 value) {
            if (axis.tick_label_formatter) {
                return axis.tick_label_formatter(value);
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value);
            return string{buffer};
        }

        void append_linear_ticks(f64 lo, f64 hi, u32 target_tick_count, const AxisConfig &axis,
                                 vector<TickSpec> &out) {
            if (!(hi > lo)) {
                out.push_back(TickSpec{.value = lo, .label = format_tick_label(axis, lo)});
                return;
            }
            const f64 raw_range = nice_number(hi - lo, false);
            const f64 step = nice_number(raw_range / std::max<f64>(1.0, static_cast<f64>(target_tick_count) - 1.0), true);
            if (step <= 1e-12) {
                out.push_back(TickSpec{.value = lo, .label = format_tick_label(axis, lo)});
                return;
            }
            const f64 start = std::floor(lo / step) * step;
            const f64 end = std::ceil(hi / step) * step;
            for (f64 v = start; v <= end + step * 0.5; v += step) {
                if (v < lo - step * 0.5 || v > hi + step * 0.5) {
                    continue;
                }
                out.push_back(TickSpec{.value = v, .label = format_tick_label(axis, v)});
            }
        }

        /// Continuous, monotonic forward transform for `ScaleKind::Symlog`: linear within
        /// `[-threshold, threshold]` (mapping to `[-1, 1]`), logarithmic beyond it on either side, with
        /// `|value| == threshold` landing on `±1` from both branches so the piecewise function has no
        /// discontinuity.
        ///
        /// @param value Value consumed by the operation.
        /// @param threshold `threshold` value used by the operation.
        /// @param base `base` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 symlog_forward(f64 value, f64 threshold, f64 base) noexcept {
            if (std::abs(value) <= threshold) {
                return value / threshold;
            }
            const f64 sign = value < 0.0 ? -1.0 : 1.0;
            return sign * (1.0 + std::log(std::abs(value) / threshold) / std::log(base));
        }

        /// Inverse of symlog_forward().
        ///
        /// @param u Value consumed by the operation.
        /// @param threshold `threshold` value used by the operation.
        /// @param base `base` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 symlog_inverse(f64 u, f64 threshold, f64 base) noexcept {
            if (std::abs(u) <= 1.0) {
                return u * threshold;
            }
            const f64 sign = u < 0.0 ? -1.0 : 1.0;
            return sign * threshold * std::exp((std::abs(u) - 1.0) * std::log(base));
        }

    } // namespace

    /// Maps a data-space value to a normalized position for `ScaleTransform` using the supplied
    /// arguments.
    ///
    /// @param value Value consumed by the operation.
    /// @param domain_min Requested or available size for the operation.
    /// @param domain_max Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 ScaleTransform::to_normalized(f64 value, f64 domain_min, f64 domain_max) const noexcept {
        switch (kind) {
            case ScaleKind::Log: {
                const f64 log_min = std::log(std::max(domain_min, 1e-300));
                const f64 log_max = std::log(std::max(domain_max, 1e-300));
                const f64 log_v = std::log(std::max(value, 1e-300));
                const f64 span = log_max - log_min;
                return std::abs(span) > 1e-300 ? (log_v - log_min) / span : 0.0;
            }
            case ScaleKind::Symlog: {
                const f64 threshold = std::max(symlog_linear_threshold, 1e-12);
                const f64 base = log_base > 1.0 ? log_base : 10.0;
                const f64 u_min = symlog_forward(domain_min, threshold, base);
                const f64 u_max = symlog_forward(domain_max, threshold, base);
                const f64 span = u_max - u_min;
                return std::abs(span) > 1e-300 ? (symlog_forward(value, threshold, base) - u_min) / span : 0.0;
            }
            case ScaleKind::Custom: {
                if (!custom_forward) {
                    break;
                }
                const f64 t_min = custom_forward(domain_min);
                const f64 t_max = custom_forward(domain_max);
                const f64 span = t_max - t_min;
                return std::abs(span) > 1e-300 ? (custom_forward(value) - t_min) / span : 0.0;
            }
            case ScaleKind::Linear:
            default:
                break;
        }
        const f64 span = domain_max - domain_min;
        return std::abs(span) > 1e-300 ? (value - domain_min) / span : 0.0;
    }

    /// Maps a normalized position to a data-space value for `ScaleTransform` using the supplied
    /// arguments.
    ///
    /// @param t Value consumed by the operation.
    /// @param domain_min Requested or available size for the operation.
    /// @param domain_max Requested or available size for the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 ScaleTransform::from_normalized(f64 t, f64 domain_min, f64 domain_max) const noexcept {
        switch (kind) {
            case ScaleKind::Log: {
                const f64 log_min = std::log(std::max(domain_min, 1e-300));
                const f64 log_max = std::log(std::max(domain_max, 1e-300));
                return std::exp(log_min + t * (log_max - log_min));
            }
            case ScaleKind::Symlog: {
                const f64 threshold = std::max(symlog_linear_threshold, 1e-12);
                const f64 base = log_base > 1.0 ? log_base : 10.0;
                const f64 u_min = symlog_forward(domain_min, threshold, base);
                const f64 u_max = symlog_forward(domain_max, threshold, base);
                return symlog_inverse(u_min + t * (u_max - u_min), threshold, base);
            }
            case ScaleKind::Custom: {
                if (!custom_inverse || !custom_forward) {
                    break;
                }
                const f64 t_min = custom_forward(domain_min);
                const f64 t_max = custom_forward(domain_max);
                return custom_inverse(t_min + t * (t_max - t_min));
            }
            case ScaleKind::Linear:
            default:
                break;
        }
        return domain_min + t * (domain_max - domain_min);
    }

    /// Resolves an axis's effective domain from the supplied parameters.
    ///
    /// @param axis `axis` value used by the operation.
    /// @param values Data-space values (e.g. every bound series' samples for this axis) to autoscale from.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    AxisRange resolve_axis_range(const AxisConfig &axis, std::span<const f64> values) noexcept {
        f64 raw_min = 0.0;
        f64 raw_max = 1.0;
        f64 smallest_positive = 1.0;
        bool have_positive = false;
        if (!values.empty()) {
            raw_min = values[0];
            raw_max = values[0];
            for (const f64 v : values) {
                if (!std::isfinite(v)) {
                    continue;
                }
                raw_min = std::min(raw_min, v);
                raw_max = std::max(raw_max, v);
                if (v > 0.0 && (!have_positive || v < smallest_positive)) {
                    smallest_positive = v;
                    have_positive = true;
                }
            }
        }

        if (axis.scale.kind == ScaleKind::Log) {
            // A log axis has no representation for non-positive values. Padding the raw range
            // additively (the plain path below) can easily push a small-but-positive minimum below
            // zero — e.g. autoscaling [1, 148] with 5% linear padding gives a resolved minimum of
            // -6.35 — which every consumer of this range (ScaleTransform::to_normalized/
            // from_normalized) would then clamp to a near-zero epsilon, collapsing the entire mapping
            // toward t=1 instead of actually failing loudly. Clamp to strictly-positive data first,
            // then pad multiplicatively in log-space so the padded minimum stays positive too.
            const f64 positive_min = have_positive ? smallest_positive : std::max(raw_max, 1e-6);
            const f64 positive_max = std::max(raw_max, positive_min * 10.0);
            f64 resolved_min = axis.min.has_value() ? std::max(*axis.min, 1e-300) : positive_min;
            f64 resolved_max = axis.max.has_value() ? std::max(*axis.max, resolved_min * (1.0 + 1e-6)) : positive_max;
            if (!axis.min.has_value() || !axis.max.has_value()) {
                const f64 log_min = std::log(resolved_min);
                const f64 log_max = std::log(std::max(resolved_max, resolved_min * (1.0 + 1e-6)));
                const f64 log_pad = std::max(log_max - log_min, 1e-6) * static_cast<f64>(axis.autoscale_padding_percent);
                if (!axis.min.has_value()) {
                    resolved_min = std::exp(log_min - log_pad);
                }
                if (!axis.max.has_value()) {
                    resolved_max = std::exp(log_max + log_pad);
                }
            }
            if (!(resolved_max > resolved_min)) {
                resolved_max = resolved_min * 10.0;
            }
            return AxisRange{.min = resolved_min, .max = resolved_max};
        }

        f64 resolved_min = axis.min.value_or(raw_min);
        f64 resolved_max = axis.max.value_or(raw_max);
        if (!axis.min.has_value() || !axis.max.has_value()) {
            const f64 span = std::max(resolved_max - resolved_min, 1e-9);
            const f64 pad = span * static_cast<f64>(axis.autoscale_padding_percent);
            if (!axis.min.has_value()) {
                resolved_min -= pad;
            }
            if (!axis.max.has_value()) {
                resolved_max += pad;
            }
        }
        if (!(resolved_max > resolved_min)) {
            resolved_max = resolved_min + 1.0;
        }
        return AxisRange{.min = resolved_min, .max = resolved_max};
    }

    /// Generates ticks for `AxisConfig` using the supplied arguments.
    ///
    /// @param axis `axis` value used by the operation.
    /// @param resolved_min Resolved axis minimum, from resolve_axis_range().
    /// @param resolved_max Resolved axis maximum, from resolve_axis_range().
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<TickSpec> generate_ticks(const AxisConfig &axis, f64 resolved_min, f64 resolved_max) {
        vector<TickSpec> ticks;

        if (axis.is_categorical) {
            ticks.reserve(axis.categories.size());
            for (usize i = 0; i < axis.categories.size(); ++i) {
                ticks.push_back(TickSpec{.value = static_cast<f64>(i), .label = axis.categories[i]});
            }
            return ticks;
        }

        if (!(resolved_max > resolved_min)) {
            ticks.push_back(TickSpec{.value = resolved_min, .label = format_tick_label(axis, resolved_min)});
            return ticks;
        }

        switch (axis.scale.kind) {
            case ScaleKind::Log: {
                if (resolved_min <= 0.0 || resolved_max <= 0.0) {
                    // A log axis has no representation for zero/negative values — fail loudly (a
                    // single degenerate tick) rather than silently placing ticks at nonsense positions.
                    ticks.push_back(TickSpec{.value = resolved_min, .label = format_tick_label(axis, resolved_min)});
                    break;
                }
                const f64 base = axis.scale.log_base > 1.0 ? axis.scale.log_base : 10.0;
                const f64 log_base_val = std::log(base);
                const auto first_decade = static_cast<i64>(std::floor(std::log(resolved_min) / log_base_val));
                const auto last_decade = static_cast<i64>(std::ceil(std::log(resolved_max) / log_base_val));
                for (i64 decade = first_decade; decade <= last_decade; ++decade) {
                    const f64 major_value = std::pow(base, static_cast<f64>(decade));
                    if (major_value >= resolved_min * 0.9999 && major_value <= resolved_max * 1.0001) {
                        ticks.push_back(TickSpec{.value = major_value, .label = format_tick_label(axis, major_value)});
                    }
                    if (axis.show_minor_gridlines) {
                        for (f64 mult = 2.0; mult < base; mult += 1.0) {
                            const f64 minor_value = major_value * mult;
                            if (minor_value >= resolved_min && minor_value <= resolved_max) {
                                ticks.push_back(TickSpec{.value = minor_value, .is_minor = true});
                            }
                        }
                    }
                }
                break;
            }
            case ScaleKind::Symlog: {
                const f64 threshold = std::max(axis.scale.symlog_linear_threshold, 1e-12);
                const f64 base = axis.scale.log_base > 1.0 ? axis.scale.log_base : 10.0;
                const f64 log_base_val = std::log(base);

                const f64 lin_lo = std::max(resolved_min, -threshold);
                const f64 lin_hi = std::min(resolved_max, threshold);
                if (lin_hi > lin_lo) {
                    append_linear_ticks(lin_lo, lin_hi, axis.target_tick_count, axis, ticks);
                }

                for (const f64 sign : {1.0, -1.0}) {
                    const f64 outer_bound = sign > 0.0 ? resolved_max : resolved_min;
                    if (sign * outer_bound < threshold) {
                        continue; // this side never leaves the linear region
                    }
                    const f64 outer_abs = std::abs(outer_bound);
                    const auto first_decade = static_cast<i64>(std::floor(std::log(threshold) / log_base_val));
                    const auto last_decade = static_cast<i64>(std::ceil(std::log(outer_abs) / log_base_val));
                    for (i64 decade = first_decade; decade <= last_decade; ++decade) {
                        const f64 value = sign * std::pow(base, static_cast<f64>(decade));
                        if (value >= resolved_min && value <= resolved_max) {
                            ticks.push_back(TickSpec{.value = value, .label = format_tick_label(axis, value)});
                        }
                    }
                }
                std::sort(ticks.begin(), ticks.end(),
                         [](const TickSpec &a, const TickSpec &b) noexcept { return a.value < b.value; });
                break;
            }
            case ScaleKind::Custom: {
                if (!axis.scale.custom_inverse || !axis.scale.custom_forward) {
                    append_linear_ticks(resolved_min, resolved_max, axis.target_tick_count, axis, ticks);
                    break;
                }
#ifndef NDEBUG
                {
                    // Spot-check that custom_forward/custom_inverse actually round-trip — a
                    // non-invertible pair silently produces wrong tick positions rather than a crash,
                    // per this function's own doc comment.
                    const f64 probe = axis.scale.custom_forward(resolved_min);
                    const f64 round_tripped = axis.scale.custom_inverse(probe);
                    assert(std::abs(round_tripped - resolved_min) <=
                          std::max(1e-6, std::abs(resolved_min) * 1e-6) &&
                          "AxisConfig::scale.custom_forward/custom_inverse must be exact inverses");
                }
#endif
                const u32 count = std::max<u32>(2, axis.target_tick_count);
                for (u32 i = 0; i < count; ++i) {
                    const f64 t = static_cast<f64>(i) / static_cast<f64>(count - 1);
                    const f64 value = axis.scale.from_normalized(t, resolved_min, resolved_max);
                    ticks.push_back(TickSpec{.value = value, .label = format_tick_label(axis, value)});
                }
                break;
            }
            case ScaleKind::Linear:
            default:
                append_linear_ticks(resolved_min, resolved_max, axis.target_tick_count, axis, ticks);
                break;
        }
        return ticks;
    }

} // namespace SFT::UI
