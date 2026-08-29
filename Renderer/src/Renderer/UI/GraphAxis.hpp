#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/UiStroke.hpp>

using std::optional;
using std::string;
using std::vector;

// Plain data + free functions behind the graph widget's axis handling (Renderer/UI/Graph.hpp) —
// deliberately Clay-free, mirroring UiQuad.hpp/CustomElement.hpp's own separation, and independent of
// any particular chart type: every 2D chart type shares this same axis/scale/tick machinery.
namespace SFT::UI {

    /// The mapping from data-space values to a normalized [0,1] axis position. `Custom` lets a caller
    /// supply an arbitrary monotonic transform (e.g. sqrt, a perceptual/decibel scale) via
    /// `custom_forward`/`custom_inverse`; the built-in kinds cover the common cases without needing
    /// one.
    enum class ScaleKind : u8 { Linear, Log, Symlog, Custom };

    struct ScaleTransform {
        ScaleKind kind = ScaleKind::Linear;

        /// Logarithm base for `Log`/`Symlog`. Only the ratio between decades matters for tick
        /// placement/spacing, so this is rarely anything but 10.
        f64 log_base = 10.0;

        /// `Symlog` only: the domain stays linear within `[-symlog_linear_threshold,
        /// +symlog_linear_threshold]` (so a value can cross zero without `log(0)`), and switches to a
        /// log scale beyond it on both signs. Matplotlib's `symlog` scale uses the same idea.
        f64 symlog_linear_threshold = 1.0;

        /// `Custom` only: caller-supplied forward (data -> monotonic transformed space) and inverse
        /// transforms. Must be exact inverses of each other — `generate_ticks()` round-trips a few
        /// sample points through both and asserts in debug builds if they don't agree within a small
        /// tolerance, since a non-invertible pair silently produces wrong tick positions rather than a
        /// crash.
        std::function<f64(f64)> custom_forward;
        std::function<f64(f64)> custom_inverse;

        /// Maps a data-space `value` to a normalized position, given the resolved axis domain
        /// `[domain_min, domain_max]` (post-autoscale/padding — see `resolve_axis_range()`). The
        /// result is 0 at `domain_min` and 1 at `domain_max`; values outside the domain extrapolate
        /// linearly rather than clamping, so a caller can still plot a point slightly outside the
        /// visible range (e.g. for a marker that's meant to clip at the plot edge).
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 to_normalized(f64 value, f64 domain_min, f64 domain_max) const noexcept;

        /// Inverse of to_normalized(): maps a normalized position back to a data-space value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 from_normalized(f64 t, f64 domain_min, f64 domain_max) const noexcept;
    };

    struct TickSpec {
        f64 value = 0.0;
        string label;

        /// Minor ticks (e.g. the 2x..9x marks within a log decade) get a gridline but no label by
        /// default — see AxisConfig::show_minor_gridlines.
        bool is_minor = false;
    };

    struct AxisConfig {
        ScaleTransform scale{};

        /// Fixed axis bounds; `nullopt` means autoscale from whatever series values are bound to this
        /// axis (see `resolve_axis_range()`), padded by `autoscale_padding_percent`. Setting only one
        /// of the two pins that side while the other still autoscales.
        optional<f64> min;
        optional<f64> max;
        f32 autoscale_padding_percent = 0.05f;

        /// A hint, not an exact count — `generate_ticks()` picks whatever "nice" step lands closest to
        /// this many ticks for the active scale kind.
        u32 target_tick_count = 6;

        /// Overrides the default tick-label formatting (a plain "%g"-style rendering). Called once per
        /// generated tick.
        std::function<string(f64)> tick_label_formatter;

        bool show_gridlines = true;
        bool show_minor_gridlines = false;
        Color axis_color{0.45, 0.47, 0.53, 1.0};
        Color gridline_color{1.0, 1.0, 1.0, 0.06};
        Color minor_gridline_color{1.0, 1.0, 1.0, 0.03};
        StrokeStyle axis_stroke{.width = 1.0f, .snap_to_pixel_grid = true};
        StrokeStyle gridline_stroke{.width = 1.0f, .snap_to_pixel_grid = true};

        string title;

        /// A categorical axis (e.g. a bar chart's category axis) ignores `scale`/autoscale entirely —
        /// one evenly-spaced tick per `categories` entry, in order.
        bool is_categorical = false;
        vector<string> categories;
    };

    struct AxisRange {
        f64 min = 0.0;
        f64 max = 1.0;
    };

    /// Resolves an axis's effective `[min, max]` domain from its own fixed bounds (if set) and/or the
    /// data values bound to it, applying `autoscale_padding_percent` to whichever side(s) autoscale.
    /// Always returns `max > min` (a degenerate all-equal `values` span still produces a usable unit
    /// range) so callers never need to guard against a zero-width domain themselves.
    ///
    /// @param axis `axis` value used by the operation.
    /// @param values Data-space values (e.g. every bound series' samples for this axis) to autoscale from.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] AxisRange resolve_axis_range(const AxisConfig &axis, std::span<const f64> values) noexcept;

    /// Generates the tick marks for an axis already resolved to `[resolved_min, resolved_max]` (see
    /// `resolve_axis_range()`). For `ScaleKind::Log`, `resolved_min`/`resolved_max` must both be
    /// strictly positive (a log axis has no representation for zero or negative values) — a range that
    /// isn't produces a single tick at `resolved_min` rather than nonsense positions, matching the
    /// "fail loudly, not silently wrong" precedent elsewhere in this UI (e.g.
    /// UiCustomCommandKind/CustomShaderRef's push-constant-size validation).
    ///
    /// @param axis `axis` value used by the operation.
    /// @param resolved_min Resolved axis minimum, from resolve_axis_range().
    /// @param resolved_max Resolved axis maximum, from resolve_axis_range().
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] vector<TickSpec> generate_ticks(const AxisConfig &axis, f64 resolved_min, f64 resolved_max);

} // namespace SFT::UI
