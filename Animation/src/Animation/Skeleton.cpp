#include <Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace SFT::Animation {

    glm::mat4 JointTransform::to_matrix() const {
        glm::mat4 m = glm::mat4_cast(rotation);
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = glm::vec4(translation, 1.0f);
        return m;
    }

    u32 Skeleton::find_joint(std::string_view name) const noexcept {
        for (usize i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return static_cast<u32>(i);
            }
        }
        return no_joint;
    }

    bool Skeleton::valid() const noexcept {
        const usize n = parents.size();
        if (rest_pose.size() != n || inverse_bind.size() != n || (!names.empty() && names.size() != n)) {
            return false;
        }
        for (usize i = 0; i < n; ++i) {
            if (parents[i] != no_joint && parents[i] >= i) {
                return false;
            }
        }
        return true;
    }

    Pose rest_pose_of(const Skeleton &skeleton) {
        return skeleton.rest_pose;
    }

    JointTransform blend(const JointTransform &a, const JointTransform &b, f32 weight) {
        JointTransform out;
        out.translation = glm::mix(a.translation, b.translation, weight);
        out.scale = glm::mix(a.scale, b.scale, weight);
        glm::quat qb = b.rotation;
        if (glm::dot(a.rotation, qb) < 0.0f) {
            qb = -qb;
        }
        out.rotation = glm::normalize(glm::quat(
            glm::mix(a.rotation.w, qb.w, weight),
            glm::mix(a.rotation.x, qb.x, weight),
            glm::mix(a.rotation.y, qb.y, weight),
            glm::mix(a.rotation.z, qb.z, weight)));
        return out;
    }

    void blend_poses(const Pose &a, const Pose &b, f32 weight, Pose &out) {
        const usize n = std::min(a.size(), b.size());
        out.resize(n);
        for (usize i = 0; i < n; ++i) {
            out[i] = blend(a[i], b[i], weight);
        }
    }

    JointTransform add_joint(const JointTransform &base, const JointTransform &additive,
                             const JointTransform &reference, f32 weight) {
        JointTransform r;
        r.translation = base.translation + (additive.translation - reference.translation) * weight;
        r.scale = base.scale * glm::mix(glm::vec3(1.0f), additive.scale / reference.scale, weight);
        const glm::quat delta = additive.rotation * glm::inverse(reference.rotation);
        r.rotation = glm::normalize(blend(JointTransform{}, JointTransform{.rotation = delta}, weight).rotation * base.rotation);
        return r;
    }

    void apply_additive(const Pose &base, const Pose &additive, const Pose &reference, f32 weight, Pose &out) {
        const usize n = std::min({base.size(), additive.size(), reference.size()});
        out.resize(n);
        for (usize i = 0; i < n; ++i) {
            out[i] = add_joint(base[i], additive[i], reference[i], weight);
        }
    }

    void local_to_model(const Skeleton &skeleton, const Pose &pose, std::vector<glm::mat4> &model) {
        const usize n = skeleton.joint_count();
        model.resize(n);
        for (usize i = 0; i < n; ++i) {
            const glm::mat4 local = pose[i].to_matrix();
            const u32 parent = skeleton.parents[i];
            model[i] = parent == no_joint ? local : model[parent] * local;
        }
    }

    void skin_matrices(const Skeleton &skeleton, const Pose &pose, std::vector<glm::mat4> &out) {
        local_to_model(skeleton, pose, out);
        for (usize i = 0; i < out.size(); ++i) {
            out[i] = out[i] * skeleton.inverse_bind[i];
        }
    }

} // namespace SFT::Animation
