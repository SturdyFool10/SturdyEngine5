#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <glm/mat3x3.hpp>
#include <limits>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <numbers>
#pragma endregion

// CSS colour spaces for the UI: conversion between them and interpolation in them, as CSS Color Module Level 4 (13 Nov 2026 draft)
// defines both. Colours are the vector types shaders use: a `glm::vec3` is three components in a named space, a `glm::vec4` adds straight
// (non-premultiplied) alpha as the last component, and every function has both forms. `Shaders/color_spaces.slang` is the same code
// for the GPU; `UiColorTest` and `UiEffectGpuTest` check the two agree.
//
// Component conventions are the CSS reference ranges, because that is what published values (and every colour a web developer knows)
// use:
//
//   space             components                         notes
//   srgb              r g b            0..1              gamma encoded; values outside 0..1 are legal and kept
//   srgb-linear       r g b            0..1              the same primaries without the transfer function
//   display-p3        r g b            0..1              P3 primaries, sRGB transfer function
//   display-p3-linear r g b            0..1
//   a98-rgb           r g b            0..1              Adobe RGB (1998), gamma 563/256
//   prophoto-rgb      r g b            0..1              D50 white, gamma 1.8 with a linear toe
//   rec2020           r g b            0..1              gamma 2.4
//   xyz-d50, xyz-d65  x y z            Y = 1 is white
//   lab               L a b            L 0..100, a/b about -125..125     D50
//   oklab             L a b            L 0..1,   a/b about -0.4..0.4     D65
//   hsl               H S L            H degrees [0,360), S and L 0..100
//   hwb               H W B            H degrees [0,360), W and B 0..100
//   lch               L C H            L 0..100, C 0..~150, H degrees    polar form of lab
//   oklch             L C H            L 0..1,   C 0..~0.4, H degrees    polar form of oklab
//
// Nothing is clamped or gamut-mapped by a conversion or an interpolation; only the final write to a render target clips. Not implemented:
// CSS's carry-forward of `none` components between *analogous* components of different spaces. A powerless hue (a grey in a polar space)
// is handled: it takes the other colour's hue.
namespace SFT::UI {

    enum class ColorSpace : u8 {
        // rectangular
        Srgb,
        SrgbLinear,
        DisplayP3,
        DisplayP3Linear,
        A98Rgb,
        ProphotoRgb,
        Rec2020,
        XyzD50,
        XyzD65,
        Lab,
        Oklab,
        // polar
        Hsl,
        Hwb,
        Lch,
        Oklch,
        // aliases: `Linear` is the name this engine's blend setting used before; `Xyz` is CSS's `xyz`, an alias of `xyz-d65`
        Linear = SrgbLinear,
        Xyz = XyzD65,
    };

    /// How the hue is turned around the colour wheel when interpolating in a polar space (CSS `<hue-interpolation-method>`).
    enum class HueInterpolation : u8 {
        Shorter,    ///< the shorter arc; the CSS default
        Longer,     ///< the longer arc
        Increasing, ///< always increasing from the first hue to the second
        Decreasing, ///< always decreasing
    };

    /// CSS `<color-interpolation-method>`: the space colours are mixed in, and for a polar space how the hue travels. Oklab is the default
    /// everywhere in this engine (CSS's own default for host syntaxes that do not set one).
    struct ColorInterpolation {
        ColorSpace space = ColorSpace::Oklab;
        HueInterpolation hue = HueInterpolation::Shorter;

        friend constexpr bool operator==(const ColorInterpolation &, const ColorInterpolation &) noexcept = default;
    };

    /// The old name of `ColorSpace` from when only a handful of blend spaces existed. Same type, so existing code keeps compiling.
    using ColorBlendSpace = ColorSpace;

    [[nodiscard]] constexpr bool is_polar(ColorSpace space) noexcept {
        return space == ColorSpace::Hsl || space == ColorSpace::Hwb || space == ColorSpace::Lch || space == ColorSpace::Oklch;
    }

    /// Which component of a polar space is the hue (HSL, HWB: 0; LCH, OKLCH: 2). Meaningless for a rectangular space.
    [[nodiscard]] constexpr int hue_component(ColorSpace space) noexcept {
        return (space == ColorSpace::Lch || space == ColorSpace::Oklch) ? 2 : 0;
    }

