#include <Animation/Clip.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>

using namespace SFT::Animation;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    Skeleton skel;
    skel.names = {"root", "arm"};
    skel.parents = {no_joint, 0};
    skel.rest_pose = {JointTransform{}, JointTransform{.translation = {0, 1, 0}}};
    skel.inverse_bind = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, 0))};
    check(skel.valid(), "skeleton valid");
    check(skel.find_joint("arm") == 1 && skel.find_joint("x") == no_joint, "find_joint");
    skel.parents[0] = 1;
    check(!skel.valid(), "non parent-first hierarchy is rejected");
    skel.parents[0] = no_joint;

    Clip clip;
    clip.channels.resize(2);
    auto &t = clip.channels[0].translation;
    t.times = {0.0f, 1.0f, 2.0f};
    t.values = {0, 0, 0, 2, 0, 0, 2, 4, 0};
    clip.recompute_duration();
    check(near(clip.duration, 2.0f), "duration from tracks");

    Pose pose;
    sample_clip(skel, clip, 0.5f, false, pose);
    check(near(pose[0].translation.x, 1.0f), "linear midpoint");
    sample_clip(skel, clip, 1.5f, false, pose);
    check(near(pose[0].translation.y, 2.0f) && near(pose[0].translation.x, 2.0f), "second segment");
    sample_clip(skel, clip, 5.0f, false, pose);
    check(near(pose[0].translation.y, 4.0f), "clamped past the end");
    sample_clip(skel, clip, 2.5f, true, pose);
    check(near(pose[0].translation.x, 1.0f), "looped wrap");
    check(near(pose[1].translation.y, 1.0f), "untouched joint keeps rest value");

    t.interpolation = Interpolation::Step;
    sample_clip(skel, clip, 0.9f, false, pose);
    check(near(pose[0].translation.x, 0.0f), "step holds previous key");

    // Cubic spline with zero tangents is a smoothstep between the values.
    Track cubic;
    cubic.interpolation = Interpolation::CubicSpline;
    cubic.times = {0.0f, 1.0f};
    cubic.values = {0, 0, 0, 0, 0, 0, 0, 0, 0, /*key1*/ 0, 0, 0, 10, 0, 0, 0, 0, 0};
    check(near(sample_vec3(cubic, 0.5f).x, 5.0f), "cubic midpoint with zero tangents");
    check(near(sample_vec3(cubic, 0.25f).x, 10.0f * (3 * 0.0625f - 2 * 0.015625f)), "cubic eased");

    // Rotation: 90 degrees about Y, halfway is 45 degrees.
    Track rot;
    rot.times = {0.0f, 1.0f};
    const float s = std::sin(glm::radians(45.0f)), c = std::cos(glm::radians(45.0f));
    rot.values = {0, 0, 0, 1, 0, s, 0, c};
    const glm::quat half = sample_quat(rot, 0.5f);
    check(near(glm::degrees(glm::angle(half)), 45.0f, 1e-2f), "slerp halfway");

    // Skinning: rest pose yields identity skin matrices; moving the root translates children.
    std::vector<glm::mat4> skin;
    skin_matrices(skel, rest_pose_of(skel), skin);
    check(near(skin[1][3].y, 0.0f) && near(skin[1][0].x, 1.0f), "rest pose skin matrices are identity");
    Pose moved = rest_pose_of(skel);
    moved[0].translation = {3, 0, 0};
    skin_matrices(skel, moved, skin);
    check(near(skin[1][3].x, 3.0f), "child follows parent");

    Pose a = rest_pose_of(skel), b = rest_pose_of(skel), out;
    b[0].translation = {2, 0, 0};
    blend_poses(a, b, 0.5f, out);
    check(near(out[0].translation.x, 1.0f), "pose blend");
    Pose add = rest_pose_of(skel);
    add[0].translation = {1, 0, 0};
    apply_additive(a, add, rest_pose_of(skel), 0.5f, out);
    check(near(out[0].translation.x, 0.5f), "additive layer weight");

    return failures == 0 ? 0 : 1;
}
