// CPU-only checks of the depth-of-field optics (Renderer/DepthOfField.cpp): the thin-lens circle of confusion the shaders evaluate,
// and the gating of the feature.

#include <Renderer/DepthOfField.hpp>

#include <glm/gtc/matrix_transform.hpp>

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
    bool near(float a, float b, float relative = 1e-3f) { return std::abs(a - b) <= relative * std::max(1.0f, std::abs(b)); }
} // namespace

int main() {
    using namespace SFT::Renderer;

    SFT::RenderSettings::DepthOfFieldSettings lens;
    lens.focal_length_mm = 50.0f;
    lens.aperture_f_stop = 2.0f;
    lens.focus_distance = 5.0f;
    lens.sensor_height_mm = 24.0f;
    constexpr float kHeight = 1080.0f;

    // Hand-computed: d = 5000 mm, f = 50 mm, N = 2 -> the disc is f^2 / (N (d - f)) = 2500 / (2 * 4950) = 0.252525 mm for
    // |z - d| / z = 1, i.e. 0.252525 * 1080 / 24 = 11.3636 px.
    const float unit = 11.363636f;
    check(near(circle_of_confusion_px(lens, 5.0f, kHeight), 0.0f), "the focal plane is perfectly sharp");
    check(near(circle_of_confusion_px(lens, 2.5f, kHeight), -unit), "half the focus distance: |z - d| / z = 1, near field (negative)");
    check(near(circle_of_confusion_px(lens, 10.0f, kHeight), 0.5f * unit), "twice the focus distance: |z - d| / z = 0.5, far field (positive)");
    check(near(circle_of_confusion_px(lens, 1.0e6f, kHeight), unit, 1e-3f), "infinity approaches the full scale (the sky blurs the most a far object can)");

    // Scaling laws: the blur halves with the aperture's f-number doubling, doubles with the strength, scales with image height.
    SFT::RenderSettings::DepthOfFieldSettings narrow = lens;
    narrow.aperture_f_stop = 4.0f;
    check(near(circle_of_confusion_px(narrow, 10.0f, kHeight), 0.5f * circle_of_confusion_px(lens, 10.0f, kHeight)),
          "stopping down from f/2 to f/4 halves the blur");
    SFT::RenderSettings::DepthOfFieldSettings strong = lens;
    strong.strength = 2.0f;
    check(near(circle_of_confusion_px(strong, 10.0f, kHeight), 2.0f * circle_of_confusion_px(lens, 10.0f, kHeight)), "strength scales the blur");
    check(near(circle_of_confusion_px(lens, 10.0f, 2.0f * kHeight), 2.0f * circle_of_confusion_px(lens, 10.0f, kHeight)),
          "the blur scales with the image height in pixels");
    SFT::RenderSettings::DepthOfFieldSettings long_lens = lens;
    long_lens.focal_length_mm = 100.0f;
    check(circle_of_confusion_px(long_lens, 10.0f, kHeight) > 3.5f * circle_of_confusion_px(lens, 10.0f, kHeight),
          "a longer lens blurs much more (about f squared)");

    // Degenerate inputs stay finite.
    SFT::RenderSettings::DepthOfFieldSettings broken = lens;
    broken.focus_distance = 0.01f; // closer than the focal length: unreachable focus
    check(std::isfinite(circle_of_confusion_px(broken, 3.0f, kHeight)), "an unreachable focus distance stays finite");
    broken.aperture_f_stop = NAN;
    broken.focal_length_mm = INFINITY;
    check(std::isfinite(circle_of_confusion_px(broken, 3.0f, kHeight)), "non-finite lens values fall back to defaults");

    // Sample counts per quality level.
    check(depth_of_field_sample_count(0) == 24 && depth_of_field_sample_count(1) == 40 && depth_of_field_sample_count(2) == 64 &&
              depth_of_field_sample_count(99) == 64,
          "quality maps to 24 / 40 / 64 samples (clamped)");

    // Gating: off by default; on needs a scene and a perspective camera.
    {
        RenderGraphSettings settings;
        CameraView perspective;
        perspective.projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        check(!depth_of_field_active(settings, perspective), "depth of field is off by default");
        settings.frame.depth_of_field.enabled = true;
        settings.render_scene = true;
        check(depth_of_field_active(settings, perspective), "enabled with a scene and a perspective camera");
        settings.render_scene = false;
        check(!depth_of_field_active(settings, perspective), "no scene, no depth of field");
        settings.render_scene = true;
        CameraView orthographic;
        orthographic.projection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
        check(!depth_of_field_active(settings, orthographic), "an orthographic camera has no depth of field");
    }

    if (failures == 0) std::printf("DepthOfFieldTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
