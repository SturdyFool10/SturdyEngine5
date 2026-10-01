#include <Animation/Clip.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Animation {

    namespace {

        struct Segment {
            usize index = 0; // key at or before the time
            f32 t = 0.0f;    // normalized position to the next key
            f32 dt = 0.0f;   // key interval in seconds
            bool at_end = false;
        };

        Segment locate(const Track &track, f32 time) {
            const auto &times = track.times;
            if (time <= times.front()) {
                return {0, 0.0f, 0.0f, times.size() == 1};
            }
            if (time >= times.back()) {
                return {times.size() - 1, 0.0f, 0.0f, true};
            }
            const auto it = std::upper_bound(times.begin(), times.end(), time);
            const usize i = static_cast<usize>(it - times.begin()) - 1;
            const f32 dt = times[i + 1] - times[i];
            return {i, dt > 0.0f ? (time - times[i]) / dt : 0.0f, dt, false};
        }

        // Cubic Hermite per the glTF spec; tangents are scaled by the key interval.
        template <class V>
        V hermite(const V &p0, const V &m0, const V &p1, const V &m1, f32 t, f32 dt) {
            const f32 t2 = t * t;
            const f32 t3 = t2 * t;
            return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * dt * m0
                 + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * dt * m1;
        }

        template <class V, usize N>
        V read(const Track &track, usize key, usize slot) {
            // CubicSpline: three tuples per key, [in, value, out]; otherwise one.
            const usize stride = track.interpolation == Interpolation::CubicSpline ? 3 : 1;
            const usize base = (key * stride + slot) * N;
            if constexpr (N == 3) {
                return V(track.values[base], track.values[base + 1], track.values[base + 2]);
            } else {
                return V(track.values[base + 3], track.values[base], track.values[base + 1], track.values[base + 2]);
            }
        }

    } // namespace

    void Clip::recompute_duration() {
        duration = 0.0f;
        for (const auto &c : channels) {
            for (const Track *t : {&c.translation, &c.rotation, &c.scale}) {
                if (!t->empty()) {
                    duration = std::max(duration, t->times.back());
                }
            }
        }
        for (const auto &m : morph_tracks) {
            if (!m.track.empty()) {
                duration = std::max(duration, m.track.times.back());
            }
        }
    }

    const MorphTrack *Clip::find_morph_track(std::string_view target) const noexcept {
        for (const auto &m : morph_tracks) {
            if (target.empty() || m.target == target) {
                return &m;
            }
        }
        return nullptr;
    }

    void sample_weights(const MorphTrack &morph, f32 time, std::vector<f32> &out) {
        const usize n = morph.weight_count;
        out.assign(n, 0.0f);
        const Track &track = morph.track;
        if (track.empty() || n == 0) {
            return;
        }
        const bool cubic = track.interpolation == Interpolation::CubicSpline;
        const usize stride = cubic ? 3 : 1;
        const auto value = [&](usize key, usize slot, usize i) { return track.values[(key * stride + slot) * n + i]; };
        const usize slot_value = cubic ? 1 : 0;
        const Segment s = locate(track, time);
        for (usize i = 0; i < n; ++i) {
            const f32 a = value(s.index, slot_value, i);
            if (s.at_end || track.interpolation == Interpolation::Step) {
                out[i] = a;
            } else if (!cubic) {
                out[i] = a + (value(s.index + 1, 0, i) - a) * s.t;
            } else {
                out[i] = hermite(a, value(s.index, 2, i), value(s.index + 1, 1, i), value(s.index + 1, 0, i), s.t, s.dt);
            }
        }
    }

    glm::vec3 sample_vec3(const Track &track, f32 time) {
        const Segment s = locate(track, time);
        const usize slot_value = track.interpolation == Interpolation::CubicSpline ? 1 : 0;
        const glm::vec3 a = read<glm::vec3, 3>(track, s.index, slot_value);
        if (s.at_end || track.interpolation == Interpolation::Step) {
            return a;
        }
        const glm::vec3 b = read<glm::vec3, 3>(track, s.index + 1, slot_value);
        if (track.interpolation == Interpolation::Linear) {
            return glm::mix(a, b, s.t);
        }
        return hermite(a, read<glm::vec3, 3>(track, s.index, 2), b, read<glm::vec3, 3>(track, s.index + 1, 0), s.t, s.dt);
    }

    glm::quat sample_quat(const Track &track, f32 time) {
        const Segment s = locate(track, time);
        const usize slot_value = track.interpolation == Interpolation::CubicSpline ? 1 : 0;
        const glm::quat a = read<glm::quat, 4>(track, s.index, slot_value);
        if (s.at_end || track.interpolation == Interpolation::Step) {
            return glm::normalize(a);
        }
        const glm::quat b = read<glm::quat, 4>(track, s.index + 1, slot_value);
        if (track.interpolation == Interpolation::Linear) {
            return glm::normalize(glm::slerp(a, b, s.t));
        }
        // glm::quat has no vector operators we can use blindly; do Hermite on components.
        const glm::quat m0 = read<glm::quat, 4>(track, s.index, 2);
        const glm::quat m1 = read<glm::quat, 4>(track, s.index + 1, 0);
        const f32 t2 = s.t * s.t;
        const f32 t3 = t2 * s.t;
        const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h10 = (t3 - 2.0f * t2 + s.t) * s.dt;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        const f32 h11 = (t3 - t2) * s.dt;
        glm::quat r(
            h00 * a.w + h10 * m0.w + h01 * b.w + h11 * m1.w,
            h00 * a.x + h10 * m0.x + h01 * b.x + h11 * m1.x,
            h00 * a.y + h10 * m0.y + h01 * b.y + h11 * m1.y,
            h00 * a.z + h10 * m0.z + h01 * b.z + h11 * m1.z);
        return glm::normalize(r);
    }

    f32 wrap_time(const Clip &clip, f32 time, bool loop) {
        if (clip.duration <= 0.0f) {
            return 0.0f;
        }
        if (loop) {
            const f32 w = std::fmod(time, clip.duration);
            return w < 0.0f ? w + clip.duration : w;
        }
        return std::clamp(time, 0.0f, clip.duration);
    }

    void sample_clip(const Skeleton &skeleton, const Clip &clip, f32 time, bool loop, Pose &out) {
        out = skeleton.rest_pose;
        const f32 t = wrap_time(clip, time, loop);
        const usize n = std::min(out.size(), clip.channels.size());
        for (usize i = 0; i < n; ++i) {
            const auto &c = clip.channels[i];
            if (!c.translation.empty()) {
                out[i].translation = sample_vec3(c.translation, t);
            }
            if (!c.rotation.empty()) {
                out[i].rotation = sample_quat(c.rotation, t);
            }
            if (!c.scale.empty()) {
                out[i].scale = sample_vec3(c.scale, t);
            }
        }
    }

} // namespace SFT::Animation
