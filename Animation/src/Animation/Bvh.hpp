#pragma once

#include <Animation/Clip.hpp>
#include <Animation/Skeleton.hpp>

#include <expected>
#include <string>

namespace SFT::Animation {

    struct BvhOptions {
        /// Multiplier from file units to metres. Zero picks one automatically: skeletons taller than ten units are
        /// assumed to be in centimetres.
        f32 scale = 0.0f;
        /// Convert a Z-up file to the engine's Y-up (rotate -90 degrees about X at the root).
        bool z_up = false;
    };

    struct BvhData {
        Skeleton skeleton;
        Clip clip;
        f32 frame_time = 1.0f / 30.0f;
    };

    /// Parses a BioVision Hierarchy motion-capture file (the lingua franca of mocap libraries such as CMU,
    /// Mixamo exports, Rokoko, Xsens and Perception Neuron). The skeleton is parent-first with the OFFSET values as
    /// its rest pose; the clip holds one linear key per frame. End Sites are skipped.
    [[nodiscard]] std::expected<BvhData, UString> parse_bvh(const ustr &text, const BvhOptions &options = {});

} // namespace SFT::Animation
