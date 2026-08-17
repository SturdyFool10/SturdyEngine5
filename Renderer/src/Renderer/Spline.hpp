#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <Text/Text.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {


    template <typename Vec>
    class CatmullRomSpline {
      public:
        /// Constructs a `CatmullRomSpline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        CatmullRomSpline() noexcept = default;


        /// Creates a `CatmullRomSpline` resource or value from the supplied parameters.
        ///
        /// @param waypoints `waypoints` value used by the operation.
        /// @param samples_per_segment `samples_per_segment` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static CatmullRomSpline create(vector<Vec> waypoints, u32 samples_per_segment = 32) {
            CatmullRomSpline spline;
            if (waypoints.size() < 2) {
                return spline;
            }
            spline.points_.reserve(waypoints.size() + 2);
            spline.points_.push_back(waypoints.front() - (waypoints[1] - waypoints.front()));
            spline.points_.insert(spline.points_.end(), waypoints.begin(), waypoints.end());
            const Vec &last = waypoints.back();
            const Vec &second_last = waypoints[waypoints.size() - 2];
            spline.points_.push_back(last + (last - second_last));

            spline.segment_count_ = static_cast<u32>(waypoints.size() - 1);
            spline.build_arc_length_table(std::max(samples_per_segment, 2u));
            return spline;
        }

        /// Returns the current or globally available valid value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool valid() const noexcept { return segment_count_ > 0; }


        /// Returns the segment count for this `CatmullRomSpline`.
        ///
        /// @return Returns the current segment count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 segment_count() const noexcept { return segment_count_; }

        /// Performs the position operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param t `t` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Vec position(f32 t) const noexcept {
            if (!valid()) {
                return Vec{};
            }
            t = std::clamp(t, 0.0f, static_cast<f32>(segment_count_));
            u32 segment = static_cast<u32>(t);
            if (segment >= segment_count_) {
                segment = segment_count_ - 1;
            }
            const f32 local = t - static_cast<f32>(segment);
            return evaluate_segment(segment, local);
        }


        /// Performs the tangent operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param t `t` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Vec tangent(f32 t) const noexcept {
            constexpr f32 epsilon = 1.0e-3f;
            const f32 max_t = static_cast<f32>(segment_count_);
            const f32 t0 = std::clamp(t - epsilon, 0.0f, max_t);
            const f32 t1 = std::clamp(t + epsilon, 0.0f, max_t);
            if (t1 - t0 <= 0.0f) {
                return Vec{};
            }
            return (position(t1) - position(t0)) / (t1 - t0);
        }

        /// Returns the current or globally available total length value.
        ///
        /// @return Returns the current total length value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 total_length() const noexcept { return arc_length_table_.empty() ? 0.0f : arc_length_table_.back().length; }


        /// Performs the t at arc length operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param arc_length `arc_length` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 t_at_arc_length(f32 arc_length) const noexcept {
            if (arc_length_table_.empty()) {
                return 0.0f;
            }
            if (arc_length <= 0.0f) {
                return arc_length_table_.front().t;
            }
            if (arc_length >= total_length()) {
                return arc_length_table_.back().t;
            }
            usize lo = 0;
            usize hi = arc_length_table_.size() - 1;
            while (lo + 1 < hi) {
                const usize mid = (lo + hi) / 2;
                if (arc_length_table_[mid].length <= arc_length) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            const Sample &a = arc_length_table_[lo];
            const Sample &b = arc_length_table_[hi];
            const f32 span_length = b.length - a.length;
            const f32 fraction = span_length > 0.0f ? (arc_length - a.length) / span_length : 0.0f;
            return a.t + (b.t - a.t) * fraction;
        }

      private:
        struct Sample {
            f32 t = 0.0f;
            f32 length = 0.0f;
        };

        /// Performs the evaluate segment operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param segment `segment` value used by the operation.
        /// @param local `local` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Vec evaluate_segment(u32 segment, f32 local) const noexcept {
            const Vec &p0 = points_[segment];
            const Vec &p1 = points_[segment + 1];
            const Vec &p2 = points_[segment + 2];
            const Vec &p3 = points_[segment + 3];

            constexpr f32 alpha = 0.5f;
            const f32 t0 = 0.0f;
            const f32 t1 = t0 + knot_delta(p0, p1, alpha);
            const f32 t2 = t1 + knot_delta(p1, p2, alpha);
            const f32 t3 = t2 + knot_delta(p2, p3, alpha);
            const f32 t = t1 + (t2 - t1) * local;

            return barry_goldman(p0, p1, p2, p3, t0, t1, t2, t3, t);
        }

        /// Performs the knot delta operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param a `a` value used by the operation.
        /// @param b `b` value used by the operation.
        /// @param alpha `alpha` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static f32 knot_delta(const Vec &a, const Vec &b, f32 alpha) noexcept {
            const f32 distance = glm::length(b - a);
            return distance > 0.0f ? std::pow(distance, alpha) : 1.0e-4f;
        }

        /// Performs the lerp knots operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param a `a` value used by the operation.
        /// @param b `b` value used by the operation.
        /// @param ta `ta` value used by the operation.
        /// @param tb `tb` value used by the operation.
        /// @param t `t` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static Vec lerp_knots(const Vec &a, const Vec &b, f32 ta, f32 tb, f32 t) noexcept {
            const f32 span_length = tb - ta;
            if (span_length <= 0.0f) {
                return a;
            }
            return a * ((tb - t) / span_length) + b * ((t - ta) / span_length);
        }

        /// Performs the barry goldman operation for `CatmullRomSpline` using the supplied arguments.
        ///
        /// @param p0 `p0` value used by the operation.
        /// @param p1 `p1` value used by the operation.
        /// @param p2 `p2` value used by the operation.
        /// @param p3 `p3` value used by the operation.
        /// @param t0 `t0` value used by the operation.
        /// @param t1 `t1` value used by the operation.
        /// @param t2 `t2` value used by the operation.
        /// @param t3 `t3` value used by the operation.
        /// @param t `t` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static Vec barry_goldman(const Vec &p0, const Vec &p1, const Vec &p2, const Vec &p3, f32 t0, f32 t1,
                                               f32 t2, f32 t3, f32 t) noexcept {
            const Vec a1 = lerp_knots(p0, p1, t0, t1, t);
            const Vec a2 = lerp_knots(p1, p2, t1, t2, t);
            const Vec a3 = lerp_knots(p2, p3, t2, t3, t);
            const Vec b1 = lerp_knots(a1, a2, t0, t2, t);
            const Vec b2 = lerp_knots(a2, a3, t1, t3, t);
            return lerp_knots(b1, b2, t1, t2, t);
        }

        /// Builds arc length table.
        ///
        /// @param samples_per_segment `samples_per_segment` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_arc_length_table(u32 samples_per_segment) {
            arc_length_table_.clear();
            if (!valid()) {
                return;
            }
            const u32 total_samples = segment_count_ * samples_per_segment + 1;
            arc_length_table_.reserve(total_samples);

            f32 accumulated = 0.0f;
            Vec previous = position(0.0f);
            arc_length_table_.push_back(Sample{.t = 0.0f, .length = 0.0f});
            for (u32 i = 1; i < total_samples; ++i) {
                const f32 t = static_cast<f32>(segment_count_) * static_cast<f32>(i) / static_cast<f32>(total_samples - 1);
                const Vec current = position(t);
                accumulated += glm::length(current - previous);
                arc_length_table_.push_back(Sample{.t = t, .length = accumulated});
                previous = current;
            }
        }

        vector<Vec> points_;
        u32 segment_count_ = 0;
        vector<Sample> arc_length_table_;
    };

    using Spline2D = CatmullRomSpline<glm::vec2>;
    using Spline3D = CatmullRomSpline<glm::vec3>;


    struct GlyphPathPlacement2D {
        glm::vec2 position{0.0f};
        f32 rotation = 0.0f;
    };


    /// Performs the layout text on spline 2d operation using the supplied arguments.
    ///
    /// @param spline `spline` value used by the operation.
    /// @param glyphs `glyphs` value used by the operation.
    /// @param units_per_em `units_per_em` value used by the operation.
    /// @param pixel_size Requested or available size for the operation.
    /// @param start_offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<GlyphPathPlacement2D> layout_text_on_spline_2d(const Spline2D &spline,
                                                                               span<const Text::PositionedGlyph> glyphs,
                                                                               u32 units_per_em, f32 pixel_size,
                                                                               f32 start_offset = 0.0f);


    /// Performs the layout text on spline 3d operation for `Renderer` using the supplied arguments.
    ///
    /// @param spline `spline` value used by the operation.
    /// @param glyphs `glyphs` value used by the operation.
    /// @param units_per_em `units_per_em` value used by the operation.
    /// @param pixel_size Requested or available size for the operation.
    /// @param up_hint `up_hint` value used by the operation.
    /// @param start_offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<glm::mat4> layout_text_on_spline_3d(const Spline3D &spline,
                                                                    span<const Text::PositionedGlyph> glyphs,
                                                                    u32 units_per_em, f32 pixel_size, glm::vec3 up_hint,
                                                                    f32 start_offset = 0.0f);

} // namespace SFT::Renderer