    /// CSS identifier of a space (`oklab`, `display-p3`, ...), for diagnostics and for parsing/serialising.
    [[nodiscard]] constexpr const char *css_name(ColorSpace space) noexcept {
        switch (space) {
            case ColorSpace::Srgb: return "srgb";
            case ColorSpace::SrgbLinear: return "srgb-linear";
            case ColorSpace::DisplayP3: return "display-p3";
            case ColorSpace::DisplayP3Linear: return "display-p3-linear";
            case ColorSpace::A98Rgb: return "a98-rgb";
            case ColorSpace::ProphotoRgb: return "prophoto-rgb";
            case ColorSpace::Rec2020: return "rec2020";
            case ColorSpace::XyzD50: return "xyz-d50";
            case ColorSpace::XyzD65: return "xyz-d65";
            case ColorSpace::Lab: return "lab";
            case ColorSpace::Oklab: return "oklab";
            case ColorSpace::Hsl: return "hsl";
            case ColorSpace::Hwb: return "hwb";
            case ColorSpace::Lch: return "lch";
            case ColorSpace::Oklch: return "oklch";
        }
        return "unknown";
    }

    namespace Detail {

        using dvec3 = glm::dvec3;
        using dmat3 = glm::dmat3;

        /// A matrix written the way the spec writes it: row by row.
        [[nodiscard]] inline dmat3 rows(double a, double b, double c, double d, double e, double f, double g, double h, double i) noexcept {
            return dmat3{a, d, g, b, e, h, c, f, i};
        }

