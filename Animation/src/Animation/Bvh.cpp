#include <Animation/Bvh.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <vector>

namespace SFT::Animation {

    namespace {

        enum class Channel : u8 { Xpos, Ypos, Zpos, Xrot, Yrot, Zrot };

        struct Tokens {
            std::vector<std::string> list;
            usize at = 0;

            explicit Tokens(std::string_view text) {
                std::string current;
                for (char c : text) {
                    if (c == '{' || c == '}') {
                        if (!current.empty()) list.push_back(std::move(current));
                        current.clear();
                        list.emplace_back(1, c);
                    } else if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!current.empty()) list.push_back(std::move(current));
                        current.clear();
                    } else {
                        current.push_back(c);
                    }
                }
                if (!current.empty()) list.push_back(std::move(current));
            }
            [[nodiscard]] bool done() const { return at >= list.size(); }
            [[nodiscard]] const std::string &peek() const { return list[at]; }
            std::string next() { return done() ? std::string{} : list[at++]; }
        };

        [[nodiscard]] bool equals_nocase(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (usize i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }

        [[nodiscard]] bool to_float(const std::string &s, f32 &out) {
            const char *begin = s.data();
            const char *end = begin + s.size();
            // std::from_chars for float is available with libstdc++ 11+, but accept a leading '+' as BVH writers do.
            if (begin != end && *begin == '+') ++begin;
            const auto [ptr, ec] = std::from_chars(begin, end, out);
            return ec == std::errc{} && ptr == end;
        }

        struct JointInfo {
            std::vector<Channel> channels;
            usize first_channel = 0;
        };

    } // namespace

    std::expected<BvhData, std::string> parse_bvh(std::string_view text, const BvhOptions &options) {
        Tokens t(text);
        if (t.done() || !equals_nocase(t.next(), "HIERARCHY")) {
            return std::unexpected("BVH: missing HIERARCHY header.");
        }

        BvhData data;
        std::vector<JointInfo> joints;
        std::vector<glm::vec3> offsets;
        usize channel_total = 0;
        std::string error;

        // Recursive descent over ROOT/JOINT blocks.
        const auto parse_vec3 = [&](glm::vec3 &v) {
            for (int i = 0; i < 3; ++i) {
                if (!to_float(t.next(), v[i])) return false;
            }
            return true;
        };
        const auto parse_joint = [&](auto &&self, u32 parent, std::string name) -> bool {
            if (t.next() != "{") {
                error = "BVH: expected '{' after joint '" + name + "'.";
                return false;
            }
            const u32 index = static_cast<u32>(data.skeleton.parents.size());
            data.skeleton.names.push_back(name);
            data.skeleton.parents.push_back(parent);
            joints.emplace_back();
            offsets.emplace_back(0.0f);
            while (!t.done()) {
                const std::string word = t.next();
                if (word == "}") return true;
                if (equals_nocase(word, "OFFSET")) {
                    if (!parse_vec3(offsets[index])) {
                        error = "BVH: bad OFFSET on '" + name + "'.";
                        return false;
                    }
                } else if (equals_nocase(word, "CHANNELS")) {
                    f32 count_f = 0;
                    if (!to_float(t.next(), count_f)) {
                        error = "BVH: bad CHANNELS count on '" + name + "'.";
                        return false;
                    }
                    joints[index].first_channel = channel_total;
                    for (int c = 0; c < static_cast<int>(count_f); ++c) {
                        const std::string ch = t.next();
                        Channel channel;
                        if (equals_nocase(ch, "Xposition")) channel = Channel::Xpos;
                        else if (equals_nocase(ch, "Yposition")) channel = Channel::Ypos;
                        else if (equals_nocase(ch, "Zposition")) channel = Channel::Zpos;
                        else if (equals_nocase(ch, "Xrotation")) channel = Channel::Xrot;
                        else if (equals_nocase(ch, "Yrotation")) channel = Channel::Yrot;
                        else if (equals_nocase(ch, "Zrotation")) channel = Channel::Zrot;
                        else {
                            error = "BVH: unknown channel '" + ch + "'.";
                            return false;
                        }
                        joints[index].channels.push_back(channel);
                    }
                    channel_total += joints[index].channels.size();
                } else if (equals_nocase(word, "JOINT")) {
                    if (!self(self, index, t.next())) return false;
                } else if (equals_nocase(word, "End")) {
                    (void)t.next(); // "Site"
                    if (t.next() != "{") {
                        error = "BVH: malformed End Site.";
                        return false;
                    }
                    int depth = 1;
                    while (!t.done() && depth > 0) {
                        const std::string w = t.next();
                        if (w == "{") ++depth;
                        else if (w == "}") --depth;
                    }
                } else {
                    error = "BVH: unexpected token '" + word + "'.";
                    return false;
                }
            }
            error = "BVH: unterminated joint '" + name + "'.";
            return false;
        };

        if (t.done() || !equals_nocase(t.next(), "ROOT")) {
            return std::unexpected("BVH: expected ROOT.");
        }
        if (!parse_joint(parse_joint, no_joint, t.next())) {
            return std::unexpected(error);
        }

        if (t.done() || !equals_nocase(t.next(), "MOTION")) {
            return std::unexpected("BVH: missing MOTION section.");
        }
        f32 frames_f = 0.0f;
        f32 frame_time = 0.0f;
        if (!equals_nocase(t.next(), "Frames:") || !to_float(t.next(), frames_f) || !equals_nocase(t.next(), "Frame") ||
            !equals_nocase(t.next(), "Time:") || !to_float(t.next(), frame_time)) {
            return std::unexpected("BVH: malformed Frames/Frame Time line.");
        }
        const usize frames = static_cast<usize>(std::max(frames_f, 0.0f));
        if (frames == 0 || frame_time <= 0.0f) {
            return std::unexpected("BVH: the file has no frames.");
        }
        if (t.list.size() - t.at < frames * channel_total) {
            return std::unexpected("BVH: fewer motion values than Frames x channels.");
        }
        data.frame_time = frame_time;

        // Units and orientation.
        f32 scale = options.scale;
        if (scale <= 0.0f) {
            f32 extent = 0.0f;
            std::vector<glm::vec3> world(offsets.size());
            for (usize j = 0; j < offsets.size(); ++j) {
                const u32 p = data.skeleton.parents[j];
                world[j] = (p == no_joint ? glm::vec3(0.0f) : world[p]) + offsets[j];
                extent = std::max(extent, glm::length(world[j]));
            }
            scale = extent > 10.0f ? 0.01f : 1.0f;
        }
        const glm::quat up_fix = options.z_up ? glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0)) : glm::quat(1, 0, 0, 0);

        const usize n = joints.size();
        data.skeleton.rest_pose.resize(n);
        for (usize j = 0; j < n; ++j) {
            data.skeleton.rest_pose[j].translation = offsets[j] * scale;
            if (data.skeleton.parents[j] == no_joint) {
                data.skeleton.rest_pose[j].translation = up_fix * data.skeleton.rest_pose[j].translation;
                data.skeleton.rest_pose[j].rotation = up_fix;
            }
        }
        data.skeleton.inverse_bind.resize(n);
        {
            std::vector<glm::mat4> model;
            local_to_model(data.skeleton, data.skeleton.rest_pose, model);
            for (usize j = 0; j < n; ++j) data.skeleton.inverse_bind[j] = glm::inverse(model[j]);
        }

        Clip &clip = data.clip;
        clip.channels.resize(n);
        clip.joint_names = data.skeleton.names;
        clip.duration = frame_time * static_cast<f32>(frames - 1);
        for (usize j = 0; j < n; ++j) {
            const bool root = data.skeleton.parents[j] == no_joint;
            JointChannels &c = clip.channels[j];
            const bool has_pos = std::any_of(joints[j].channels.begin(), joints[j].channels.end(),
                                             [](Channel ch) { return ch <= Channel::Zpos; });
            const bool has_rot = std::any_of(joints[j].channels.begin(), joints[j].channels.end(),
                                             [](Channel ch) { return ch >= Channel::Xrot; });
            if (has_pos) {
                c.translation.times.reserve(frames);
                c.translation.values.reserve(frames * 3);
            }
            if (has_rot) {
                c.rotation.times.reserve(frames);
                c.rotation.values.reserve(frames * 4);
            }
            (void)root;
        }

        std::vector<f32> row(channel_total);
        for (usize f = 0; f < frames; ++f) {
            for (usize i = 0; i < channel_total; ++i) {
                if (!to_float(t.next(), row[i])) {
                    return std::unexpected("BVH: non-numeric motion value in frame " + std::to_string(f) + ".");
                }
            }
            const f32 time = frame_time * static_cast<f32>(f);
            for (usize j = 0; j < n; ++j) {
                const JointInfo &info = joints[j];
                if (info.channels.empty()) continue;
                glm::vec3 position = offsets[j];
                glm::quat rotation(1, 0, 0, 0);
                bool has_pos = false, has_rot = false;
                for (usize k = 0; k < info.channels.size(); ++k) {
                    const f32 v = row[info.first_channel + k];
                    switch (info.channels[k]) {
                        case Channel::Xpos: position.x = offsets[j].x + v; has_pos = true; break;
                        case Channel::Ypos: position.y = offsets[j].y + v; has_pos = true; break;
                        case Channel::Zpos: position.z = offsets[j].z + v; has_pos = true; break;
                        case Channel::Xrot: rotation = rotation * glm::angleAxis(glm::radians(v), glm::vec3(1, 0, 0)); has_rot = true; break;
                        case Channel::Yrot: rotation = rotation * glm::angleAxis(glm::radians(v), glm::vec3(0, 1, 0)); has_rot = true; break;
                        case Channel::Zrot: rotation = rotation * glm::angleAxis(glm::radians(v), glm::vec3(0, 0, 1)); has_rot = true; break;
                    }
                }
                const bool root = data.skeleton.parents[j] == no_joint;
                JointChannels &c = clip.channels[j];
                if (has_pos) {
                    glm::vec3 p = position * scale;
                    if (root) p = up_fix * p;
                    c.translation.times.push_back(time);
                    c.translation.values.insert(c.translation.values.end(), {p.x, p.y, p.z});
                }
                if (has_rot) {
                    glm::quat q = glm::normalize(rotation);
                    if (root) q = up_fix * q;
                    c.rotation.times.push_back(time);
                    c.rotation.values.insert(c.rotation.values.end(), {q.x, q.y, q.z, q.w});
                }
            }
        }
        clip.name = "bvh";
        return data;
    }

} // namespace SFT::Animation
