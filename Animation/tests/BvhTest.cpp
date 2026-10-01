#include <Animation/Bvh.hpp>

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
    bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    const char *text = R"(HIERARCHY
ROOT Hips
{
	OFFSET 0.00 100.00 0.00
	CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
	JOINT Spine
	{
		OFFSET 0.00 20.00 0.00
		CHANNELS 3 Zrotation Xrotation Yrotation
		JOINT Head
		{
			OFFSET 0.00 30.00 0.00
			CHANNELS 3 Zrotation Xrotation Yrotation
			End Site
			{
				OFFSET 0.00 10.00 0.00
			}
		}
	}
}
MOTION
Frames: 3
Frame Time: 0.5
0 0 0 0 0 0   0 0 0   0 0 0
10 0 0 0 0 0  0 0 0   0 0 0
20 0 0 90 0 0 0 0 0   0 0 0
)";
    auto parsed = parse_bvh(text);
    check(parsed.has_value(), "BVH parses");
    if (!parsed) {
        std::cerr << parsed.error() << '\n';
        return 1;
    }
    const BvhData &d = *parsed;
    check(d.skeleton.joint_count() == 3 && d.skeleton.valid(), "three joints, parent-first");
    check(d.skeleton.names[2] == "Head" && d.skeleton.parents[2] == 1, "hierarchy");
    check(near(d.skeleton.rest_pose[0].translation.y, 1.0f), "centimetres auto-converted to metres");
    check(near(d.clip.duration, 1.0f) && d.clip.channels[0].translation.times.size() == 3, "frames and duration");

    Pose pose;
    sample_clip(d.skeleton, d.clip, 0.5f, false, pose);
    check(near(pose[0].translation.x, 0.1f), "root position channel (cm -> m) at frame 1");
    sample_clip(d.skeleton, d.clip, 1.0f, false, pose);
    check(near(glm::degrees(glm::angle(pose[0].rotation)), 90.0f, 0.1f), "Z rotation channel at frame 2");
    check(near(pose[1].rotation.w, 1.0f), "joints without motion stay at identity");

    check(!parse_bvh("HIERARCHY\nROOT x\n{\n").has_value(), "truncated file is an error");
    check(!parse_bvh("not a bvh").has_value(), "garbage is an error");
    return failures == 0 ? 0 : 1;
}