        // CSS Color 4 section 19 sample code, verbatim. All "to XYZ" matrices act on linear light.
        inline constexpr double kD50[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
        inline constexpr double kD65[3] = {0.3127 / 0.3290, 1.0, (1.0 - 0.3127 - 0.3290) / 0.3290};

        inline const dmat3 kLinSrgbToXyz = rows(506752.0 / 1228815.0, 87881.0 / 245763.0, 12673.0 / 70218.0,
                                                    87098.0 / 409605.0, 175762.0 / 245763.0, 12673.0 / 175545.0,
                                                    7918.0 / 409605.0, 87881.0 / 737289.0, 1001167.0 / 1053270.0);
        inline const dmat3 kXyzToLinSrgb = rows(12831.0 / 3959.0, -329.0 / 214.0, -1974.0 / 3959.0,
                                                    -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0,
                                                    705.0 / 12673.0, -2585.0 / 12673.0, 705.0 / 667.0);
        inline const dmat3 kLinP3ToXyz = rows(608311.0 / 1250200.0, 189793.0 / 714400.0, 198249.0 / 1000160.0,
                                                  35783.0 / 156275.0, 247089.0 / 357200.0, 198249.0 / 2500400.0,
                                                  0.0, 32229.0 / 714400.0, 5220557.0 / 5000800.0);
        inline const dmat3 kXyzToLinP3 = rows(446124.0 / 178915.0, -333277.0 / 357830.0, -72051.0 / 178915.0,
                                                  -14852.0 / 17905.0, 63121.0 / 35810.0, 423.0 / 17905.0,
                                                  11844.0 / 330415.0, -50337.0 / 660830.0, 316169.0 / 330415.0);
        inline const dmat3 kLinA98ToXyz = rows(573536.0 / 994567.0, 263643.0 / 1420810.0, 187206.0 / 994567.0,
                                                   591459.0 / 1989134.0, 6239551.0 / 9945670.0, 374412.0 / 4972835.0,
                                                   53769.0 / 1989134.0, 351524.0 / 4972835.0, 4929758.0 / 4972835.0);
        inline const dmat3 kXyzToLinA98 = rows(1829569.0 / 896150.0, -506331.0 / 896150.0, -308931.0 / 896150.0,
                                                   -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0,
                                                   16779.0 / 1248040.0, -147721.0 / 1248040.0, 1266979.0 / 1248040.0);
        inline const dmat3 kLinProphotoToXyz = rows(0.79776664490064230, 0.13518129740053308, 0.03134773412839220,
                                                        0.28807482881940130, 0.71183523424187300, 0.00008993693872564,
                                                        0.0, 0.0, 0.82510460251046020);
        inline const dmat3 kXyzToLinProphoto = rows(1.34578688164715830, -0.25557208737979464, -0.05110186497554526,
                                                        -0.54463070512490190, 1.50824774284514680, 0.02052744743642139,
                                                        0.0, 0.0, 1.21196754563894520);
        inline const dmat3 kLin2020ToXyz = rows(63426534.0 / 99577255.0, 20160776.0 / 139408157.0, 47086771.0 / 278816314.0,
                                                    26158966.0 / 99577255.0, 472592308.0 / 697040785.0, 8267143.0 / 139408157.0,
                                                    0.0, 19567812.0 / 697040785.0, 295819943.0 / 278816314.0);
        inline const dmat3 kXyzToLin2020 = rows(30757411.0 / 17917100.0, -6372589.0 / 17917100.0, -4539589.0 / 17917100.0,
                                                    -19765991.0 / 29648200.0, 47925759.0 / 29648200.0, 467509.0 / 29648200.0,
                                                    792561.0 / 44930125.0, -1921689.0 / 44930125.0, 42328811.0 / 44930125.0);
        // Bradford chromatic adaptation between the two whites.
        inline const dmat3 kD65ToD50 = rows(1.0479297925449969, 0.022946870601609652, -0.05019226628920524,
                                                0.02962780877005599, 0.9904344267538799, -0.017073799063418826,
                                                -0.009243040646204504, 0.015055191490298152, 0.7518742814281371);
        inline const dmat3 kD50ToD65 = rows(0.955473421488075, -0.02309845494876471, 0.06325924320057072,
                                                -0.0283697093338637, 1.0099953980813041, 0.021041441191917323,
                                                0.012314014864481998, -0.020507649298898964, 1.330365926242124);
        // OKLab (D65).
        inline const dmat3 kXyzToLms = rows(0.8190224379967030, 0.3619062600528904, -0.1288737815209879,
                                                0.0329836539323885, 0.9292868615863434, 0.0361446663506424,
                                                0.0481771893596242, 0.2642395317527308, 0.6335478284694309);
        inline const dmat3 kLmsToOklab = rows(0.2104542683093140, 0.7936177747023054, -0.0040720430116193,
                                                  1.9779985324311684, -2.4285922420485799, 0.4505937096174110,
                                                  0.0259040424655478, 0.7827717124575296, -0.8086757549230774);
        inline const dmat3 kLmsToXyz = rows(1.2268798758459243, -0.5578149944602171, 0.2813910456659647,
                                                -0.0405757452148008, 1.1122868032803170, -0.0717110580655164,
                                                -0.0763729366746601, -0.4214933324022432, 1.5869240198367816);
        inline const dmat3 kOklabToLms = rows(1.0000000000000000, 0.3963377773761749, 0.2158037573099136,
                                                  1.0000000000000000, -0.1055613458156586, -0.0638541728258133,
                                                  1.0000000000000000, -0.0894841775298119, -1.2914855480194092);

        [[nodiscard]] inline double sign_of(double v) noexcept { return v < 0.0 ? -1.0 : 1.0; }

        // Transfer functions (decode = "lin_*", encode = "gam_*"), sign-preserving so out-of-gamut values survive a round trip.
        [[nodiscard]] inline double decode_srgb(double v) noexcept {
            const double a = std::abs(v);
            return a <= 0.04045 ? v / 12.92 : sign_of(v) * std::pow((a + 0.055) / 1.055, 2.4);
        }
        [[nodiscard]] inline double encode_srgb(double v) noexcept {
            const double a = std::abs(v);
            return a > 0.0031308 ? sign_of(v) * (1.055 * std::pow(a, 1.0 / 2.4) - 0.055) : 12.92 * v;
        }
        [[nodiscard]] inline double decode_power(double v, double gamma) noexcept { return sign_of(v) * std::pow(std::abs(v), gamma); }
        [[nodiscard]] inline double decode_prophoto(double v) noexcept {
            const double a = std::abs(v);
            return a <= 16.0 / 512.0 ? v / 16.0 : sign_of(v) * std::pow(a, 1.8);
        }
        [[nodiscard]] inline double encode_prophoto(double v) noexcept {
            const double a = std::abs(v);
            return a >= 1.0 / 512.0 ? sign_of(v) * std::pow(a, 1.0 / 1.8) : 16.0 * v;
        }

        template <typename Fn>
        [[nodiscard]] inline dvec3 map3(dvec3 v, Fn fn) noexcept {
            return dvec3{fn(v.x), fn(v.y), fn(v.z)};
        }

        inline constexpr double kLabEpsilon = 216.0 / 24389.0;
        inline constexpr double kLabKappa = 24389.0 / 27.0;
        inline constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

        [[nodiscard]] inline dvec3 hsl_to_srgb(dvec3 hsl) noexcept {
            double hue = std::fmod(hsl.x, 360.0);
            if (hue < 0.0) hue += 360.0;
            const double sat = hsl.y / 100.0;
            const double light = hsl.z / 100.0;
            const auto f = [&](double n) {
                const double k = std::fmod(n + hue / 30.0, 12.0);
                const double a = sat * std::min(light, 1.0 - light);
                return light - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
            };
            return dvec3{f(0.0), f(8.0), f(4.0)};
        }

        /// The spec's `rgbToHsl`, with a powerless hue (grey) reported as NaN like the reference code does.
        [[nodiscard]] inline dvec3 srgb_to_hsl(dvec3 rgb) noexcept {
            const double mx = std::max({rgb.x, rgb.y, rgb.z});
            const double mn = std::min({rgb.x, rgb.y, rgb.z});
            double hue = std::numeric_limits<double>::quiet_NaN();
            double sat = 0.0;
            const double light = (mn + mx) / 2.0;
            const double d = mx - mn;
            constexpr double epsilon = 1.0 / 100000.0;
            if (d != 0.0) {
                sat = (std::abs(light) <= epsilon || std::abs(1.0 - light) < epsilon) ? 0.0 : (mx - light) / std::min(light, 1.0 - light);
                if (mx == rgb.x) {
                    hue = (rgb.y - rgb.z) / d + (rgb.y < rgb.z ? 6.0 : 0.0);
                } else if (mx == rgb.y) {
                    hue = (rgb.z - rgb.x) / d + 2.0;
                } else {
                    hue = (rgb.x - rgb.y) / d + 4.0;
                }
                hue *= 60.0;
            }
            if (sat < 0.0) {
                hue += 180.0;
                sat = std::abs(sat);
            }
            if (hue >= 360.0) hue -= 360.0;
            if (sat <= epsilon) {
                hue = std::numeric_limits<double>::quiet_NaN();
                sat = 0.0;
            }
            return dvec3{hue, sat * 100.0, light * 100.0};
        }

        [[nodiscard]] inline dvec3 hwb_to_srgb(dvec3 hwb) noexcept {
            const double w = hwb.y / 100.0;
            const double b = hwb.z / 100.0;
            if (w + b >= 1.0) {
                const double grey = w / (w + b);
                return dvec3{grey};
            }
            dvec3 rgb = hsl_to_srgb(dvec3{hwb.x, 100.0, 50.0});
            return rgb * (1.0 - w - b) + dvec3{w};
        }

        [[nodiscard]] inline dvec3 srgb_to_hwb(dvec3 rgb) noexcept {
            const dvec3 hsl = srgb_to_hsl(rgb);
            const double white = std::min({rgb.x, rgb.y, rgb.z});
            const double black = 1.0 - std::max({rgb.x, rgb.y, rgb.z});
            return dvec3{hsl.x, white * 100.0, black * 100.0};
        }

        [[nodiscard]] inline dvec3 polar_to_rect(dvec3 lch) noexcept {
            const double hue = std::isnan(lch.z) ? 0.0 : lch.z;
            return dvec3{lch.x, lch.y * std::cos(hue * kRadiansPerDegree), lch.y * std::sin(hue * kRadiansPerDegree)};
        }

        /// Rectangular (L a b) to polar (L C H) with the spec's powerless-hue threshold: hue is NaN at or below `epsilon` chroma.
        [[nodiscard]] inline dvec3 rect_to_polar(dvec3 lab, double epsilon) noexcept {
            const double chroma = std::sqrt(lab.y * lab.y + lab.z * lab.z);
            double hue = std::atan2(lab.z, lab.y) / kRadiansPerDegree;
            if (hue < 0.0) hue += 360.0;
            if (chroma <= epsilon) hue = std::numeric_limits<double>::quiet_NaN();
            return dvec3{lab.x, chroma, hue};
        }

        [[nodiscard]] inline dvec3 xyz_d50_to_lab(dvec3 xyz) noexcept {
            const dvec3 scaled{xyz.x / kD50[0], xyz.y / kD50[1], xyz.z / kD50[2]};
            const auto f = [](double v) { return v > kLabEpsilon ? std::cbrt(v) : (kLabKappa * v + 16.0) / 116.0; };
            const dvec3 fv{f(scaled.x), f(scaled.y), f(scaled.z)};
            return dvec3{116.0 * fv.y - 16.0, 500.0 * (fv.x - fv.y), 200.0 * (fv.y - fv.z)};
        }

        [[nodiscard]] inline dvec3 lab_to_xyz_d50(dvec3 lab) noexcept {
            const double l116 = lab.x / 116.0;
            const double a500 = lab.y / 500.0;
            const double b200 = lab.z / 200.0;
            const double fy = l116 + 16.0 / 116.0;
            const double fx = fy + a500;
            const double fz = fy - b200;
            const double x = std::pow(fx, 3.0) > kLabEpsilon ? std::pow(fx, 3.0) : (l116 + a500) * (116.0 / kLabKappa);
            const double y = lab.x > kLabKappa * kLabEpsilon ? std::pow((lab.x + 16.0) / 116.0, 3.0) : lab.x / kLabKappa;
            const double z = std::pow(fz, 3.0) > kLabEpsilon ? std::pow(fz, 3.0) : (l116 - b200) * (116.0 / kLabKappa);
            return dvec3{x * kD50[0], y * kD50[1], z * kD50[2]};
        }

        [[nodiscard]] inline dvec3 xyz_d65_to_oklab(dvec3 xyz) noexcept {
            const dvec3 lms = kXyzToLms * xyz;
            return kLmsToOklab * dvec3{std::cbrt(lms.x), std::cbrt(lms.y), std::cbrt(lms.z)};
        }

        [[nodiscard]] inline dvec3 oklab_to_xyz_d65(dvec3 lab) noexcept {
            const dvec3 lms = kOklabToLms * lab;
            return kLmsToXyz * dvec3{lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z};
        }

        /// Components of a colour in `space` to CIE XYZ with the D65 white every converter here pivots on.
        [[nodiscard]] inline dvec3 to_xyz_d65(dvec3 c, ColorSpace space) noexcept {
            switch (space) {
                case ColorSpace::Srgb: return kLinSrgbToXyz * map3(c, decode_srgb);
                case ColorSpace::SrgbLinear: return kLinSrgbToXyz * c;
                case ColorSpace::DisplayP3: return kLinP3ToXyz * map3(c, decode_srgb);
                case ColorSpace::DisplayP3Linear: return kLinP3ToXyz * c;
                case ColorSpace::A98Rgb: return kLinA98ToXyz * map3(c, [](double v) { return decode_power(v, 563.0 / 256.0); });
                case ColorSpace::ProphotoRgb: return kD50ToD65 * (kLinProphotoToXyz * map3(c, decode_prophoto));
                case ColorSpace::Rec2020: return kLin2020ToXyz * map3(c, [](double v) { return decode_power(v, 2.4); });
                case ColorSpace::XyzD50: return kD50ToD65 * c;
                case ColorSpace::XyzD65: return c;
                case ColorSpace::Lab: return kD50ToD65 * lab_to_xyz_d50(c);
                case ColorSpace::Oklab: return oklab_to_xyz_d65(c);
                case ColorSpace::Hsl: return kLinSrgbToXyz * map3(hsl_to_srgb(c), decode_srgb);
                case ColorSpace::Hwb: return kLinSrgbToXyz * map3(hwb_to_srgb(c), decode_srgb);
                case ColorSpace::Lch: return kD50ToD65 * lab_to_xyz_d50(polar_to_rect(c));
                case ColorSpace::Oklch: return oklab_to_xyz_d65(polar_to_rect(c));
            }
            return c;
        }

        /// The inverse of `to_xyz_d65`. A powerless hue comes back as NaN (CSS "missing"), which `convert` leaves for the caller.
        [[nodiscard]] inline dvec3 from_xyz_d65(dvec3 xyz, ColorSpace space) noexcept {
            switch (space) {
                case ColorSpace::Srgb: return map3(kXyzToLinSrgb * xyz, encode_srgb);
                case ColorSpace::SrgbLinear: return kXyzToLinSrgb * xyz;
                case ColorSpace::DisplayP3: return map3(kXyzToLinP3 * xyz, encode_srgb);
                case ColorSpace::DisplayP3Linear: return kXyzToLinP3 * xyz;
                case ColorSpace::A98Rgb: return map3(kXyzToLinA98 * xyz, [](double v) { return decode_power(v, 256.0 / 563.0); });
                case ColorSpace::ProphotoRgb: return map3(kXyzToLinProphoto * (kD65ToD50 * xyz), encode_prophoto);
                case ColorSpace::Rec2020: return map3(kXyzToLin2020 * xyz, [](double v) { return decode_power(v, 1.0 / 2.4); });
                case ColorSpace::XyzD50: return kD65ToD50 * xyz;
                case ColorSpace::XyzD65: return xyz;
                case ColorSpace::Lab: return xyz_d50_to_lab(kD65ToD50 * xyz);
                case ColorSpace::Oklab: return xyz_d65_to_oklab(xyz);
                case ColorSpace::Hsl: return srgb_to_hsl(map3(kXyzToLinSrgb * xyz, encode_srgb));
                case ColorSpace::Hwb: return srgb_to_hwb(map3(kXyzToLinSrgb * xyz, encode_srgb));
                case ColorSpace::Lch: return rect_to_polar(xyz_d50_to_lab(kD65ToD50 * xyz), 0.0015);
                case ColorSpace::Oklch: return rect_to_polar(xyz_d65_to_oklab(xyz), 0.000004);
            }
            return xyz;
        }

        /// A polar colour's hue is powerless (CSS "missing" after conversion) when its colourfulness is negligible.
        [[nodiscard]] inline bool hue_is_powerless(dvec3 c, ColorSpace space) noexcept {
            switch (space) {
                case ColorSpace::Hsl: return c.y <= 0.001;
                case ColorSpace::Hwb: return c.y + c.z >= 99.999;
                case ColorSpace::Lch: return c.y <= 0.0015;
                case ColorSpace::Oklch: return c.y <= 0.000004;
                default: return false;
            }
        }

        [[nodiscard]] inline dvec3 convert3(dvec3 c, ColorSpace from, ColorSpace to) noexcept {
            if (from == to) {
                return c;
            }
            return from_xyz_d65(to_xyz_d65(c, from), to);
        }

    } // namespace Detail

