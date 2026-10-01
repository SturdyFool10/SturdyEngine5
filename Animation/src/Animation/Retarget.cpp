#include <Animation/Retarget.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace SFT::Animation {

    std::string_view humanoid_bone_name(HumanoidBone bone) noexcept {
        static constexpr std::string_view names[] = {
            "Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
            "LeftShoulder", "LeftUpperArm", "LeftLowerArm", "LeftHand",
            "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
            "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
            "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes"};
        const usize i = static_cast<usize>(bone);
        return i < std::size(names) ? names[i] : std::string_view{};
    }

    bool HumanoidMap::usable() const noexcept {
        for (HumanoidBone b : {HumanoidBone::Hips, HumanoidBone::LeftUpperLeg, HumanoidBone::LeftLowerLeg,
                               HumanoidBone::LeftFoot, HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg,
                               HumanoidBone::RightFoot, HumanoidBone::LeftUpperArm, HumanoidBone::LeftLowerArm,
                               HumanoidBone::LeftHand, HumanoidBone::RightUpperArm, HumanoidBone::RightLowerArm,
                               HumanoidBone::RightHand}) {
            if ((*this)[b] == no_joint) return false;
        }
        return true;
    }

    // ---- name normalisation ----------------------------------------------------------------

    namespace {

        [[nodiscard]] std::string lower(std::string_view s) {
            std::string out(s);
            for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // Splits "UpperArm_L", "upper_arm.L", "LeftUpArm", "Bip01 L Forearm" into lower-case tokens.
        [[nodiscard]] std::vector<std::string> tokenize(std::string_view name) {
            if (const usize colon = name.rfind(':'); colon != std::string_view::npos) name = name.substr(colon + 1);
            std::vector<std::string> tokens;
            std::string current;
            const auto flush = [&] {
                if (!current.empty()) tokens.push_back(lower(current));
                current.clear();
            };
            for (usize i = 0; i < name.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(name[i]);
                if (!std::isalnum(c)) {
                    flush();
                    continue;
                }
                const bool boundary = !current.empty() && std::isupper(c) &&
                                      (std::islower(static_cast<unsigned char>(current.back())) ||
                                       std::isdigit(static_cast<unsigned char>(current.back())));
                const bool digit_edge = !current.empty() && std::isdigit(c) != std::isdigit(static_cast<unsigned char>(current.back())) &&
                                        std::isdigit(c);
                if (boundary || digit_edge) flush();
                current.push_back(static_cast<char>(c));
            }
            flush();
            return tokens;
        }

        struct ParsedName {
            int side = 0;         // -1 left, +1 right, 0 none
            std::string base;     // joined remaining tokens, e.g. "upperarm"
        };

        [[nodiscard]] ParsedName parse_name(std::string_view name) {
            ParsedName parsed;
            std::vector<std::string> tokens = tokenize(name);
            std::vector<std::string> kept;
            for (const std::string &t : tokens) {
                if (t == "left" || t == "l") parsed.side = -1;
                else if (t == "right" || t == "r") parsed.side = +1;
                else if (t == "mixamorig" || t == "bip" || t == "bip01" || t == "bip001" || t == "jbip" || t == "c" ||
                         t == "j" || t == "def" || t == "deform" || t == "armature" || t == "skeleton" || t == "01" ||
                         t == "001" || t == "root")
                    continue;
                else kept.push_back(t);
            }
            // "leftupperarm" (single token) -> side + remainder
            if (parsed.side == 0 && kept.size() == 1) {
                for (const auto &[prefix, side] : {std::pair<std::string_view, int>{"left", -1}, {"right", +1}}) {
                    if (kept[0].rfind(prefix, 0) == 0 && kept[0].size() > prefix.size()) {
                        parsed.side = side;
                        kept[0] = kept[0].substr(prefix.size());
                    }
                }
            }
            for (const std::string &t : kept) parsed.base += t;
            return parsed;
        }

    } // namespace

    std::string normalize_joint_name(std::string_view name) {
        std::string out;
        for (const std::string &t : tokenize(name)) {
            if (t == "mixamorig" || t == "armature") continue;
            out += t;
        }
        return out;
    }

    HumanoidMap guess_humanoid_map(const Skeleton &skeleton) {
        HumanoidMap map;
        struct Candidate {
            u32 joint;
            std::string base;
        };
        std::vector<Candidate> spine_chain;
        std::vector<u32> plain_leg_left, plain_leg_right;
        const auto depth = [&](u32 j) {
            u32 d = 0;
            for (u32 p = skeleton.parents[j]; p != no_joint; p = skeleton.parents[p]) ++d;
            return d;
        };
        const auto set_if_empty = [&](HumanoidBone b, u32 j) {
            if (map[b] == no_joint) map[b] = j;
        };

        // Joints are parent-first, so the first match along a limb is the upper segment.
        for (u32 j = 0; j < skeleton.joint_count(); ++j) {
            const ParsedName n = parse_name(skeleton.names.empty() ? std::string_view{} : std::string_view{skeleton.names[j]});
            const std::string &b = n.base;
            const bool left = n.side < 0, right = n.side > 0;
            if (b == "hips" || b == "pelvis" || b == "hip") {
                if (n.side == 0) set_if_empty(HumanoidBone::Hips, j);
            } else if (n.side == 0 && (b == "spine" || b == "spine1" || b == "spine2" || b == "spine3" || b == "chest" ||
                                        b == "upperchest" || b == "spine01" || b == "spine02" || b == "spine03" || b == "abdomen" || b == "torso")) {
                spine_chain.push_back({j, b});
            } else if (n.side == 0 && b == "neck") {
                set_if_empty(HumanoidBone::Neck, j);
            } else if (n.side == 0 && b == "head") {
                set_if_empty(HumanoidBone::Head, j);
            } else if (b == "shoulder" || b == "clavicle" || b == "collar") {
                if (left) set_if_empty(HumanoidBone::LeftShoulder, j);
                if (right) set_if_empty(HumanoidBone::RightShoulder, j);
            } else if (b == "upperarm" || b == "arm" || b == "uparm" || b == "armupper") {
                if (left) set_if_empty(HumanoidBone::LeftUpperArm, j);
                if (right) set_if_empty(HumanoidBone::RightUpperArm, j);
            } else if (b == "lowerarm" || b == "forearm" || b == "armlower" || b == "elbow") {
                if (left) set_if_empty(HumanoidBone::LeftLowerArm, j);
                if (right) set_if_empty(HumanoidBone::RightLowerArm, j);
            } else if (b == "hand" || b == "wrist") {
                if (left) set_if_empty(HumanoidBone::LeftHand, j);
                if (right) set_if_empty(HumanoidBone::RightHand, j);
            } else if (b == "leg" || b == "leg1" || b == "leg2") {
                // Ambiguous: Mixamo's "Leg" is the shin next to "UpLeg", other rigs use "Leg"/"Leg2" for thigh/shin.
                (left ? plain_leg_left : plain_leg_right).push_back(j);
            } else if (b == "upperleg" || b == "thigh" || b == "upleg" || b == "legupper") {
                if (left) set_if_empty(HumanoidBone::LeftUpperLeg, j);
                if (right) set_if_empty(HumanoidBone::RightUpperLeg, j);
            } else if (b == "lowerleg" || b == "calf" || b == "shin" || b == "leglower" || b == "knee") {
                if (left) set_if_empty(HumanoidBone::LeftLowerLeg, j);
                if (right) set_if_empty(HumanoidBone::RightLowerLeg, j);
            } else if (b == "foot" || b == "ankle") {
                if (left) set_if_empty(HumanoidBone::LeftFoot, j);
                if (right) set_if_empty(HumanoidBone::RightFoot, j);
            } else if (b == "toes" || b == "toe" || b == "toebase" || b == "ball") {
                if (left) set_if_empty(HumanoidBone::LeftToes, j);
                if (right) set_if_empty(HumanoidBone::RightToes, j);
            }
        }

        const auto resolve_legs = [&](const std::vector<u32> &plain, HumanoidBone upper, HumanoidBone lower) {
            usize next = 0;
            if (map[upper] == no_joint && next < plain.size()) map[upper] = plain[next++];
            if (map[lower] == no_joint && next < plain.size()) map[lower] = plain[next++];
        };
        resolve_legs(plain_leg_left, HumanoidBone::LeftUpperLeg, HumanoidBone::LeftLowerLeg);
        resolve_legs(plain_leg_right, HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg);

        // Spine chain: hierarchy order -> Spine, Chest, UpperChest (extra segments are skipped).
        std::stable_sort(spine_chain.begin(), spine_chain.end(),
                         [&](const Candidate &a, const Candidate &c) { return depth(a.joint) < depth(c.joint); });
        constexpr HumanoidBone slots[] = {HumanoidBone::Spine, HumanoidBone::Chest, HumanoidBone::UpperChest};
        if (spine_chain.size() <= 3) {
            for (usize i = 0; i < spine_chain.size(); ++i) map[slots[i]] = spine_chain[i].joint;
        } else {
            map[HumanoidBone::Spine] = spine_chain.front().joint;
            map[HumanoidBone::Chest] = spine_chain[spine_chain.size() / 2].joint;
            map[HumanoidBone::UpperChest] = spine_chain.back().joint;
        }
        // A rig with a single "leg" segment per side name pair (Unreal "thigh"/"calf") resolved above; a rig whose
        // legs are named "leg"+"leg2" is left with the lower leg empty and reported unusable.
        return map;
    }

    // ---- retargeting -------------------------------------------------------------------------

    namespace {

        struct WorldPose {
            std::vector<glm::quat> rotation;
            std::vector<glm::vec3> position;
        };

        // Rotation + position per joint in model space (scale ignored: rigs here are rigid-bone).
        void compute_world(const Skeleton &skeleton, const Pose &pose, WorldPose &out) {
            const usize n = skeleton.joint_count();
            out.rotation.resize(n);
            out.position.resize(n);
            for (usize j = 0; j < n; ++j) {
                const u32 p = skeleton.parents[j];
                if (p == no_joint) {
                    out.rotation[j] = pose[j].rotation;
                    out.position[j] = pose[j].translation;
                } else {
                    out.rotation[j] = out.rotation[p] * pose[j].rotation;
                    out.position[j] = out.position[p] + out.rotation[p] * (pose[j].translation * 1.0f);
                }
            }
        }

        void append_track(Track &track, f32 time, const glm::vec3 &v) {
            track.times.push_back(time);
            track.values.insert(track.values.end(), {v.x, v.y, v.z});
        }
        void append_track(Track &track, f32 time, const glm::quat &q) {
            track.times.push_back(time);
            track.values.insert(track.values.end(), {q.x, q.y, q.z, q.w});
        }

    } // namespace

    Clip retarget_clip(const Clip &clip, const Skeleton &source, const HumanoidMap &source_map,
                       const Skeleton &target, const HumanoidMap &target_map, const RetargetOptions &options) {
        Clip out;
        out.name = clip.name;
        out.duration = clip.duration;
        out.events = clip.events;
        out.morph_tracks = clip.morph_tracks;
        out.channels.resize(target.joint_count());
        out.joint_names = target.names;

        // target joint -> source joint it copies the motion of
        std::vector<u32> mapped(target.joint_count(), no_joint);
        for (usize b = 0; b < humanoid_bone_count; ++b) {
            if (source_map.joints[b] != no_joint && target_map.joints[b] != no_joint) {
                mapped[target_map.joints[b]] = source_map.joints[b];
            }
        }
        if (options.copy_matching_bones) {
            std::unordered_map<std::string, u32> source_by_name;
            for (u32 j = 0; j < source.joint_count(); ++j) {
                if (!source.names.empty()) source_by_name.emplace(normalize_joint_name(source.names[j]), j);
            }
            for (u32 j = 0; j < target.joint_count(); ++j) {
                if (mapped[j] != no_joint || target.names.empty()) continue;
                const auto it = source_by_name.find(normalize_joint_name(target.names[j]));
                if (it != source_by_name.end()) mapped[j] = it->second;
            }
        }

        WorldPose source_bind, target_bind;
        compute_world(source, source.rest_pose, source_bind);
        compute_world(target, target.rest_pose, target_bind);

        f32 hips_scale = options.hips_translation_scale;
        const u32 source_hips = source_map[HumanoidBone::Hips];
        const u32 target_hips = target_map[HumanoidBone::Hips];
        if (hips_scale < 0.0f) {
            hips_scale = 1.0f;
            if (source_hips != no_joint && target_hips != no_joint) {
                const f32 sh = std::fabs(source_bind.position[source_hips].y);
                const f32 th = std::fabs(target_bind.position[target_hips].y);
                if (sh > 1e-4f && th > 1e-4f) hips_scale = th / sh;
            }
        }

        const f32 duration = std::max(clip.duration, 0.0f);
        const usize frames = std::max<usize>(2, static_cast<usize>(std::ceil(duration * options.sample_rate)) + 1);
        Pose source_pose, target_pose = target.rest_pose;
        WorldPose source_world;
        std::vector<glm::quat> target_world_rot(target.joint_count());
        std::vector<glm::vec3> target_world_pos(target.joint_count());

        for (usize f = 0; f < frames; ++f) {
            const f32 time = frames > 1 ? duration * static_cast<f32>(f) / static_cast<f32>(frames - 1) : 0.0f;
            sample_clip(source, clip, time, false, source_pose);
            compute_world(source, source_pose, source_world);

            for (u32 j = 0; j < target.joint_count(); ++j) {
                const u32 p = target.parents[j];
                const glm::quat parent_rot = p == no_joint ? glm::quat(1, 0, 0, 0) : target_world_rot[p];
                const glm::vec3 parent_pos = p == no_joint ? glm::vec3(0) : target_world_pos[p];
                JointTransform local = target.rest_pose[j];
                glm::quat world_rot = parent_rot * local.rotation;
                glm::vec3 world_pos = parent_pos + parent_rot * local.translation;
                const u32 s = mapped[j];
                if (s != no_joint) {
                    const glm::quat delta = source_world.rotation[s] * glm::inverse(source_bind.rotation[s]);
                    world_rot = glm::normalize(delta * target_bind.rotation[j]);
                    local.rotation = glm::normalize(glm::inverse(parent_rot) * world_rot);
                    if (j == target_hips) {
                        world_pos = target_bind.position[j] + (source_world.position[s] - source_bind.position[s]) * hips_scale;
                        local.translation = glm::inverse(parent_rot) * (world_pos - parent_pos);
                    }
                }
                target_pose[j] = local;
                target_world_rot[j] = world_rot;
                target_world_pos[j] = world_pos;
            }

            for (u32 j = 0; j < target.joint_count(); ++j) {
                if (mapped[j] == no_joint) continue;
                append_track(out.channels[j].rotation, time, target_pose[j].rotation);
                if (j == target_hips) append_track(out.channels[j].translation, time, target_pose[j].translation);
            }
        }
        return out;
    }

    Clip adapt_clip(const Clip &clip, const Skeleton &source, const Skeleton &target, const RetargetOptions &options) {
        Clip named = clip;
        if (named.joint_names.empty()) named.joint_names = source.names;

        std::unordered_map<std::string, bool> source_names;
        for (const std::string &n : source.names) source_names.emplace(normalize_joint_name(n), true);
        usize matches = 0;
        for (const std::string &n : target.names) matches += source_names.count(normalize_joint_name(n));
        const f32 coverage = target.joint_count() > 0 ? static_cast<f32>(matches) / static_cast<f32>(target.joint_count()) : 0.0f;
        if (coverage >= 0.7f) {
            return remap_clip_by_name(named, target);
        }
        const HumanoidMap source_map = guess_humanoid_map(source);
        const HumanoidMap target_map = guess_humanoid_map(target);
        if (source_map.usable() && target_map.usable()) {
            return retarget_clip(named, source, source_map, target, target_map, options);
        }
        return remap_clip_by_name(named, target);
    }

    Clip remap_clip_by_name(const Clip &clip, const Skeleton &target) {
        Clip out;
        out.name = clip.name;
        out.duration = clip.duration;
        out.events = clip.events;
        out.morph_tracks = clip.morph_tracks;
        out.channels.resize(target.joint_count());
        out.joint_names = target.names;
        std::unordered_map<std::string, usize> by_name;
        for (usize i = 0; i < clip.joint_names.size(); ++i) {
            by_name.emplace(clip.joint_names[i], i);
            by_name.emplace(normalize_joint_name(clip.joint_names[i]), i);
        }
        for (usize j = 0; j < target.joint_count(); ++j) {
            if (target.names.empty()) break;
            auto it = by_name.find(target.names[j]);
            if (it == by_name.end()) it = by_name.find(normalize_joint_name(target.names[j]));
            if (it != by_name.end() && it->second < clip.channels.size()) {
                out.channels[j] = clip.channels[it->second];
            }
        }
        return out;
    }

} // namespace SFT::Animation
