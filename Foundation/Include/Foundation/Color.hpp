#pragma once

#include <Foundation/Types.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cmath>
#include <type_traits>
#include <utility>

namespace SFT::Foundation::Color {

    inline constexpr f64 epsilon = 1.0e-10;
    inline constexpr f64 pi = 3.141592653589793238462643383279502884;
    inline constexpr f64 radians_per_degree = pi / 180.0;
    inline constexpr f64 degrees_per_radian = 180.0 / pi;

    struct Srgb;
    struct Xyz;
    struct AdobeRgb;
    struct DisplayP3;
    struct Rec2020;
    struct Hsl;
    struct Hsv;
    struct Hwb;
    struct Lab;
    struct Lch;
    struct Luv;
    struct Oklab;
    struct Oklch;

    [[nodiscard]] inline f64 clamp01(f64 value) noexcept { return std::clamp(value, 0.0, 1.0); }
    [[nodiscard]] inline f64 wrap_degrees(f64 hue) noexcept {
        const f64 wrapped = std::fmod(hue, 360.0);
        return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
    }
    [[nodiscard]] inline f64 srgb_to_linear_channel(f64 value) noexcept {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }
    [[nodiscard]] inline f64 linear_to_srgb_channel(f64 value) noexcept {
        return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }

    struct Linear {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;

        [[nodiscard]] static constexpr Linear opaque(f64 red, f64 green, f64 blue) noexcept { return {red, green, blue, 1.0}; }
        [[nodiscard]] constexpr Linear to_linear() const noexcept { return *this; }
        [[nodiscard]] static constexpr Linear from_linear(const Linear &color) noexcept { return color; }
        [[nodiscard]] auto operator<=>(const Linear &) const = default;

        [[nodiscard]] Srgb to_srgb() const noexcept;
        [[nodiscard]] Xyz to_xyz() const noexcept;
        [[nodiscard]] AdobeRgb to_adobe_rgb() const noexcept;
        [[nodiscard]] DisplayP3 to_display_p3() const noexcept;
        [[nodiscard]] Rec2020 to_rec2020() const noexcept;
        [[nodiscard]] Hsl to_hsl() const noexcept;
        [[nodiscard]] Hsv to_hsv() const noexcept;
        [[nodiscard]] Hwb to_hwb() const noexcept;
        [[nodiscard]] Lab to_lab() const noexcept;
        [[nodiscard]] Lch to_lch() const noexcept;
        [[nodiscard]] Luv to_luv() const noexcept;
        [[nodiscard]] Oklab to_oklab() const noexcept;
        [[nodiscard]] Oklch to_oklch() const noexcept;
    };

    struct Srgb {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Srgb &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept { return {srgb_to_linear_channel(r), srgb_to_linear_channel(g), srgb_to_linear_channel(b), a}; }
        [[nodiscard]] static Srgb from_linear(const Linear &c) noexcept { return {linear_to_srgb_channel(c.r), linear_to_srgb_channel(c.g), linear_to_srgb_channel(c.b), c.a}; }
    };