    /// `color` (three components in `from`) expressed in `to`. A grey in a polar `to` has a NaN hue: CSS "missing", meaning "no hue".
    [[nodiscard]] inline glm::vec3 convert(glm::vec3 color, ColorSpace from, ColorSpace to) noexcept {
        const Detail::dvec3 result = Detail::convert3(Detail::dvec3{color}, from, to);
        return glm::vec3{static_cast<float>(result.x), static_cast<float>(result.y), static_cast<float>(result.z)};
    }

    /// As above, alpha carried through unchanged.
    [[nodiscard]] inline glm::vec4 convert(glm::vec4 color, ColorSpace from, ColorSpace to) noexcept {
        return glm::vec4{convert(glm::vec3{color}, from, to), color.w};
    }

    /// CSS interpolation of two colours given in `source` space: both are converted to `method.space`, hues are fixed up for a polar space
    /// (a powerless hue takes the other colour's), alpha-premultiplied components are mixed linearly with `t` the weight of `b`, the result
    /// is un-premultiplied and converted back to `result_space`. `source` and `result_space` default to sRGB, what a UI colour is.
    [[nodiscard]] inline glm::vec4 mix(glm::vec4 a, glm::vec4 b, float t, ColorInterpolation method = {}, ColorSpace source = ColorSpace::Srgb,
                                       ColorSpace result_space = ColorSpace::Srgb) noexcept {
        using Detail::dvec3;
        const ColorSpace space = method.space;
        dvec3 ca = Detail::convert3(dvec3{a}, source, space);
        dvec3 cb = Detail::convert3(dvec3{b}, source, space);
        const double alpha_a = a.w;
        const double alpha_b = b.w;
        const double weight = t;

        int hue_index = -1;
        if (is_polar(space)) {
            hue_index = hue_component(space);
            const bool missing_a = Detail::hue_is_powerless(ca, space) || std::isnan(ca[hue_index]);
            const bool missing_b = Detail::hue_is_powerless(cb, space) || std::isnan(cb[hue_index]);
            if (missing_a && !missing_b) {
                ca[hue_index] = cb[hue_index];
            } else if (missing_b && !missing_a) {
                cb[hue_index] = ca[hue_index];
            } else if (missing_a && missing_b) {
                ca[hue_index] = 0.0;
                cb[hue_index] = 0.0;
            }
            // Hue fix-up (CSS Color 4 section 13.6).
            double h1 = std::fmod(ca[hue_index], 360.0);
            double h2 = std::fmod(cb[hue_index], 360.0);
            if (h1 < 0.0) h1 += 360.0;
            if (h2 < 0.0) h2 += 360.0;
            switch (method.hue) {
                case HueInterpolation::Shorter:
                    if (h2 - h1 > 180.0) {
                        h1 += 360.0;
                    } else if (h2 - h1 < -180.0) {
                        h2 += 360.0;
                    }
                    break;
                case HueInterpolation::Longer:
                    if (h2 - h1 > 0.0 && h2 - h1 < 180.0) {
                        h1 += 360.0;
                    } else if (h2 - h1 > -180.0 && h2 - h1 <= 0.0) {
                        h2 += 360.0;
                    }
                    break;
                case HueInterpolation::Increasing:
                    if (h2 < h1) h2 += 360.0;
                    break;
                case HueInterpolation::Decreasing:
                    if (h1 < h2) h1 += 360.0;
                    break;
            }
            ca[hue_index] = h1;
            cb[hue_index] = h2;
        }

        // Premultiply: every component by alpha, except the hue of a polar space.
        for (int i = 0; i < 3; ++i) {
            if (i != hue_index) {
                ca[i] *= alpha_a;
                cb[i] *= alpha_b;
            }
        }
        dvec3 mixed = ca * (1.0 - weight) + cb * weight;
        const double alpha = alpha_a * (1.0 - weight) + alpha_b * weight;
        if (alpha != 0.0) {
            for (int i = 0; i < 3; ++i) {
                if (i != hue_index) {
                    mixed[i] /= alpha;
                }
            }
        }
        if (hue_index >= 0) {
            mixed[hue_index] = std::fmod(mixed[hue_index], 360.0);
            if (mixed[hue_index] < 0.0) mixed[hue_index] += 360.0;
        }
        const dvec3 out = Detail::convert3(mixed, space, result_space);
        return glm::vec4{static_cast<float>(out.x), static_cast<float>(out.y), static_cast<float>(out.z), static_cast<float>(alpha)};
    }

