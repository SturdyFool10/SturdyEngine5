#pragma once

#include <Animation/Skeleton.hpp>

#include <string>
#include <vector>

namespace SFT::Animation {

    enum class Interpolation : u8 {
        Step,
        Linear,
        /// glTF layout: per key, (in-tangent, value, out-tangent).
        CubicSpline,
    };

    /// A single animated property of one joint. `values` holds `components` floats per key (3 for
    /// translation/scale, 4 xyzw for rotation); CubicSpline stores three such tuples per key.
    struct Track {
        Interpolation interpolation = Interpolation::Linear;
        std::vector<f32> times;
        std::vector<f32> values;

        [[nodiscard]] bool empty() const noexcept { return times.empty(); }
    };

    struct JointChannels {
        Track translation;
        Track rotation;
        Track scale;
    };

    /// Morph-target weights over time for the mesh carried by the node called `target`. `Track::values` holds
    /// `weight_count` floats per key (three such tuples per key for CubicSpline).
    struct MorphTrack {
        UString target;
        u32 weight_count = 0;
        Track track;
    };

    /// A named marker on a clip's timeline (footstep, muzzle flash, "spawn projectile"), in seconds.
    struct ClipEvent {
        f32 time = 0.0f;
        UString name;
    };

    /// Animation clip addressed by joint index of a specific skeleton.
    struct Clip {
        UString name;
        f32 duration = 0.0f;
        /// Same length as the skeleton's joint count; joints with empty tracks keep their rest value.
        std::vector<JointChannels> channels;
        /// Name of the joint each channel animates (parallel to `channels`); lets the clip be rebound to a
        /// different skeleton by name (see `retarget.hpp`'s `remap_clip`).
        std::vector<UString> joint_names;
        std::vector<MorphTrack> morph_tracks;
        std::vector<ClipEvent> events;

        void recompute_duration();

        /// The morph track for `target`, or the first one when `target` is empty; null when there is none.
        [[nodiscard]] const MorphTrack *find_morph_track(const ustr &target) const noexcept;
    };

    [[nodiscard]] glm::vec3 sample_vec3(const Track &track, f32 time);
    [[nodiscard]] glm::quat sample_quat(const Track &track, f32 time);

    /// Samples `weight_count` floats of a morph track into `out` (resized).
    void sample_weights(const MorphTrack &track, f32 time, std::vector<f32> &out);

    /// Wraps (`loop`) or clamps `time` into [0, duration].
    [[nodiscard]] f32 wrap_time(const Clip &clip, f32 time, bool loop);

    /// Samples every joint at `time`; untouched channels take the skeleton's rest pose.
    void sample_clip(const Skeleton &skeleton, const Clip &clip, f32 time, bool loop, Pose &out);

} // namespace SFT::Animation
