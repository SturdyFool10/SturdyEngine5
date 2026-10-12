#include "sturdy_rs/color.hpp"

#include <variant>

#include <Engine/ColorSpace.hpp>
#include <Engine/HdrTransfer.hpp>
#include <Foundation/Color.hpp>

// Generated from src/color.rs.
#include "sturdy_rs/color_bridge.h"

namespace sturdy_rs::color {
    namespace {

        namespace C = SFT::Foundation::Color;
        namespace D = SFT::Engine::Detail;

        using Arr = std::array<double, 4>;

        template <class T>
        T make(const Arr &c) noexcept {
            return T{c[0], c[1], c[2], c[3]};
        }

        template <class T>
        Arr pack(const T &value) noexcept {
            const auto [a, b, c, d] = value;
            return {a, b, c, d};
        }

        /// Calls `f.template operator()<T>()` for the color type at `index` (0..13, see color.rs).
        template <class F>
        auto with_space(std::uint8_t index, F &&f) {
            switch (index) {
            case 0: return f.template operator()<C::Srgb>();
            case 1: return f.template operator()<C::Linear>();
            case 2: return f.template operator()<C::Xyz>();
            case 3: return f.template operator()<C::AdobeRgb>();
            case 4: return f.template operator()<C::DisplayP3>();
            case 5: return f.template operator()<C::Rec2020>();
            case 6: return f.template operator()<C::Hsl>();
            case 7: return f.template operator()<C::Hsv>();
            case 8: return f.template operator()<C::Hwb>();
            case 9: return f.template operator()<C::Lab>();
            case 10: return f.template operator()<C::Lch>();
            case 11: return f.template operator()<C::Luv>();
            case 12: return f.template operator()<C::Oklab>();
            default: return f.template operator()<C::Oklch>();
            }
        }

        D::Chromaticity chroma(std::array<float, 2> v) noexcept { return {v[0], v[1]}; }
        std::array<float, 2> chroma(const D::Chromaticity &c) noexcept { return {c.x, c.y}; }

        D::ColorPrimaries to_engine(const PrimariesDesc &p) noexcept {
            return D::ColorPrimaries{chroma(p.red), chroma(p.green), chroma(p.blue), chroma(p.white)};
        }
        PrimariesDesc to_bridge(const D::ColorPrimaries &p) noexcept {
            return PrimariesDesc{chroma(p.red), chroma(p.green), chroma(p.blue), chroma(p.white)};
        }
        D::ColorMatrix3 to_engine(const Matrix3 &m) noexcept { return D::ColorMatrix3{m.m}; }
        Matrix3 to_bridge(const D::ColorMatrix3 &m) noexcept { return Matrix3{m.m}; }

    } // namespace

    Arr color_convert(std::uint8_t from, std::uint8_t to, Arr c) {
        return with_space(from, [&]<class From>() {
            return with_space(to, [&]<class To>() { return pack(C::convert_to<To>(make<From>(c))); });
        });
    }

    Arr color_lerp(std::uint8_t ls, Arr l, std::uint8_t rs, Arr r, double t) {
        return with_space(ls, [&]<class L>() {
            return with_space(rs, [&]<class R>() { return pack(C::lerp(make<L>(l), make<R>(r), t)); });
        });
    }

    Arr color_mix(std::uint8_t ls, Arr l, std::uint8_t rs, Arr r, double t) {
        return with_space(ls, [&]<class L>() {
            return with_space(rs, [&]<class R>() { return pack(C::mix(make<L>(l), make<R>(r), t)); });
        });
    }

    Arr color_modulate(std::uint8_t ls, Arr l, std::uint8_t rs, Arr r) {
        return with_space(ls, [&]<class L>() {
            return with_space(rs, [&]<class R>() { return pack(C::modulate(make<L>(l), make<R>(r))); });
        });
    }

    Arr color_over(std::uint8_t fs, Arr f, std::uint8_t bs, Arr b) {
        return with_space(fs, [&]<class F>() {
            return with_space(bs, [&]<class B>() { return pack(C::over(make<F>(f), make<B>(b))); });
        });
    }

    Arr color_premultiply(std::uint8_t space, Arr c) {
        return with_space(space, [&]<class T>() { return pack(C::premultiply_alpha(make<T>(c))); });
    }
    Arr color_unpremultiply(std::uint8_t space, Arr c) {
        return with_space(space, [&]<class T>() { return pack(C::unpremultiply_alpha(make<T>(c))); });
    }
    Arr color_with_alpha(std::uint8_t space, Arr c, double alpha) {
        return with_space(space, [&]<class T>() { return pack(C::with_alpha(make<T>(c), alpha)); });
    }
    Arr color_clamp_rgb(std::uint8_t space, Arr c, double low, double high) {
        return with_space(space, [&]<class T>() { return pack(C::clamp_rgb(make<T>(c), low, high)); });
    }
    double color_srgb_to_linear_channel(double v) noexcept { return C::srgb_to_linear_channel(v); }
    double color_linear_to_srgb_channel(double v) noexcept { return C::linear_to_srgb_channel(v); }

    PrimariesResult primaries_from_h273(std::uint8_t code) noexcept {
        const auto p = D::primaries_from_h273(code);
        return p ? PrimariesResult{true, to_bridge(*p)} : PrimariesResult{};
    }
    PrimariesDesc working_space_primaries() noexcept { return to_bridge(D::working_space_primaries()); }
    Matrix3 rgb_to_xyz_matrix(const PrimariesDesc &p) noexcept { return to_bridge(D::rgb_to_xyz_matrix(to_engine(p))); }
    Matrix3 xyz_to_rgb_matrix(const PrimariesDesc &p) noexcept { return to_bridge(D::xyz_to_rgb_matrix(to_engine(p))); }
    Matrix3 chromatic_adaptation_matrix(std::array<float, 2> from_white, std::array<float, 2> to_white) noexcept {
        return to_bridge(D::chromatic_adaptation_matrix(chroma(from_white), chroma(to_white)));
    }
    Matrix3 color_conversion_matrix(const PrimariesDesc &from, const PrimariesDesc &to) noexcept {
        return to_bridge(D::color_conversion_matrix(to_engine(from), to_engine(to)));
    }
    Matrix3 matrix3_multiply(const Matrix3 &l, const Matrix3 &r) noexcept {
        return to_bridge(D::multiply(to_engine(l), to_engine(r)));
    }
    MatrixResult matrix3_inverse(const Matrix3 &m) noexcept {
        const auto inv = D::inverse(to_engine(m));
        return inv ? MatrixResult{true, to_bridge(*inv)} : MatrixResult{};
    }
    std::array<float, 3> matrix3_apply(const Matrix3 &m, std::array<float, 3> rgb) noexcept {
        return to_engine(m).apply(rgb);
    }
    std::array<float, 3> map_into_gamut(std::array<float, 3> rgb, std::uint8_t mapping, float maximum) noexcept {
        return D::map_into_gamut(rgb, mapping == 1 ? D::GamutMapping::Desaturate : D::GamutMapping::Clip, maximum);
    }

    std::uint8_t transfer_function_from_h273(std::uint8_t code) noexcept {
        return static_cast<std::uint8_t>(D::transfer_function_from_h273(code));
    }
    float pq_eotf_to_linear(float v) noexcept { return D::pq_eotf_to_linear(v); }
    float hlg_eotf_to_linear(float v) noexcept { return D::hlg_eotf_to_linear(v); }
    std::uint16_t float_to_half(float v) noexcept { return D::float_to_half(v); }
    float half_to_float(std::uint16_t v) noexcept { return D::half_to_float(v); }

} // namespace sturdy_rs::color
