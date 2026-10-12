// The body-camera rig (Engine/BodyCameraRig.cpp): its springs, gait, handheld wander and impulses, as properties.

#include <Engine/BodyCameraRig.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdio>

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
    bool near(const glm::vec3 &a, const glm::vec3 &b, float eps = 1e-3f) { return glm::length(a - b) <= eps; }
    // atan2 of the relative rotation's vector and scalar parts: acos(dot) loses all precision for angles near zero.
    float angle_between(const glm::quat &a, const glm::quat &b) {
        const glm::quat delta = a * glm::inverse(b);
        return 2.0f * std::atan2(glm::length(glm::vec3{delta.x, delta.y, delta.z}), std::abs(delta.w));
    }

    // Springs only: no gait, no wander.
    SFT::Engine::BodyCameraRigSettings springs_only() {
        SFT::Engine::BodyCameraRigSettings s;
        s.bob_amplitude = s.sway_amplitude = s.roll_amplitude_degrees = s.pitch_amplitude_degrees = 0.0f;
        s.handheld_rotation_degrees = s.handheld_position = 0.0f;
        return s;
    }
    const glm::quat kIdentity{1.0f, 0.0f, 0.0f, 0.0f};
} // namespace

int main() {
    using SFT::Engine::BodyCameraRig;
    using SFT::Engine::BodyCameraRigSettings;
    const float dt = 1.0f / 60.0f;

    // A steady aim leaves the camera exactly there.
    {
        BodyCameraRig rig{springs_only()};
        rig.reset({1, 2, 3}, kIdentity);
        for (int i = 0; i < 120; ++i) rig.update(dt, {1, 2, 3}, kIdentity, 0.0f);
        check(near(rig.position(), {1, 2, 3}, 1e-5f) && angle_between(rig.orientation(), kIdentity) < 1e-5f, "a steady aim leaves the camera put");
    }

    // Position step response: critically damped = approaches the target monotonically and never overshoots; settles.
    {
        BodyCameraRig rig{springs_only()};
        rig.reset({0, 0, 0}, kIdentity);
        float previous = 0.0f;
        bool monotonic = true, overshoot = false;
        for (int i = 0; i < 240; ++i) {
            rig.update(dt, {10, 0, 0}, kIdentity, 0.0f);
            const float x = rig.position().x;
            monotonic = monotonic && x >= previous - 1e-5f;
            overshoot = overshoot || x > 10.0f + 1e-3f;
            previous = x;
        }
        check(monotonic && !overshoot, "critically damped: monotonic approach, no overshoot");
        check(near(rig.position().x, 10.0f, 1e-2f), "settles on the target within 4 seconds");
        check(rig.position().x > 0.0f && near(rig.position().y, 0.0f), "stays on the line to the target");
    }

    // Underdamped springs do overshoot (the setting does something).
    {
        BodyCameraRigSettings s = springs_only();
        s.damping_ratio = 0.3f;
        BodyCameraRig rig{s};
        rig.reset({0, 0, 0}, kIdentity);
        bool overshoot = false;
        for (int i = 0; i < 240; ++i) {
            rig.update(dt, {10, 0, 0}, kIdentity, 0.0f);
            overshoot = overshoot || rig.position().x > 10.5f;
        }
        check(overshoot, "a damping ratio below 1 overshoots");
    }

    // Rotation lag: a 90 degree turn is followed gradually, takes the short way, and arrives.
    {
        BodyCameraRig rig{springs_only()};
        rig.reset({0, 0, 0}, kIdentity);
        const glm::quat target = glm::angleAxis(glm::half_pi<float>(), glm::vec3{0, 1, 0});
        rig.update(dt, {0, 0, 0}, target, 0.0f);
        const float early = angle_between(rig.orientation(), target);
        check(early > 1.0f && early < glm::half_pi<float>(), "one frame after a snap turn, the camera has barely begun to follow");
        float previous = early;
        bool closing = true;
        for (int i = 0; i < 180; ++i) {
            rig.update(dt, {0, 0, 0}, target, 0.0f);
            const float now = angle_between(rig.orientation(), target);
            closing = closing && now <= previous + 1e-4f;
            previous = now;
        }
        check(closing && previous < 1e-3f, "the camera closes on the aim monotonically and arrives");

        // 170 degrees one way vs 190 the other are the same short rotation (quaternion double cover).
        BodyCameraRig wrap{springs_only()};
        wrap.reset({0, 0, 0}, glm::angleAxis(glm::radians(10.0f), glm::vec3{0, 1, 0}));
        const glm::quat far_target = glm::angleAxis(glm::radians(-170.0f), glm::vec3{0, 1, 0}); // 180 degrees away, the short way is ambiguous
        const glm::quat near_target = glm::angleAxis(glm::radians(350.0f), glm::vec3{0, 1, 0}); // == -10 degrees, via the double cover
        wrap.update(dt, {0, 0, 0}, near_target, 0.0f);
        check(angle_between(wrap.orientation(), glm::angleAxis(glm::radians(10.0f), glm::vec3{0, 1, 0})) < glm::radians(5.0f),
              "an equivalent-by-sign target is a short turn, not a spin the long way");
        (void)far_target;
    }

    // Frame-rate independence: the same second of motion at 30, 60 and 240 Hz ends in nearly the same place.
    {
        glm::vec3 results[3];
        const float steps[3] = {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 240.0f};
        for (int k = 0; k < 3; ++k) {
            BodyCameraRig rig{springs_only()};
            rig.reset({0, 0, 0}, kIdentity);
            const int frames = static_cast<int>(std::round(0.5f / steps[k]));
            for (int i = 0; i < frames; ++i) rig.update(steps[k], {4, 1, -2}, kIdentity, 0.0f);
            results[k] = rig.position();
        }
        // The springs are solved in closed form, so a constant aim gives the same answer however the time is sliced.
        check(near(results[0], results[1], 1e-3f) && near(results[1], results[2], 1e-3f), "0.5 s of motion agrees to a millimetre across 30 / 60 / 240 Hz frames");
    }

    // Determinism: identical inputs, identical pose (including the wander).
    {
        BodyCameraRig a, b;
        a.reset({0, 1, 0}, kIdentity);
        b.reset({0, 1, 0}, kIdentity);
        for (int i = 0; i < 200; ++i) {
            a.update(dt, {float(i) * 0.01f, 1, 0}, kIdentity, 1.4f);
            b.update(dt, {float(i) * 0.01f, 1, 0}, kIdentity, 1.4f);
        }
        check(a.position() == b.position() && a.orientation() == b.orientation(), "the rig is deterministic");
    }

    // Gait: standing still has none; walking fades it in; the bob never dips below rest and repeats every stride.
    {
        BodyCameraRigSettings s;
        s.handheld_rotation_degrees = s.handheld_position = 0.0f;
        s.sway_amplitude = s.roll_amplitude_degrees = s.pitch_amplitude_degrees = 0.0f; // bob only
        BodyCameraRig rig{s};
        rig.reset({0, 1, 0}, kIdentity);
        for (int i = 0; i < 120; ++i) rig.update(dt, {0, 1, 0}, kIdentity, 0.0f);
        check(near(rig.position().y, 1.0f, 1e-5f) && near(rig.gait_amount(), 0.0f, 1e-5f), "no speed, no gait");

        float lowest = 10.0f, highest = -10.0f;
        for (int i = 0; i < 600; ++i) {
            rig.update(dt, {0, 1, 0}, kIdentity, 1.6f);
            if (i > 120) { // gait fully blended in
                lowest = std::min(lowest, rig.position().y);
                highest = std::max(highest, rig.position().y);
            }
        }
        check(near(rig.gait_amount(), 1.0f, 0.02f), "walking at the reference speed settles at gait 1");
        check(lowest >= 1.0f - 1e-4f, "the bob never dips below the rest height");
        check(near(highest - 1.0f, s.bob_amplitude, 0.003f), "the bob reaches its amplitude at the reference speed");

        // Stopping eases the gait out rather than cutting it.
        rig.update(dt, {0, 1, 0}, kIdentity, 0.0f);
        check(rig.gait_amount() > 0.5f, "after stopping, the gait fades out over a few frames, not instantly");
        for (int i = 0; i < 240; ++i) rig.update(dt, {0, 1, 0}, kIdentity, 0.0f);
        check(rig.gait_amount() < 0.01f, "...and is gone after a few seconds");
    }

    // Stride frequency follows speed / stride length: the bob (twice per stride) repeats at 2 * speed / length hertz.
    {
        BodyCameraRigSettings s;
        s.handheld_rotation_degrees = s.handheld_position = 0.0f;
        s.sway_amplitude = s.roll_amplitude_degrees = s.pitch_amplitude_degrees = 0.0f;
        s.stride_length = 1.0f;
        s.gait_blend_rate = 1000.0f; // gait is instantly "on" so only the frequency is under test
        BodyCameraRig rig{s};
        rig.reset({0, 0, 0}, kIdentity);
        const float speed = 2.0f; // 2 strides per second -> 4 bobs per second
        int peaks = 0;
        float previous = 0.0f, before = 0.0f;
        for (int i = 0; i < 480; ++i) { // 8 seconds
            rig.update(1.0f / 60.0f, {0, 0, 0}, kIdentity, speed);
            const float y = rig.position().y;
            if (i > 5 && previous > before && previous > y && previous > 0.5f * s.bob_amplitude) ++peaks;
            before = previous;
            previous = y;
        }
        check(peaks >= 30 && peaks <= 34, "at 2 m/s and a 1 m stride the bob peaks about 4 times a second");
    }

    // Handheld wander: small, smooth and bounded; different seeds differ.
    {
        BodyCameraRigSettings s = springs_only();
        s.handheld_rotation_degrees = 0.5f;
        s.handheld_position = 0.01f;
        BodyCameraRig rig{s};
        rig.reset({0, 0, 0}, kIdentity);
        float max_angle = 0.0f, max_offset = 0.0f, max_step = 0.0f;
        glm::quat previous = rig.orientation();
        for (int i = 0; i < 900; ++i) {
            rig.update(dt, {0, 0, 0}, kIdentity, 0.0f);
            max_angle = std::max(max_angle, angle_between(rig.orientation(), kIdentity));
            max_offset = std::max(max_offset, glm::length(rig.position()));
            max_step = std::max(max_step, angle_between(rig.orientation(), previous));
            previous = rig.orientation();
        }
        check(max_angle > glm::radians(0.05f) && max_angle < glm::radians(1.2f), "the wander moves the camera a fraction of a degree");
        check(max_offset > 0.0005f && max_offset < 0.03f, "...and a few millimetres");
        check(max_step < glm::radians(0.05f), "...smoothly (no frame-to-frame jumps)");
        BodyCameraRigSettings other = s;
        other.seed = 7;
        BodyCameraRig second{other};
        second.reset({0, 0, 0}, kIdentity);
        for (int i = 0; i < 300; ++i) second.update(dt, {0, 0, 0}, kIdentity, 0.0f);
        BodyCameraRig first{s};
        first.reset({0, 0, 0}, kIdentity);
        for (int i = 0; i < 300; ++i) first.update(dt, {0, 0, 0}, kIdentity, 0.0f);
        check(!(first.position() == second.position()), "different seeds wander differently");
    }

    // Impulses: a kick moves the camera and the springs absorb it.
    {
        BodyCameraRig rig{springs_only()};
        rig.reset({0, 0, 0}, kIdentity);
        rig.add_translation_impulse({0, 3, 0});
        rig.add_rotation_impulse({0.5f, 0, 0});
        float peak = 0.0f, peak_angle = 0.0f;
        for (int i = 0; i < 60; ++i) {
            rig.update(dt, {0, 0, 0}, kIdentity, 0.0f);
            peak = std::max(peak, rig.position().y);
            peak_angle = std::max(peak_angle, angle_between(rig.orientation(), kIdentity));
        }
        // A critically damped spring kicked with velocity v0 peaks at v0 / (omega * e).
        const float expected_peak = 3.0f / (glm::two_pi<float>() * 9.0f * 2.71828f);
        const float expected_angle = 0.5f / (glm::two_pi<float>() * 5.0f * 2.71828f);
        check(near(peak, expected_peak, 0.15f * expected_peak) && near(peak_angle, expected_angle, 0.15f * expected_angle),
              "an impulse kicks the camera by v0 / (omega * e), as a critically damped spring should");
        for (int i = 0; i < 300; ++i) rig.update(dt, {0, 0, 0}, kIdentity, 0.0f);
        check(near(rig.position(), {0, 0, 0}, 1e-3f) && angle_between(rig.orientation(), kIdentity) < 1e-3f, "...and the springs bring it back");
        rig.add_translation_impulse({NAN, 0, 0});
        check(std::isfinite(rig.position().x), "a non-finite impulse is ignored");
    }

    // Bad input is ignored for that call; a long hitch snaps rather than integrating.
    {
        BodyCameraRig rig{springs_only()};
        rig.reset({1, 1, 1}, kIdentity);
        rig.update(dt, {NAN, 0, 0}, kIdentity, 1.0f);
        rig.update(dt, {1, 1, 1}, glm::quat{0, 0, 0, 0}, 1.0f);
        rig.update(-1.0, {5, 5, 5}, kIdentity, 1.0f);
        check(near(rig.position(), {1, 1, 1}, 1e-5f), "NaN targets, a zero quaternion and a negative dt are ignored");
        rig.update(2.0, {50, 0, 0}, kIdentity, 0.0f);
        check(near(rig.followed_position(), {50, 0, 0}, 1e-5f), "a 2 second hitch snaps to the aim instead of simulating");
    }

    // pose_matrix agrees with position() / orientation().
    {
        BodyCameraRig rig;
        rig.reset({1, 2, 3}, glm::angleAxis(0.7f, glm::normalize(glm::vec3{1, 1, 0})));
        for (int i = 0; i < 30; ++i) rig.update(dt, {1, 2, 3}, rig.followed_orientation(), 1.0f);
        const glm::mat4 pose = rig.pose_matrix();
        check(near(glm::vec3{pose[3]}, rig.position(), 1e-5f), "the pose matrix carries the position");
        const glm::vec3 forward = glm::vec3{pose * glm::vec4{0, 0, -1, 0}};
        const glm::vec3 expected = rig.orientation() * glm::vec3{0, 0, -1};
        check(near(forward, expected, 1e-4f), "...and the orientation");
    }

    if (failures == 0) std::printf("BodyCameraRigTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
