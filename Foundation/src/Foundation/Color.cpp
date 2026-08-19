#include <Foundation/Color.hpp>


namespace SFT::Foundation::Color {

    /// Performs the clamp01 operation for `Color` using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 clamp01(f64 value) noexcept { return std::clamp(value, 0.0, 1.0); }

    /// Performs the wrap degrees operation for `Color` using the supplied arguments.
    ///
    /// @param hue `hue` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 wrap_degrees(f64 hue) noexcept {
        const f64 wrapped = std::fmod(hue, 360.0);
        return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
    }

    /// Performs the sRGB to linear channel operation for `Color` using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 srgb_to_linear_channel(f64 value) noexcept {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    /// Performs the linear to sRGB channel operation for `Color` using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 linear_to_srgb_channel(f64 value) noexcept {
        return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Srgb::to_linear() const noexcept { return {srgb_to_linear_channel(r), srgb_to_linear_channel(g), srgb_to_linear_channel(b), a}; }

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Srgb Srgb::from_linear(const Linear &c) noexcept { return {linear_to_srgb_channel(c.r), linear_to_srgb_channel(c.g), linear_to_srgb_channel(c.b), c.a}; }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Xyz::to_linear() const noexcept {
        return {
            3.2406 * x - 1.5372 * y - 0.4986 * z,
            -0.9689 * x + 1.8758 * y + 0.0415 * z,
            0.0557 * x - 0.2040 * y + 1.0570 * z,
            alpha,
        };
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Xyz Xyz::from_linear(const Linear &c) noexcept {
        return {
            0.4124 * c.r + 0.3576 * c.g + 0.1805 * c.b,
            0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b,
            0.0193 * c.r + 0.1192 * c.g + 0.9505 * c.b,
            c.a,
        };
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear AdobeRgb::to_linear() const noexcept {
        constexpr f64 inv_gamma = 563.0 / 256.0;
        const f64 rl = std::abs(r) < epsilon ? 0.0 : std::pow(r, inv_gamma);
        const f64 gl = std::abs(g) < epsilon ? 0.0 : std::pow(g, inv_gamma);
        const f64 bl = std::abs(b) < epsilon ? 0.0 : std::pow(b, inv_gamma);
        const f64 x = 0.57667 * rl + 0.18556 * gl + 0.18823 * bl;
        const f64 y = 0.29734 * rl + 0.62736 * gl + 0.07529 * bl;
        const f64 z = 0.02703 * rl + 0.07069 * gl + 0.99134 * bl;
        return Xyz{x, y, z, a}.to_linear();
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    AdobeRgb AdobeRgb::from_linear(const Linear &c) noexcept {
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

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear DisplayP3::to_linear() const noexcept {
        const f64 rl = srgb_to_linear_channel(r);
        const f64 gl = srgb_to_linear_channel(g);
        const f64 bl = srgb_to_linear_channel(b);
        const f64 x = 0.486569 * rl + 0.265673 * gl + 0.198187 * bl;
        const f64 y = 0.228973 * rl + 0.691752 * gl + 0.0792749 * bl;
        const f64 z = 0.0451143 * gl + 1.04379 * bl;
        return Xyz{x, y, z, a}.to_linear();
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    DisplayP3 DisplayP3::from_linear(const Linear &c) noexcept {
        const Xyz xyz = Xyz::from_linear(c);
        const f64 rl = 1.2249 * xyz.x - 0.2247 * xyz.y - 0.0040 * xyz.z;
        const f64 gl = -0.0420 * xyz.x + 1.0419 * xyz.y + 0.0001 * xyz.z;
        const f64 bl = -0.0776 * xyz.y + 0.9398 * xyz.z;
        return {linear_to_srgb_channel(rl), linear_to_srgb_channel(gl), linear_to_srgb_channel(bl), c.a};
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Rec2020::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Rec2020 Rec2020::from_linear(const Linear &c) noexcept {
        const Xyz xyz = Xyz::from_linear(c);
        const f64 rl = 1.7166634 * xyz.x - 0.3556733 * xyz.y - 0.2533681 * xyz.z;
        const f64 gl = -0.6666738 * xyz.x + 1.6164557 * xyz.y + 0.0157683 * xyz.z;
        const f64 bl = 0.0176425 * xyz.x - 0.0427769 * xyz.y + 0.9422433 * xyz.z;
        return {std::pow(clamp01(rl), 1.0 / 2.4), std::pow(clamp01(gl), 1.0 / 2.4), std::pow(clamp01(bl), 1.0 / 2.4), c.a};
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Hsl::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Hsl Hsl::from_linear(const Linear &c) noexcept {
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

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Hsv::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Hsv Hsv::from_linear(const Linear &c) noexcept {
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

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Hwb::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Hwb Hwb::from_linear(const Linear &c) noexcept {
        const Hsv hsv = Hsv::from_linear(c);
        return {hsv.h, std::min({c.r, c.g, c.b}), 1.0 - std::max({c.r, c.g, c.b}), hsv.a};
    }

    /// Performs the f operation for `Color` using the supplied arguments.
    ///
    /// @param t `t` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    f64 Lab::f(f64 t) noexcept {
        constexpr f64 eps = (6.0 / 29.0) * (6.0 / 29.0) * (6.0 / 29.0);
        constexpr f64 k = (1.0 / 3.0) * (29.0 / 6.0) * (29.0 / 6.0);
        constexpr f64 c = 4.0 / 29.0;
        return t > eps ? (std::abs(t) < epsilon ? 0.0 : std::pow(t, 1.0 / 3.0)) : k * t + c;
    }

    /// Performs the f inv operation for `Color` using the supplied arguments.
    ///
    /// @param u `u` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 Lab::f_inv(f64 u) noexcept {
        constexpr f64 eps = 6.0 / 29.0;
        constexpr f64 k = 3.0 * (6.0 / 29.0) * (6.0 / 29.0);
        constexpr f64 c = 4.0 / 29.0;
        return u > eps ? (std::abs(u) < epsilon ? 0.0 : u * u * u) : k * (u - c);
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Lab::to_linear() const noexcept {
        const f64 fy = (l + 16.0) / 116.0;
        const f64 fx = fy + a / 500.0;
        const f64 fz = fy - b / 200.0;
        return Xyz{0.95047 * f_inv(fx), f_inv(fy), 1.08883 * f_inv(fz), alpha}.to_linear();
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Lab Lab::from_linear(const Linear &c) noexcept {
        const Xyz xyz = Xyz::from_linear(c);
        const f64 fx = f(xyz.x / 0.95047);
        const f64 fy = f(xyz.y);
        const f64 fz = f(xyz.z / 1.08883);
        return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz), c.a};
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Lch::to_linear() const noexcept {
        const f64 chroma = std::abs(c) < epsilon ? 0.0 : c;
        const f64 rad = h * radians_per_degree;
        return Lab{l, chroma * std::cos(rad), chroma * std::sin(rad), a}.to_linear();
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Lch Lch::from_linear(const Linear &color) noexcept {
        const Lab lab = Lab::from_linear(color);
        const f64 chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
        const f64 hue = std::abs(chroma) < epsilon ? 0.0 : wrap_degrees(std::atan2(lab.b, lab.a) * degrees_per_radian);
        return {lab.l, chroma, hue, lab.alpha};
    }

    /// Performs the u prime operation for `Color` using the supplied arguments.
    ///
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param z `z` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 Luv::u_prime(f64 x, f64 y, f64 z) noexcept {
        const f64 denom = x + 15.0 * y + 3.0 * z;
        return std::abs(denom) < epsilon ? 0.0 : 4.0 * x / denom;
    }

    /// Performs the v prime operation for `Color` using the supplied arguments.
    ///
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param z `z` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 Luv::v_prime(f64 x, f64 y, f64 z) noexcept {
        const f64 denom = x + 15.0 * y + 3.0 * z;
        return std::abs(denom) < epsilon ? 0.0 : 9.0 * y / denom;
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Luv::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Luv Luv::from_linear(const Linear &c) noexcept {
        const Xyz xyz = Xyz::from_linear(c);
        const f64 yr = xyz.y;
        const f64 l = yr > 0.008856 ? 116.0 * std::pow(yr, 1.0 / 3.0) - 16.0 : 903.3 * yr;
        const f64 ur_n = u_prime(0.95047, 1.0, 1.08883);
        const f64 vr_n = v_prime(0.95047, 1.0, 1.08883);
        return {l, 13.0 * l * (u_prime(xyz.x, xyz.y, xyz.z) - ur_n), 13.0 * l * (v_prime(xyz.x, xyz.y, xyz.z) - vr_n), c.a};
    }

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Oklab::to_linear() const noexcept {
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

    /// Creates or converts a value from linear representation.
    ///
    /// @param c `c` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Oklab Oklab::from_linear(const Linear &c) noexcept {
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

    /// Converts the value to linear representation.
    ///
    /// @return Returns the current to linear value.
    /// @note This function does not throw exceptions.
    Linear Oklch::to_linear() const noexcept {
        const f64 chroma = std::abs(c) < epsilon ? 0.0 : c;
        const f64 rad = h * radians_per_degree;
        return Oklab{l, chroma * std::cos(rad), chroma * std::sin(rad), alpha}.to_linear();
    }

    /// Creates or converts a value from linear representation.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @return Returns the newly constructed or converted value.
    /// @note This function does not throw exceptions.
    Oklch Oklch::from_linear(const Linear &color) noexcept {
        const Oklab lab = Oklab::from_linear(color);
        const f64 chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
        const f64 hue = std::abs(chroma) < epsilon ? 0.0 : wrap_degrees(std::atan2(lab.b, lab.a) * degrees_per_radian);
        return {lab.l, chroma, hue, lab.alpha};
    }

    /// Converts the value to sRGB representation.
    ///
    /// @return Returns the current to sRGB value.
    /// @note This function does not throw exceptions.
    Srgb Linear::to_srgb() const noexcept { return Srgb::from_linear(*this); }

    /// Converts the value to xyz representation.
    ///
    /// @return Returns the current to xyz value.
    /// @note This function does not throw exceptions.
    Xyz Linear::to_xyz() const noexcept { return Xyz::from_linear(*this); }

    /// Converts the value to adobe RGB representation.
    ///
    /// @return Returns the current to adobe RGB value.
    /// @note This function does not throw exceptions.
    AdobeRgb Linear::to_adobe_rgb() const noexcept { return AdobeRgb::from_linear(*this); }

    /// Converts the value to display p3 representation.
    ///
    /// @return Returns the current to display p3 value.
    /// @note This function does not throw exceptions.
    DisplayP3 Linear::to_display_p3() const noexcept { return DisplayP3::from_linear(*this); }

    /// Converts the value to rec2020 representation.
    ///
    /// @return Returns the current to rec2020 value.
    /// @note This function does not throw exceptions.
    Rec2020 Linear::to_rec2020() const noexcept { return Rec2020::from_linear(*this); }

    /// Converts the value to hsl representation.
    ///
    /// @return Returns the current to hsl value.
    /// @note This function does not throw exceptions.
    Hsl Linear::to_hsl() const noexcept { return Hsl::from_linear(*this); }

    /// Converts the value to hsv representation.
    ///
    /// @return Returns the current to hsv value.
    /// @note This function does not throw exceptions.
    Hsv Linear::to_hsv() const noexcept { return Hsv::from_linear(*this); }

    /// Converts the value to hwb representation.
    ///
    /// @return Returns the current to hwb value.
    /// @note This function does not throw exceptions.
    Hwb Linear::to_hwb() const noexcept { return Hwb::from_linear(*this); }

    /// Converts the value to lab representation.
    ///
    /// @return Returns the current to lab value.
    /// @note This function does not throw exceptions.
    Lab Linear::to_lab() const noexcept { return Lab::from_linear(*this); }

    /// Converts the value to lch representation.
    ///
    /// @return Returns the current to lch value.
    /// @note This function does not throw exceptions.
    Lch Linear::to_lch() const noexcept { return Lch::from_linear(*this); }

    /// Converts the value to luv representation.
    ///
    /// @return Returns the current to luv value.
    /// @note This function does not throw exceptions.
    Luv Linear::to_luv() const noexcept { return Luv::from_linear(*this); }

    /// Converts the value to oklab representation.
    ///
    /// @return Returns the current to oklab value.
    /// @note This function does not throw exceptions.
    Oklab Linear::to_oklab() const noexcept { return Oklab::from_linear(*this); }

    /// Converts the value to oklch representation.
    ///
    /// @return Returns the current to oklch value.
    /// @note This function does not throw exceptions.
    Oklch Linear::to_oklch() const noexcept { return Oklch::from_linear(*this); }

} // namespace SFT::Foundation::Color

