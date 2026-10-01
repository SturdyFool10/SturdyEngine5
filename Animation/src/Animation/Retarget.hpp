#pragma once

#include <Animation/Clip.hpp>
#include <Animation/Skeleton.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Animation {

    /// Canonical humanoid slots shared by Mixamo, Unreal, Unity, VRM, 3ds Max Biped, Blender Rigify and most
    /// mocap libraries. Fingers are not part of the map: they follow by name (`remap_clip_by_name`) or stay in
    /// the target's rest pose.
    enum class HumanoidBone : u8 {
        Hips, Spine, Chest, UpperChest, Neck, Head,
        LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
        RightShoulder, RightUpperArm, RightLowerArm, RightHand,
        LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes,
        RightUpperLeg, RightLowerLeg, RightFoot, RightToes,
        Count
    };
    inline constexpr usize humanoid_bone_count = static_cast<usize>(HumanoidBone::Count);

    [[nodiscard]] std::string_view humanoid_bone_name(HumanoidBone bone) noexcept;

    /// Which joint of a skeleton plays each humanoid slot (`no_joint` when the rig has none).
    struct HumanoidMap {
        std::array<u32, humanoid_bone_count> joints;

        HumanoidMap() { joints.fill(no_joint); }
        [[nodiscard]] u32 operator[](HumanoidBone bone) const noexcept { return joints[static_cast<usize>(bone)]; }
        u32 &operator[](HumanoidBone bone) noexcept { return joints[static_cast<usize>(bone)]; }
        /// Hips, both legs (upper/lower/foot) and both arms (upper/lower/hand) present.
        [[nodiscard]] bool usable() const noexcept;
    };

    /// Guesses the humanoid slots from bone names and hierarchy. Understands Mixamo (`mixamorig:LeftArm`), Unreal
    /// (`upperarm_l`), Biped (`Bip01 L UpperArm`), VRM, Rigify/Blender (`upper_arm.L`) and plain `LeftUpperArm`
    /// conventions. Returns the map even when partial; check `usable()`.
    [[nodiscard]] HumanoidMap guess_humanoid_map(const Skeleton &skeleton);

    struct RetargetOptions {
        /// Baked output sampling rate.
        f32 sample_rate = 30.0f;
        /// Scale applied to the hips' movement; negative = derive from the two rigs' hip heights.
        f32 hips_translation_scale = -1.0f;
        /// Also copy channels of bones that are not humanoid slots but have the same normalised name in both rigs
        /// (fingers, tail, face bones).
        bool copy_matching_bones = true;
    };

    /// Retargets `clip` (authored for `source`) onto `target` through humanoid maps. Each mapped bone keeps its
    /// world-space rotation change from rest, so rigs with different rest poses (T vs A pose) and bone axes line
    /// up; the hips' travel is scaled by body size. The result is baked at `options.sample_rate` and keeps events
    /// and morph tracks.
    [[nodiscard]] Clip retarget_clip(const Clip &clip, const Skeleton &source, const HumanoidMap &source_map,
                                     const Skeleton &target, const HumanoidMap &target_map,
                                     const RetargetOptions &options = {});

    /// Rebinds a clip to `target` purely by joint name (after normalising prefixes such as `mixamorig:`).
    /// Joints the clip does not know keep their rest value; clip joints the target lacks are dropped.
    /// Use when both rigs share a skeleton layout (e.g. an animation-only file for the same character).
    [[nodiscard]] Clip remap_clip_by_name(const Clip &clip, const Skeleton &target);

    /// Picks the right strategy for moving `clip` (authored for `source`) onto `target`: the rigs share their
    /// layout (most bone names match) -> `remap_clip_by_name`; both are recognisable humanoids -> `retarget_clip`;
    /// otherwise a best-effort name remap.
    [[nodiscard]] Clip adapt_clip(const Clip &clip, const Skeleton &source, const Skeleton &target,
                                  const RetargetOptions &options = {});

    /// Lower-cases and strips namespace/prefix noise: "mixamorig:LeftArm" -> "leftarm".
    [[nodiscard]] std::string normalize_joint_name(std::string_view name);

} // namespace SFT::Animation
