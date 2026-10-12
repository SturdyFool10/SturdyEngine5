// CPU-only checks of the colour-grade setup (Renderer/ColorGrade.cpp): white balance, the constants the shader reads, gating.

#include <Renderer/ColorGrade.hpp>

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
    bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
    float luma(const glm::vec3 &c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
} // namespace

int main() {
    using namespace SFT::Renderer;

    // White balance: neutral is identity; warm lifts red and lowers blue; magenta lowers green; luminance is preserved.
    {
        const glm::vec3 neutral = white_balance_gain(0.0f, 0.0f);
        check(near(neutral.x, 1.0f) && near(neutral.y, 1.0f) && near(neutral.z, 1.0f), "no temperature or tint changes nothing");
        const glm::vec3 warm = white_balance_gain(1.0f, 0.0f);
        check(warm.x > 1.0f && warm.z < 1.0f && near(luma(warm), 1.0f), "warm raises red, lowers blue, keeps luminance");
        const glm::vec3 cool = white_balance_gain(-1.0f, 0.0f);
        check(cool.x < 1.0f && cool.z > 1.0f && near(luma(cool), 1.0f), "cool is the mirror");
        const glm::vec3 magenta = white_balance_gain(0.0f, 1.0f);
        check(magenta.y < magenta.x && near(luma(magenta), 1.0f), "magenta tint lowers green");
        check(near(white_balance_gain(5.0f, 0.0f).x, white_balance_gain(1.0f, 0.0f).x), "out-of-range temperature is clamped");
        const glm::vec3 broken = white_balance_gain(NAN, INFINITY);
        check(std::isfinite(broken.x) && std::isfinite(broken.y) && std::isfinite(broken.z), "non-finite input stays finite");
    }

    // Defaults are the identity grade.
    {
        const SFT::RenderSettings::ColorGradeSettings neutral;
        const ColorGradeConstants c = make_color_grade_constants(neutral, 1920, 1080, 0);
        check(c.extent.x == 1920 && c.extent.y == 1080 && c.extent.z == 0, "extent and LUT size are passed through");
        check(near(c.white_balance.x, 1.0f) && near(c.white_balance.y, 1.0f) && near(c.white_balance.z, 1.0f) && near(c.white_balance.w, 1.0f),
              "default white balance and exposure are neutral");
        check(near(c.look.x, 1.0f) && near(c.look.y, 1.0f), "default contrast and saturation are 1");
        check(near(c.look.w, 0.0f), "without a LUT the LUT intensity is 0");
        check(near(c.inverse_gamma.x, 1.0f) && near(c.gain.y, 1.0f) && near(c.lift.z, 0.0f), "default lift / gamma / gain are neutral");
    }

    // Settings map onto constants: exposure in stops, gamma inverted, tints and gains clamped non-negative, LUT intensity gated by a LUT.
    {
        SFT::RenderSettings::ColorGradeSettings s;
        s.exposure_ev = 2.0f;
        s.gamma[0] = 2.0f;
        s.gain[1] = -3.0f;
        s.shadow_tint[2] = -1.0f;
        s.contrast = 99.0f;
        s.lut_intensity = 0.5f;
        const ColorGradeConstants without_lut = make_color_grade_constants(s, 64, 64, 0);
        const ColorGradeConstants with_lut = make_color_grade_constants(s, 64, 64, 33);
        check(near(without_lut.white_balance.w, 4.0f), "+2 EV is a gain of 4");
        check(near(without_lut.inverse_gamma.x, 0.5f), "gamma 2 is stored as 1/2");
        check(near(without_lut.gain.y, 0.0f) && near(without_lut.shadow_tint.z, 0.0f), "negative gains and tints are clamped to 0");
        check(near(without_lut.look.x, 4.0f), "contrast is capped at 4");
        check(near(without_lut.look.w, 0.0f) && near(with_lut.look.w, 0.5f) && with_lut.extent.z == 33, "the LUT intensity applies only with a LUT");
    }

    // Non-finite values fall back to neutral instead of reaching the GPU.
    {
        SFT::RenderSettings::ColorGradeSettings s;
        s.exposure_ev = NAN;
        s.saturation = INFINITY;
        s.lift[0] = NAN;
        s.gamma[1] = NAN;
        const ColorGradeConstants c = make_color_grade_constants(s, 8, 8, 0);
        check(near(c.white_balance.w, 1.0f) && near(c.look.y, 1.0f) && near(c.lift.x, 0.0f) && near(c.inverse_gamma.y, 1.0f),
              "NaN / infinity become the neutral value");
    }

    // Gating.
    {
        RenderGraphSettings settings;
        check(!color_grade_active(settings), "colour grading is off by default");
        settings.frame.color_grade.enabled = true;
        settings.render_scene = false;
        check(!color_grade_active(settings), "it needs a scene");
        settings.render_scene = true;
        check(color_grade_active(settings), "enabled with a scene");
    }

    if (failures == 0) std::printf("ColorGradeTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
