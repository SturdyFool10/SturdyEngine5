#pragma once

#include <Foundation/Foundation.hpp>

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

    // A curve through caller-supplied waypoints — **centripetal Catmull-Rom**, not Bezier: the
    // curve interpolates *through* every waypoint (what "fit text to a path drawn through these
    // points" wants), and the centripetal parameterization (alpha = 0.5) avoids the
    // cusps/self-intersections uniform Catmull-Rom produces when waypoints are unevenly spaced.
    // Works in both `glm::vec2` and `glm::vec3` (see `Spline2D`/`Spline3D` below) — the Barry-Goldman
    // evaluation only needs vector +/-/scalar-* and `length()`, which both types provide.
    template <typename Vec>
    class CatmullRomSpline {
      public:
        CatmullRomSpline() noexcept = default;

        // `waypoints` needs at least 2 points. `samples_per_segment` controls the arc-length
        // table's resolution (used by t_at_arc_length() for even glyph spacing) — 32 is a good
        // default; raise it for very sharp/high-curvature paths.
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

        [[nodiscard]] bool valid() const noexcept { return segment_count_ > 0; }

        // `t` in [0, segment_count()] — segment index is floor(t), local position within the
        // segment is frac(t).
        [[nodiscard]] u32 segment_count() const noexcept { return segment_count_; }

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

        // Central-difference tangent (not normalized) — robust for any segment shape without
        // needing a closed-form derivative of the Barry-Goldman recursion.
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

        [[nodiscard]] f32 total_length() const noexcept { return arc_length_table_.empty() ? 0.0f : arc_length_table_.back().length; }

        // Maps an arc-length distance along the curve (from its start) to the equivalent `t` —
        // this is what makes glyph spacing along the path even by actual advance width instead of
        // bunching up wherever the raw parameter moves slower than the curve's real speed.
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

        [[nodiscard]] static f32 knot_delta(const Vec &a, const Vec &b, f32 alpha) noexcept {
            const f32 distance = glm::length(b - a);
            return distance > 0.0f ? std::pow(distance, alpha) : 1.0e-4f; // avoid a zero-length knot span
        }

        [[nodiscard]] static Vec lerp_knots(const Vec &a, const Vec &b, f32 ta, f32 tb, f32 t) noexcept {
            const f32 span_length = tb - ta;
            if (span_length <= 0.0f) {
                return a;
            }
            return a * ((tb - t) / span_length) + b * ((t - ta) / span_length);
        }

        [[nodiscard]] static Vec barry_goldman(const Vec &p0, const Vec &p1, const Vec &p2, const Vec &p3, f32 t0, f32 t1,
                                               f32 t2, f32 t3, f32 t) noexcept {
            const Vec a1 = lerp_knots(p0, p1, t0, t1, t);
            const Vec a2 = lerp_knots(p1, p2, t1, t2, t);
            const Vec a3 = lerp_knots(p2, p3, t2, t3, t);
            const Vec b1 = lerp_knots(a1, a2, t0, t2, t);
            const Vec b2 = lerp_knots(a2, a3, t1, t3, t);
            return lerp_knots(b1, b2, t1, t2, t);
        }

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

        vector<Vec> points_; // waypoints with one phantom point prepended and appended
        u32 segment_count_ = 0;
        vector<Sample> arc_length_table_;
    };

    using Spline2D = CatmullRomSpline<glm::vec2>;
    using Spline3D = CatmullRomSpline<glm::vec3>;

    // Where and how a glyph sits on a 2D path: `position` is the glyph's pen origin, `rotation`
    // (radians) is the path's tangent angle at that point — the caller applies this the same way
    // a rotated GlyphPlacement would be built for any other 2D text (copy both fields into the
    // placement; make_glyph_instance applies the rotation around its pen origin).
    struct GlyphPathPlacement2D {
        glm::vec2 position{0.0f};
        f32 rotation = 0.0f;
    };

    // Walks `spline`'s arc length by each glyph's advance width (scaled from font units to
    // pixels), placing glyph `i`'s pen origin at that point and rotating it to the path's tangent
    // there. `start_offset` shifts the whole run's starting arc-length position (e.g. to center
    // a string on the path, pass `-total_advance_px / 2`).
    [[nodiscard]] vector<GlyphPathPlacement2D> layout_text_on_spline_2d(const Spline2D &spline,
                                                                               span<const Text::PositionedGlyph> glyphs,
                                                                               u32 units_per_em, f32 pixel_size,
                                                                               f32 start_offset = 0.0f);

    // Same idea in full 3D: one world-space transform per glyph, built from a **rotation-minimizing
    // frame** (tangent + `up_hint` orthogonalized via Gram-Schmidt) rather than a raw Frenet-Serret
    // frame, which is known to flip its normal unpredictably through inflection points and straight
    // segments — the practical fix for "follow the curve's frame in 3D" without that artifact.
    // `up_hint` is a world-space reference direction (typically world up); if the path's tangent
    // ever runs parallel to it, an arbitrary stable perpendicular is substituted so the frame never
    // degenerates. Column-major: transform[0]=right, [1]=up, [2]=forward(tangent), [3]=position.
    [[nodiscard]] vector<glm::mat4> layout_text_on_spline_3d(const Spline3D &spline,
                                                                    span<const Text::PositionedGlyph> glyphs,
                                                                    u32 units_per_em, f32 pixel_size, glm::vec3 up_hint,
                                                                    f32 start_offset = 0.0f);

} // namespace SFT::Renderer