    /// Opaque-colour form of `mix`: three components in, three out, alpha 1 throughout.
    [[nodiscard]] inline glm::vec3 mix(glm::vec3 a, glm::vec3 b, float t, ColorInterpolation method = {}, ColorSpace source = ColorSpace::Srgb,
                                       ColorSpace result_space = ColorSpace::Srgb) noexcept {
        return glm::vec3{mix(glm::vec4{a, 1.0f}, glm::vec4{b, 1.0f}, t, method, source, result_space)};
    }

    /// `mix` with just the space (and the default shorter hue).
    [[nodiscard]] inline glm::vec4 mix(glm::vec4 a, glm::vec4 b, float t, ColorSpace interpolation_space, ColorSpace source = ColorSpace::Srgb,
                                       ColorSpace result_space = ColorSpace::Srgb) noexcept {
        return mix(a, b, t, ColorInterpolation{.space = interpolation_space}, source, result_space);
    }

    [[nodiscard]] inline glm::vec3 mix(glm::vec3 a, glm::vec3 b, float t, ColorSpace interpolation_space, ColorSpace source = ColorSpace::Srgb,
                                       ColorSpace result_space = ColorSpace::Srgb) noexcept {
        return mix(a, b, t, ColorInterpolation{.space = interpolation_space}, source, result_space);
    }

} // namespace SFT::UI
