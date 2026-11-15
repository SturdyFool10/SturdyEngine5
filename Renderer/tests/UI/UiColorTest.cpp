// CSS colour spaces (UI/UiColor.hpp): conversions against published reference values and the spec's own worked number, round trips through
// every space, the four hue-interpolation methods, premultiplied-alpha interpolation and powerless hue. CPU only.

#include <Renderer/UI/UiColor.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace SFT;
using UI::ColorSpace;
using UI::HueInterpolation;

namespace {

    int failures = 0;

    void check(bool ok, const char *what, float a = 0.0f, float b = 0.0f, float c = 0.0f) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s (%.5f %.5f %.5f)\n", what, a, b, c);
            ++failures;
        }
    }

    bool near3(glm::vec3 v, glm::vec3 expected, float tolerance) {
        return std::abs(v.x - expected.x) <= tolerance && std::abs(v.y - expected.y) <= tolerance && std::abs(v.z - expected.z) <= tolerance;
    }

    float hue_distance(float a, float b) {
        float d = std::fmod(std::abs(a - b), 360.0f);
        return d > 180.0f ? 360.0f - d : d;
    }

    void reference_values() {
        // The one figure CSS Color 4 itself prints for this: xyz-d65 [1,1,1] is lab(100.1154% 9.064489 5.801761).
        const glm::vec3 lab = UI::convert(glm::vec3{1.0f, 1.0f, 1.0f}, ColorSpace::XyzD65, ColorSpace::Lab);
        check(near3(lab, {100.1154f, 9.064489f, 5.801761f}, 0.01f), "xyz-d65 white in lab (spec figure)", lab.x, lab.y, lab.z);

        const glm::vec3 white_lab = UI::convert(glm::vec3{1.0f}, ColorSpace::Srgb, ColorSpace::Lab);
        check(near3(white_lab, {100.0f, 0.0f, 0.0f}, 0.05f), "sRGB white is lab 100 0 0", white_lab.x, white_lab.y, white_lab.z);

        const glm::vec3 white_oklab = UI::convert(glm::vec3{1.0f}, ColorSpace::Srgb, ColorSpace::Oklab);
        check(near3(white_oklab, {1.0f, 0.0f, 0.0f}, 0.001f), "sRGB white is oklab 1 0 0", white_oklab.x, white_oklab.y, white_oklab.z);

        const glm::vec3 red{1.0f, 0.0f, 0.0f};
        const glm::vec3 red_oklab = UI::convert(red, ColorSpace::Srgb, ColorSpace::Oklab);
        check(near3(red_oklab, {0.62796f, 0.22486f, 0.12585f}, 0.001f), "sRGB red in oklab", red_oklab.x, red_oklab.y, red_oklab.z);
        const glm::vec3 red_lab = UI::convert(red, ColorSpace::Srgb, ColorSpace::Lab);
        check(near3(red_lab, {54.29f, 80.80f, 69.89f}, 0.06f), "sRGB red in lab (D50)", red_lab.x, red_lab.y, red_lab.z);
        const glm::vec3 red_p3 = UI::convert(red, ColorSpace::Srgb, ColorSpace::DisplayP3);
        check(near3(red_p3, {0.9175f, 0.2003f, 0.1386f}, 0.001f), "sRGB red in display-p3", red_p3.x, red_p3.y, red_p3.z);
        const glm::vec3 red_hsl = UI::convert(red, ColorSpace::Srgb, ColorSpace::Hsl);
        check(hue_distance(red_hsl.x, 0.0f) < 0.01f && near3(glm::vec3{0.0f, red_hsl.y, red_hsl.z}, {0.0f, 100.0f, 50.0f}, 0.01f),
              "sRGB red in hsl", red_hsl.x, red_hsl.y, red_hsl.z);
        const glm::vec3 green_hsl = UI::convert(glm::vec3{0.0f, 1.0f, 0.0f}, ColorSpace::Srgb, ColorSpace::Hsl);
        check(near3(green_hsl, {120.0f, 100.0f, 50.0f}, 0.01f), "sRGB green in hsl", green_hsl.x, green_hsl.y, green_hsl.z);
        const glm::vec3 red_oklch = UI::convert(red, ColorSpace::Srgb, ColorSpace::Oklch);
        check(near3(red_oklch, {0.62796f, 0.25768f, 29.23f}, 0.01f), "sRGB red in oklch", red_oklch.x, red_oklch.y, red_oklch.z);
        // Rec.2020 and a98 saturate differently: sRGB red is inside both gamuts, so its coordinates are below 1 in each.
        const glm::vec3 red_2020 = UI::convert(red, ColorSpace::Srgb, ColorSpace::Rec2020);
        check(red_2020.x < 1.0f && red_2020.x > 0.7f && red_2020.y > 0.0f && red_2020.z > 0.0f, "sRGB red sits inside rec2020", red_2020.x, red_2020.y, red_2020.z);
    }

    void round_trips() {
        const glm::vec3 original{0.2f, 0.5f, 0.8f};
        const std::array spaces{ColorSpace::Srgb, ColorSpace::SrgbLinear, ColorSpace::DisplayP3, ColorSpace::DisplayP3Linear,
                                ColorSpace::A98Rgb, ColorSpace::ProphotoRgb, ColorSpace::Rec2020, ColorSpace::XyzD50,
                                ColorSpace::XyzD65, ColorSpace::Lab, ColorSpace::Oklab, ColorSpace::Hsl, ColorSpace::Hwb,
                                ColorSpace::Lch, ColorSpace::Oklch};
        for (const ColorSpace space : spaces) {
            const glm::vec3 there = UI::convert(original, ColorSpace::Srgb, space);
            const glm::vec3 back = UI::convert(there, space, ColorSpace::Srgb);
            check(near3(back, original, 2.0e-4f), UI::css_name(space), back.x, back.y, back.z);
        }
        // Out-of-gamut values survive (nothing is clamped): a vivid P3 green is outside sRGB and comes back from it unchanged.
        const glm::vec3 p3_green{0.0f, 1.0f, 0.0f};
        const glm::vec3 as_srgb = UI::convert(p3_green, ColorSpace::DisplayP3, ColorSpace::Srgb);
        check(as_srgb.x < 0.0f, "display-p3 green is outside sRGB (negative red) and not clamped", as_srgb.x, as_srgb.y, as_srgb.z);
        check(near3(UI::convert(as_srgb, ColorSpace::Srgb, ColorSpace::DisplayP3), p3_green, 2.0e-4f), "an out-of-gamut value round-trips");
    }

    void hue_methods() {
        const glm::vec3 a{0.7f, 0.15f, 350.0f};
        const glm::vec3 b{0.7f, 0.15f, 10.0f};
        const auto middle_hue = [&](HueInterpolation method) {
            return UI::mix(a, b, 0.5f, UI::ColorInterpolation{ColorSpace::Oklch, method}, ColorSpace::Oklch, ColorSpace::Oklch).z;
        };
        check(hue_distance(middle_hue(HueInterpolation::Shorter), 0.0f) < 0.01f, "shorter: 350 to 10 passes through 0", middle_hue(HueInterpolation::Shorter));
        check(hue_distance(middle_hue(HueInterpolation::Longer), 180.0f) < 0.01f, "longer: 350 to 10 goes the long way, through 180", middle_hue(HueInterpolation::Longer));
        check(hue_distance(middle_hue(HueInterpolation::Increasing), 0.0f) < 0.01f, "increasing: 350 up to 10 passes through 0", middle_hue(HueInterpolation::Increasing));
        check(hue_distance(middle_hue(HueInterpolation::Decreasing), 180.0f) < 0.01f, "decreasing: 350 down to 10 passes through 180", middle_hue(HueInterpolation::Decreasing));
        // Endpoints are exact.
        const glm::vec3 start = UI::mix(a, b, 0.0f, UI::ColorInterpolation{ColorSpace::Oklch, HueInterpolation::Shorter}, ColorSpace::Oklch, ColorSpace::Oklch);
        check(near3(start, a, 1.0e-3f) || hue_distance(start.z, a.z) < 0.01f, "t = 0 gives the first colour", start.x, start.y, start.z);
    }

    void alpha_and_powerless() {
        // CSS premultiplies: a fully transparent end contributes alpha, not colour, so red fading to transparent blue stays red.
        const glm::vec4 mixed = UI::mix(glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}, glm::vec4{0.0f, 0.0f, 1.0f, 0.0f}, 0.5f, ColorSpace::Srgb);
        check(std::abs(mixed.w - 0.5f) < 1.0e-5f && near3(glm::vec3{mixed}, {1.0f, 0.0f, 0.0f}, 1.0e-4f),
              "premultiplied: red to transparent blue stays red at half alpha", mixed.x, mixed.y, mixed.z);

        // A grey has no hue in a polar space: mixing it with a colour keeps that colour's hue instead of sweeping the wheel.
        const glm::vec3 red{1.0f, 0.0f, 0.0f};
        const glm::vec3 grey{0.5f, 0.5f, 0.5f};
        const glm::vec3 mid = UI::mix(grey, red, 0.5f, UI::ColorInterpolation{ColorSpace::Oklch}, ColorSpace::Srgb, ColorSpace::Oklch);
        const glm::vec3 red_oklch = UI::convert(red, ColorSpace::Srgb, ColorSpace::Oklch);
        check(hue_distance(mid.z, red_oklch.z) < 0.5f, "powerless hue: grey to red keeps red's hue", mid.z, red_oklch.z);

        // Oklab is the default and differs visibly from sRGB for red to blue (the sRGB midpoint is a dark purple, the Oklab one brighter).
        const glm::vec3 blue{0.0f, 0.0f, 1.0f};
        const glm::vec3 oklab_mid = UI::mix(red, blue, 0.5f);
        const glm::vec3 srgb_mid = UI::mix(red, blue, 0.5f, ColorSpace::Srgb);
        check(near3(srgb_mid, {0.5f, 0.0f, 0.5f}, 1.0e-4f), "srgb midpoint of red and blue", srgb_mid.x, srgb_mid.y, srgb_mid.z);
        check(oklab_mid.x > 0.5f + 0.02f, "the default (oklab) mix differs from the srgb mix", oklab_mid.x, oklab_mid.y, oklab_mid.z);
    }

} // namespace

int main() {
    reference_values();
    round_trips();
    hue_methods();
    alpha_and_powerless();
    if (failures != 0) {
        std::fprintf(stderr, "UiColorTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("UiColorTest: ok\n");
    return 0;
}
