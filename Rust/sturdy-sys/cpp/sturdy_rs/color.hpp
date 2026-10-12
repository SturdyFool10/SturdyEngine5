// Rust <-> SturdyEngine 5 shim for the color library and color-space math. See src/color.rs.
#pragma once

#include <array>
#include <cstdint>

#include <rust/cxx.h>

namespace sturdy_rs::color {

    struct Matrix3;
    struct PrimariesDesc;
    struct PrimariesResult;
    struct MatrixResult;

    std::array<double, 4> color_convert(std::uint8_t from, std::uint8_t to, std::array<double, 4> c);
    std::array<double, 4> color_lerp(std::uint8_t ls, std::array<double, 4> l, std::uint8_t rs, std::array<double, 4> r, double t);
    std::array<double, 4> color_mix(std::uint8_t ls, std::array<double, 4> l, std::uint8_t rs, std::array<double, 4> r, double t);
    std::array<double, 4> color_modulate(std::uint8_t ls, std::array<double, 4> l, std::uint8_t rs, std::array<double, 4> r);
    std::array<double, 4> color_over(std::uint8_t fs, std::array<double, 4> f, std::uint8_t bs, std::array<double, 4> b);
    std::array<double, 4> color_premultiply(std::uint8_t space, std::array<double, 4> c);
    std::array<double, 4> color_unpremultiply(std::uint8_t space, std::array<double, 4> c);
    std::array<double, 4> color_with_alpha(std::uint8_t space, std::array<double, 4> c, double alpha);
    std::array<double, 4> color_clamp_rgb(std::uint8_t space, std::array<double, 4> c, double low, double high);
    double color_srgb_to_linear_channel(double value) noexcept;
    double color_linear_to_srgb_channel(double value) noexcept;

    PrimariesResult primaries_from_h273(std::uint8_t code) noexcept;
    PrimariesDesc working_space_primaries() noexcept;
    Matrix3 rgb_to_xyz_matrix(const PrimariesDesc &primaries) noexcept;
    Matrix3 xyz_to_rgb_matrix(const PrimariesDesc &primaries) noexcept;
    Matrix3 chromatic_adaptation_matrix(std::array<float, 2> from_white, std::array<float, 2> to_white) noexcept;
    Matrix3 color_conversion_matrix(const PrimariesDesc &from, const PrimariesDesc &to) noexcept;
    Matrix3 matrix3_multiply(const Matrix3 &lhs, const Matrix3 &rhs) noexcept;
    MatrixResult matrix3_inverse(const Matrix3 &matrix) noexcept;
    std::array<float, 3> matrix3_apply(const Matrix3 &matrix, std::array<float, 3> rgb) noexcept;
    std::array<float, 3> map_into_gamut(std::array<float, 3> rgb, std::uint8_t mapping, float maximum) noexcept;

    std::uint8_t transfer_function_from_h273(std::uint8_t code) noexcept;
    float pq_eotf_to_linear(float normalized) noexcept;
    float hlg_eotf_to_linear(float normalized) noexcept;
    std::uint16_t float_to_half(float value) noexcept;
    float half_to_float(std::uint16_t value) noexcept;

} // namespace sturdy_rs::color
