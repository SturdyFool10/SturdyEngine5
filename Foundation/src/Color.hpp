#pragma once

#include <Foundation/src/Types.hpp>

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

    [[nodiscard]] f64 clamp01(f64 value) noexcept;
    [[nodiscard]] f64 wrap_degrees(f64 hue) noexcept;
    [[nodiscard]] f64 srgb_to_linear_channel(f64 value) noexcept;
    [[nodiscard]] f64 linear_to_srgb_channel(f64 value) noexcept;

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
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Srgb from_linear(const Linear &c) noexcept;
    };

    struct Xyz {
        f64 x = 0.0;
        f64 y = 0.0;
        f64 z = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Xyz &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Xyz from_linear(const Linear &c) noexcept;
    };

    struct AdobeRgb {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const AdobeRgb &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static AdobeRgb from_linear(const Linear &c) noexcept;
    };

    struct DisplayP3 {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const DisplayP3 &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static DisplayP3 from_linear(const Linear &c) noexcept;
    };

    struct Rec2020 {
        f64 r = 0.0;
        f64 g = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Rec2020 &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Rec2020 from_linear(const Linear &c) noexcept;
    };

    struct Hsl {
        f64 h = 0.0;
        f64 s = 0.0;
        f64 l = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hsl &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Hsl from_linear(const Linear &c) noexcept;
    };

    struct Hsv {
        f64 h = 0.0;
        f64 s = 0.0;
        f64 v = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hsv &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Hsv from_linear(const Linear &c) noexcept;
    };

    struct Hwb {
        f64 h = 0.0;
        f64 w = 0.0;
        f64 b = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Hwb &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Hwb from_linear(const Linear &c) noexcept;
    };

    struct Lab {
        f64 l = 0.0;
        f64 a = 0.0;
        f64 b = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Lab &) const = default;
        [[nodiscard]] static f64 f(f64 t) noexcept;
        [[nodiscard]] static f64 f_inv(f64 u) noexcept;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Lab from_linear(const Linear &c) noexcept;
    };

    struct Lch {
        f64 l = 0.0;
        f64 c = 0.0;
        f64 h = 0.0;
        f64 a = 1.0;
        [[nodiscard]] auto operator<=>(const Lch &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Lch from_linear(const Linear &color) noexcept;
    };

    struct Luv {
        f64 l = 0.0;
        f64 u = 0.0;
        f64 v = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Luv &) const = default;
        [[nodiscard]] static f64 u_prime(f64 x, f64 y, f64 z) noexcept;
        [[nodiscard]] static f64 v_prime(f64 x, f64 y, f64 z) noexcept;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Luv from_linear(const Linear &c) noexcept;
    };

    struct Oklab {
        f64 l = 0.0;
        f64 a = 0.0;
        f64 b = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Oklab &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Oklab from_linear(const Linear &c) noexcept;
    };

    struct Oklch {
        f64 l = 0.0;
        f64 c = 0.0;
        f64 h = 0.0;
        f64 alpha = 1.0;
        [[nodiscard]] auto operator<=>(const Oklch &) const = default;
        [[nodiscard]] Linear to_linear() const noexcept;
        [[nodiscard]] static Oklch from_linear(const Linear &color) noexcept;
    };

    
    
    
    
    
    
    
    
    
    
    
    
    

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