    struct Xyz {
        f64 x = 0.0;
        f64 y = 0.0;
        f64 z = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Xyz &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            return {
                3.2406 * x - 1.5372 * y - 0.4986 * z,
                -0.9689 * x + 1.8758 * y + 0.0415 * z,
                0.0557 * x - 0.2040 * y + 1.0570 * z,
                alpha,
            };
        }
        [[nodiscard]] static Xyz from_linear(const Linear &c) noexcept {
            return {
                0.4124 * c.r + 0.3576 * c.g + 0.1805 * c.b,
                0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b,
                0.0193 * c.r + 0.1192 * c.g + 0.9505 * c.b,
                c.a,
            };
        }
    };

    struct AdobeRgb {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const AdobeRgb &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            constexpr f64 inv_gamma = 563.0 / 256.0;
            const f64 rl = std::abs(r) < epsilon ? 0.0 : std::pow(r, inv_gamma);
            const f64 gl = std::abs(g) < epsilon ? 0.0 : std::pow(g, inv_gamma);
            const f64 bl = std::abs(b) < epsilon ? 0.0 : std::pow(b, inv_gamma);
            const f64 x = 0.57667 * rl + 0.18556 * gl + 0.18823 * bl;
            const f64 y = 0.29734 * rl + 0.62736 * gl + 0.07529 * bl;
            const f64 z = 0.02703 * rl + 0.07069 * gl + 0.99134 * bl;
            return Xyz{x, y, z, a}.to_linear();
        }
        [[nodiscard]] static AdobeRgb from_linear(const Linear &c) noexcept {
            const Xyz xyz = Xyz::from_linear(c);
            const f64 rl = 2.04159 * xyz.x - 0.56501 * xyz.y - 0.34473 * xyz.z;
            const f64 gl = -0.96924 * xyz.x + 1.87597 * xyz.y + 0.04156 * xyz.z;
            const f64 bl = 0.01344 * xyz.x - 0.11836 * xyz.y + 1.01517 * xyz.z;
            constexpr f64 gamma = 256.0 / 563.0;
            return {
                std::abs(rl) < epsilon ? 0.0 : std::pow(rl, gamma),
                std::abs(gl) < epsilon ? 0.0 : std::pow(gl, gamma),
                std::abs(bl) < epsilon ? 0.0 : std::pow(bl, gamma),
                c.a,
            };
        }
    };

    struct DisplayP3 {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const DisplayP3 &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 rl = srgb_to_linear_channel(r);
            const f64 gl = srgb_to_linear_channel(g);
            const f64 bl = srgb_to_linear_channel(b);
            const f64 x = 0.486569 * rl + 0.265673 * gl + 0.198187 * bl;
            const f64 y = 0.228973 * rl + 0.691752 * gl + 0.0792749 * bl;
            const f64 z = 0.0451143 * gl + 1.04379 * bl;
            return Xyz{x, y, z, a}.to_linear();
        }
        [[nodiscard]] static DisplayP3 from_linear(const Linear &c) noexcept {
            const Xyz xyz = Xyz::from_linear(c);
            const f64 rl = 1.2249 * xyz.x - 0.2247 * xyz.y - 0.0040 * xyz.z;
            const f64 gl = -0.0420 * xyz.x + 1.0419 * xyz.y + 0.0001 * xyz.z;
            const f64 bl = -0.0776 * xyz.y + 0.9398 * xyz.z;
            return {linear_to_srgb_channel(rl), linear_to_srgb_channel(gl), linear_to_srgb_channel(bl), c.a};
        }
    };

    struct Rec2020 {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Rec2020 &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 rl = std::pow(clamp01(r), 2.4);
            const f64 gl = std::pow(clamp01(g), 2.4);
            const f64 bl = std::pow(clamp01(b), 2.4);
            const f64 x = 0.636958 * rl + 0.144617 * gl + 0.168881 * bl;
            const f64 y = 0.2627 * rl + 0.678 * gl + 0.0593 * bl;
            const f64 z = 0.028073 * gl + 1.060985 * bl;
            return {
                3.240969 * x - 1.537383 * y - 0.498611 * z,
                -0.969244 * x + 1.875968 * y + 0.041555 * z,
                0.05563 * x - 0.203977 * y + 1.056972 * z,
                a,
            };
        }
        [[nodiscard]] static Rec2020 from_linear(const Linear &c) noexcept {
            const Xyz xyz = Xyz::from_linear(c);
            const f64 rl = 1.7166634 * xyz.x - 0.3556733 * xyz.y - 0.2533681 * xyz.z;
            const f64 gl = -0.6666738 * xyz.x + 1.6164557 * xyz.y + 0.0157683 * xyz.z;
            const f64 bl = 0.0176425 * xyz.x - 0.0427769 * xyz.y + 0.9422433 * xyz.z;
            return {std::pow(clamp01(rl), 1.0 / 2.4), std::pow(clamp01(gl), 1.0 / 2.4), std::pow(clamp01(bl), 1.0 / 2.4), c.a};
        }
    };

    struct Hsl {
        f64 h = 0.0;
        f64 s = 0.0;
        f64 l = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hsl &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            auto hue_to_rgb = [](f64 p, f64 q, f64 t) noexcept {
                if (t < 0.0) t += 1.0;
                if (t > 1.0) t -= 1.0;
                if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
                if (t < 0.5) return q;
                if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
                return p;
            };
            const f64 hn = h / 360.0;
            const f64 q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
            const f64 p = 2.0 * l - q;
            return {hue_to_rgb(p, q, hn + 1.0 / 3.0), hue_to_rgb(p, q, hn), hue_to_rgb(p, q, hn - 1.0 / 3.0), a};
        }
        [[nodiscard]] static Hsl from_linear(const Linear &c) noexcept {
            const f64 maxc = std::max({c.r, c.g, c.b});
            const f64 minc = std::min({c.r, c.g, c.b});
            const f64 l = (maxc + minc) * 0.5;
            const f64 d = maxc - minc;
            if (maxc == minc || std::abs(d) < epsilon) return {0.0, 0.0, l, c.a};
            const f64 s = l > 0.5 ? d / (2.0 - maxc - minc) : d / (maxc + minc);
            f64 h = 0.0;
            if (maxc == c.r) h = ((c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0)) / 6.0;
            else if (maxc == c.g) h = ((c.b - c.r) / d + 2.0) / 6.0;
            else h = ((c.r - c.g) / d + 4.0) / 6.0;
            return {h * 360.0, s, l, c.a};
        }
    };

    struct Hsv {
        f64 h = 0.0;
        f64 s = 0.0;
        f64 v = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hsv &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 hs = h / 60.0;
            const i32 i = static_cast<i32>(std::floor(hs));
            const f64 f = hs - static_cast<f64>(i);
            const f64 p = v * (1.0 - s);
            const f64 q = v * (1.0 - s * f);
            const f64 t = v * (1.0 - s * (1.0 - f));
            switch ((i % 6 + 6) % 6) {
                case 0: return {v, t, p, a};
                case 1: return {q, v, p, a};
                case 2: return {p, v, t, a};
                case 3: return {p, q, v, a};
                case 4: return {t, p, v, a};
                default: return {v, p, q, a};
            }
        }
        [[nodiscard]] static Hsv from_linear(const Linear &c) noexcept {
            const f64 maxc = std::max({c.r, c.g, c.b});
            const f64 minc = std::min({c.r, c.g, c.b});
            const f64 d = maxc - minc;
            const f64 s = maxc != 0.0 ? d / maxc : 0.0;
            f64 h = 0.0;
            if (d != 0.0) {
                if (maxc == c.r) h = std::fmod((c.g - c.b) / d, 6.0);
                else if (maxc == c.g) h = ((c.b - c.r) / d) + 2.0;
                else h = ((c.r - c.g) / d) + 4.0;
                h *= 60.0;
            }
            return {h < 0.0 ? h + 360.0 : h, s, maxc, c.a};
        }
    };

    struct Hwb {
        f64 h = 0.0;
        f64 w = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hwb &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 white = clamp01(w);
            const f64 black = clamp01(b);
            const f64 sum = white + black;
            if (sum >= 1.0) {
                const f64 gray = white / (std::abs(sum) < epsilon ? epsilon : sum);
                return {gray, gray, gray, a};
            }
            const f64 v = 1.0 - black;
            const f64 s = v > epsilon ? 1.0 - white / v : 0.0;
            return Hsv{h, s, v, a}.to_linear();
        }
        [[nodiscard]] static Hwb from_linear(const Linear &c) noexcept {
            const Hsv hsv = Hsv::from_linear(c);
            return {hsv.h, std::min({c.r, c.g, c.b}), 1.0 - std::max({c.r, c.g, c.b}), hsv.a};
        }
    };

    struct Lab {
        f64 l = 0.0;
        f64 a = 0.0;
        f64 b = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Lab &) const = default;
        [[nodiscard]] static f64 f(f64 t) noexcept {
            constexpr f64 eps = (6.0 / 29.0) * (6.0 / 29.0) * (6.0 / 29.0);
            constexpr f64 k = (1.0 / 3.0) * (29.0 / 6.0) * (29.0 / 6.0);
            constexpr f64 c = 4.0 / 29.0;
            return t > eps ? (std::abs(t) < epsilon ? 0.0 : std::pow(t, 1.0 / 3.0)) : k * t + c;
        }
        [[nodiscard]] static f64 f_inv(f64 u) noexcept {
            constexpr f64 eps = 6.0 / 29.0;
            constexpr f64 k = 3.0 * (6.0 / 29.0) * (6.0 / 29.0);
            constexpr f64 c = 4.0 / 29.0;
            return u > eps ? (std::abs(u) < epsilon ? 0.0 : u * u * u) : k * (u - c);
        }
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 fy = (l + 16.0) / 116.0;
            const f64 fx = fy + a / 500.0;
            const f64 fz = fy - b / 200.0;
            return Xyz{0.95047 * f_inv(fx), f_inv(fy), 1.08883 * f_inv(fz), alpha}.to_linear();
        }
        [[nodiscard]] static Lab from_linear(const Linear &c) noexcept {
            const Xyz xyz = Xyz::from_linear(c);
            const f64 fx = f(xyz.x / 0.95047);
            const f64 fy = f(xyz.y);
            const f64 fz = f(xyz.z / 1.08883);
            return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz), c.a};
        }
    };

    struct Lch {
        f64 l = 0.0;
        f64 c = 0.0;
        f64 h = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Lch &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 chroma = std::abs(c) < epsilon ? 0.0 : c;
            const f64 rad = h * radians_per_degree;
            return Lab{l, chroma * std::cos(rad), chroma * std::sin(rad), a}.to_linear();
        }
        [[nodiscard]] static Lch from_linear(const Linear &color) noexcept {
            const Lab lab = Lab::from_linear(color);
            const f64 chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
            const f64 hue = std::abs(chroma) < epsilon ? 0.0 : wrap_degrees(std::atan2(lab.b, lab.a) * degrees_per_radian);
            return {lab.l, chroma, hue, lab.alpha};
        }
    };

    struct Luv {
        f64 l = 0.0;
        f64 u = 0.0;
        f64 v = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Luv &) const = default;
        [[nodiscard]] static f64 u_prime(f64 x, f64 y, f64 z) noexcept {
            const f64 denom = x + 15.0 * y + 3.0 * z;
            return std::abs(denom) < epsilon ? 0.0 : 4.0 * x / denom;
        }
        [[nodiscard]] static f64 v_prime(f64 x, f64 y, f64 z) noexcept {
            const f64 denom = x + 15.0 * y + 3.0 * z;
            return std::abs(denom) < epsilon ? 0.0 : 9.0 * y / denom;
        }
        [[nodiscard]] Linear to_linear() const noexcept {
            constexpr f64 xn = 0.95047;
            constexpr f64 yn = 1.0;
            constexpr f64 zn = 1.08883;
            const f64 up_ref = u_prime(xn, yn, zn);
            const f64 vp_ref = v_prime(xn, yn, zn);
            const f64 yr = l > 8.0 ? std::pow((l + 16.0) / 116.0, 3.0) : l / 903.3;
            const f64 up = std::abs(l) < epsilon ? up_ref : u / (13.0 * l) + up_ref;
            const f64 vp = std::abs(l) < epsilon ? vp_ref : v / (13.0 * l) + vp_ref;
            const f64 denom = std::max(std::abs(4.0 * vp), epsilon);
            const f64 x = yr * 9.0 * up / denom;
            const f64 z = yr * (12.0 - 3.0 * up - 20.0 * vp) / denom;
            return Xyz{x, yr, z, alpha}.to_linear();
        }
        [[nodiscard]] static Luv from_linear(const Linear &c) noexcept {
            const Xyz xyz = Xyz::from_linear(c);
            const f64 yr = xyz.y;
            const f64 l = yr > 0.008856 ? 116.0 * std::pow(yr, 1.0 / 3.0) - 16.0 : 903.3 * yr;
            const f64 ur_n = u_prime(0.95047, 1.0, 1.08883);
            const f64 vr_n = v_prime(0.95047, 1.0, 1.08883);
            return {l, 13.0 * l * (u_prime(xyz.x, xyz.y, xyz.z) - ur_n), 13.0 * l * (v_prime(xyz.x, xyz.y, xyz.z) - vr_n), c.a};
        }
    };

    struct Oklab {
        f64 l = 0.0;
        f64 a = 0.0;
        f64 b = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Oklab &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 ll = l + 0.3963377774 * a + 0.2158037573 * b;
            const f64 mm = l - 0.1055613458 * a - 0.0638541728 * b;
            const f64 ss = l - 0.0894841775 * a - 1.2914855480 * b;
            const f64 l3 = std::abs(ll) < epsilon ? 0.0 : ll * ll * ll;
            const f64 m3 = std::abs(mm) < epsilon ? 0.0 : mm * mm * mm;
            const f64 s3 = std::abs(ss) < epsilon ? 0.0 : ss * ss * ss;
            return {
                clamp01(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3),
                clamp01(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3),
                clamp01(0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3),
                clamp01(alpha),
            };
        }
        [[nodiscard]] static Oklab from_linear(const Linear &c) noexcept {
            const f64 r = clamp01(c.r);
            const f64 g = clamp01(c.g);
            const f64 b = clamp01(c.b);
            const f64 lms_l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
            const f64 lms_m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
            const f64 lms_s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
            const f64 ll = std::abs(lms_l) < epsilon ? 0.0 : std::pow(lms_l, 1.0 / 3.0);
            const f64 mm = std::abs(lms_m) < epsilon ? 0.0 : std::pow(lms_m, 1.0 / 3.0);
            const f64 ss = std::abs(lms_s) < epsilon ? 0.0 : std::pow(lms_s, 1.0 / 3.0);
            return {
                clamp01(0.2104542553 * ll + 0.7936177850 * mm - 0.0040720468 * ss),
                std::clamp(1.9779984951 * ll - 2.4285922050 * mm + 0.4505937099 * ss, -0.5, 0.5),
                std::clamp(0.0259040371 * ll + 0.7827717662 * mm - 0.8086757660 * ss, -0.5, 0.5),
                clamp01(c.a),
            };
        }
    };

    struct Oklch {
        f64 l = 0.0;
        f64 c = 0.0;
        f64 h = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Oklch &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept {
            const f64 chroma = std::abs(c) < epsilon ? 0.0 : c;
            const f64 rad = h * radians_per_degree;
            return Oklab{l, chroma * std::cos(rad), chroma * std::sin(rad), alpha}.to_linear();
        }
        [[nodiscard]] static Oklch from_linear(const Linear &color) noexcept {
            const Oklab lab = Oklab::from_linear(color);
            const f64 chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
            const f64 hue = std::abs(chroma) < epsilon ? 0.0 : wrap_degrees(std::atan2(lab.b, lab.a) * degrees_per_radian);
            return {lab.l, chroma, hue, lab.alpha};
        }
    };

    inline Srgb Linear::to_srgb() const noexcept { return Srgb::from_linear(*this); }
    inline Xyz Linear::to_xyz() const noexcept { return Xyz::from_linear(*this); }
    inline AdobeRgb Linear::to_adobe_rgb() const noexcept { return AdobeRgb::from_linear(*this); }
    inline DisplayP3 Linear::to_display_p3() const noexcept { return DisplayP3::from_linear(*this); }
    inline Rec2020 Linear::to_rec2020() const noexcept { return Rec2020::from_linear(*this); }
    inline Hsl Linear::to_hsl() const noexcept { return Hsl::from_linear(*this); }
    inline Hsv Linear::to_hsv() const noexcept { return Hsv::from_linear(*this); }
    inline Hwb Linear::to_hwb() const noexcept { return Hwb::from_linear(*this); }
    inline Lab Linear::to_lab() const noexcept { return Lab::from_linear(*this); }
    inline Lch Linear::to_lch() const noexcept { return Lch::from_linear(*this); }
    inline Luv Linear::to_luv() const noexcept { return Luv::from_linear(*this); }
    inline Oklab Linear::to_oklab() const noexcept { return Oklab::from_linear(*this); }
    inline Oklch Linear::to_oklch() const noexcept { return Oklch::from_linear(*this); }

    namespace Detail {

        template <typename T>
        struct ColorTraits {
            static constexpr bool is_color = false;
        };

#define SFT_FOUNDATION_COLOR_TRAITS(Type, C0, C1, C2, C3)                                  \
        template <>                                                                         \
        struct ColorTraits<Type> {                                                          \
            static constexpr bool is_color = true;                                          \
            [[nodiscard]] static constexpr std::array<f64, 4> components(const Type &c) noexcept { \
                return {c.C0, c.C1, c.C2, c.C3};                                           \
            }                                                                               \
            [[nodiscard]] static constexpr Type from_components(const std::array<f64, 4> &v) noexcept { \
                return {v[0], v[1], v[2], v[3]};                                           \
            }                                                                               \
        }

        SFT_FOUNDATION_COLOR_TRAITS(Linear, r, g, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(Srgb, r, g, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(Xyz, x, y, z, alpha);
        SFT_FOUNDATION_COLOR_TRAITS(AdobeRgb, r, g, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(DisplayP3, r, g, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(Rec2020, r, g, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(Hsl, h, s, l, a);
        SFT_FOUNDATION_COLOR_TRAITS(Hsv, h, s, v, a);
        SFT_FOUNDATION_COLOR_TRAITS(Hwb, h, w, b, a);
        SFT_FOUNDATION_COLOR_TRAITS(Lab, l, a, b, alpha);
        SFT_FOUNDATION_COLOR_TRAITS(Lch, l, c, h, a);
        SFT_FOUNDATION_COLOR_TRAITS(Luv, l, u, v, alpha);
        SFT_FOUNDATION_COLOR_TRAITS(Oklab, l, a, b, alpha);
        SFT_FOUNDATION_COLOR_TRAITS(Oklch, l, c, h, alpha);

#undef SFT_FOUNDATION_COLOR_TRAITS

        template <typename T>
        inline constexpr bool is_color_space_v = ColorTraits<std::remove_cvref_t<T>>::is_color;

        template <typename T>
        concept ColorScalar = (std::integral<std::remove_cvref_t<T>> || std::floating_point<std::remove_cvref_t<T>>) &&
            !std::same_as<std::remove_cvref_t<T>, bool>;

    } // namespace Detail

    template <typename T>
    concept ColorSpace = Detail::is_color_space_v<T> && requires(const std::remove_cvref_t<T> &c, const Linear &linear) {
        { c.to_linear() } -> std::same_as<Linear>;
        { std::remove_cvref_t<T>::from_linear(linear) } -> std::same_as<std::remove_cvref_t<T>>;
    };

    template <typename T>
    concept ColorScalar = Detail::ColorScalar<T>;

    template <ColorSpace Target, ColorSpace Source>
    [[nodiscard]] inline std::remove_cvref_t<Target> convert_to(const Source &source) noexcept {
        using TargetColor = std::remove_cvref_t<Target>;
        using SourceColor = std::remove_cvref_t<Source>;
        if constexpr (std::same_as<TargetColor, SourceColor>) {
            return source;
        } else {
            return TargetColor::from_linear(source.to_linear());
        }
    }

    template <ColorSpace Left, ColorSpace Right, typename Op>
    [[nodiscard]] inline std::remove_cvref_t<Left> operate_in_left_space(const Left &left, const Right &right, Op &&op) noexcept {
        using LeftColor = std::remove_cvref_t<Left>;
        const LeftColor converted = convert_to<LeftColor>(right);
        const auto l = Detail::ColorTraits<LeftColor>::components(left);
        const auto r = Detail::ColorTraits<LeftColor>::components(converted);
        return Detail::ColorTraits<LeftColor>::from_components({
            op(l[0], r[0]),
            op(l[1], r[1]),
            op(l[2], r[2]),
            op(l[3], r[3]),
        });
    }

    template <ColorSpace C, typename Op>
    [[nodiscard]] inline std::remove_cvref_t<C> operate_rgb(const C &color, Op &&op) noexcept {
        using Color = std::remove_cvref_t<C>;
        auto components = Detail::ColorTraits<Color>::components(color);
        components[0] = op(components[0]);
        components[1] = op(components[1]);
        components[2] = op(components[2]);
        return Detail::ColorTraits<Color>::from_components(components);
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> operator+(const Left &left, const Right &right) noexcept {
        return operate_in_left_space(left, right, [](f64 a, f64 b) noexcept { return a + b; });
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> operator-(const Left &left, const Right &right) noexcept {
        return operate_in_left_space(left, right, [](f64 a, f64 b) noexcept { return a - b; });
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> operator*(const Left &left, const Right &right) noexcept {
        return operate_in_left_space(left, right, [](f64 a, f64 b) noexcept { return a * b; });
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> operator/(const Left &left, const Right &right) noexcept {
        return operate_in_left_space(left, right, [](f64 a, f64 b) noexcept { return a / b; });
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> operator-(const C &color) noexcept {
        return operate_rgb(color, [](f64 v) noexcept { return -v; });
    }

    template <ColorSpace C, ColorScalar S>
    [[nodiscard]] inline std::remove_cvref_t<C> operator*(const C &color, S scalar) noexcept {
        const f64 s = static_cast<f64>(scalar);
        return operate_rgb(color, [s](f64 v) noexcept { return v * s; });
    }

    template <ColorScalar S, ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> operator*(S scalar, const C &color) noexcept {
        return color * scalar;
    }

    template <ColorSpace C, ColorScalar S>
    [[nodiscard]] inline std::remove_cvref_t<C> operator/(const C &color, S scalar) noexcept {
        const f64 s = static_cast<f64>(scalar);
        return operate_rgb(color, [s](f64 v) noexcept { return v / s; });
    }

    template <ColorSpace C, ColorScalar S>
    [[nodiscard]] inline std::remove_cvref_t<C> operator+(const C &color, S scalar) noexcept {
        const f64 s = static_cast<f64>(scalar);
        return operate_rgb(color, [s](f64 v) noexcept { return v + s; });
    }

    template <ColorScalar S, ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> operator+(S scalar, const C &color) noexcept {
        return color + scalar;
    }

    template <ColorSpace C, ColorScalar S>
    [[nodiscard]] inline std::remove_cvref_t<C> operator-(const C &color, S scalar) noexcept {
        const f64 s = static_cast<f64>(scalar);
        return operate_rgb(color, [s](f64 v) noexcept { return v - s; });
    }

    template <ColorScalar S, ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> operator-(S scalar, const C &color) noexcept {
        const f64 s = static_cast<f64>(scalar);
        return operate_rgb(color, [s](f64 v) noexcept { return s - v; });
    }

    template <ColorSpace Left, ColorSpace Right>
    inline std::remove_cvref_t<Left> &operator+=(Left &left, const Right &right) noexcept {
        left = left + right;
        return left;
    }

    template <ColorSpace Left, ColorSpace Right>
    inline std::remove_cvref_t<Left> &operator-=(Left &left, const Right &right) noexcept {
        left = left - right;
        return left;
    }

    template <ColorSpace Left, ColorSpace Right>
    inline std::remove_cvref_t<Left> &operator*=(Left &left, const Right &right) noexcept {
        left = left * right;
        return left;
    }

    template <ColorSpace Left, ColorSpace Right>
    inline std::remove_cvref_t<Left> &operator/=(Left &left, const Right &right) noexcept {
        left = left / right;
        return left;
    }

    template <ColorSpace C, ColorScalar S>
    inline std::remove_cvref_t<C> &operator+=(C &color, S scalar) noexcept {
        color = color + scalar;
        return color;
    }

    template <ColorSpace C, ColorScalar S>
    inline std::remove_cvref_t<C> &operator-=(C &color, S scalar) noexcept {
        color = color - scalar;
        return color;
    }

    template <ColorSpace C, ColorScalar S>
    inline std::remove_cvref_t<C> &operator*=(C &color, S scalar) noexcept {
        color = color * scalar;
        return color;
    }

    template <ColorSpace C, ColorScalar S>
    inline std::remove_cvref_t<C> &operator/=(C &color, S scalar) noexcept {
        color = color / scalar;
        return color;
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> lerp(const Left &left, const Right &right, f64 t) noexcept {
        return operate_in_left_space(left, right, [t](f64 a, f64 b) noexcept { return std::lerp(a, b, t); });
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> mix(const Left &left, const Right &right, f64 t) noexcept {
        return lerp(left, right, t);
    }

    template <ColorSpace Left, ColorSpace Right>
    [[nodiscard]] inline std::remove_cvref_t<Left> modulate(const Left &left, const Right &right) noexcept {
        return left * right;
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> with_alpha(const C &color, f64 alpha) noexcept {
        using Color = std::remove_cvref_t<C>;
        auto components = Detail::ColorTraits<Color>::components(color);
        components[3] = alpha;
        return Detail::ColorTraits<Color>::from_components(components);
    }

    template <ColorSpace C>
    [[nodiscard]] inline f64 alpha(const C &color) noexcept {
        return Detail::ColorTraits<std::remove_cvref_t<C>>::components(color)[3];
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> clamp_rgb(const C &color, f64 low = 0.0, f64 high = 1.0) noexcept {
        return operate_rgb(color, [low, high](f64 v) noexcept { return std::clamp(v, low, high); });
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> clamp(const C &color, f64 low = 0.0, f64 high = 1.0) noexcept {
        using Color = std::remove_cvref_t<C>;
        auto components = Detail::ColorTraits<Color>::components(color);
        for (f64 &component : components) {
            component = std::clamp(component, low, high);
        }
        return Detail::ColorTraits<Color>::from_components(components);
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> premultiply_alpha(const C &color) noexcept {
        return color * alpha(color);
    }

    template <ColorSpace C>
    [[nodiscard]] inline std::remove_cvref_t<C> unpremultiply_alpha(const C &color) noexcept {
        const f64 a = alpha(color);
        return std::abs(a) < epsilon ? color : color / a;
    }

    template <ColorSpace Foreground, ColorSpace Background>
    [[nodiscard]] inline std::remove_cvref_t<Foreground> over(const Foreground &foreground, const Background &background) noexcept {
        using Color = std::remove_cvref_t<Foreground>;
        const Color bg = convert_to<Color>(background);
        const auto f = Detail::ColorTraits<Color>::components(foreground);
        const auto b = Detail::ColorTraits<Color>::components(bg);
        const f64 out_alpha = f[3] + b[3] * (1.0 - f[3]);
        if (std::abs(out_alpha) < epsilon) {
            return Detail::ColorTraits<Color>::from_components({0.0, 0.0, 0.0, 0.0});
        }
        return Detail::ColorTraits<Color>::from_components({
            (f[0] * f[3] + b[0] * b[3] * (1.0 - f[3])) / out_alpha,
            (f[1] * f[3] + b[1] * b[3] * (1.0 - f[3])) / out_alpha,
            (f[2] * f[3] + b[2] * b[3] * (1.0 - f[3])) / out_alpha,
            out_alpha,
        });
    }

} // namespace SFT::Foundation::Color
