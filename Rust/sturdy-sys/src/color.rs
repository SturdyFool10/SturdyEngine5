//! Raw `cxx` bindings for the engine's color library (`Foundation/Color.hpp`), color-space math
//! (`Engine/ColorSpace.hpp`) and HDR transfer functions (`Engine/HdrTransfer.hpp`).
//!
//! Colors cross as four `f64` components in the order of the space's own fields (e.g. `r, g, b, a`
//! for RGB spaces, `h, s, l, a` for HSL, `l, a, b, alpha` for Lab). Spaces are addressed by index
//! in the fixed order below. Everything is pure math, independent of any `Engine`.

#[cxx::bridge(namespace = "sturdy_rs::color")]
pub mod ffi {
    /// A 3x3 matrix, row-major.
    #[derive(Debug, Clone, Copy, Default, PartialEq)]
    struct Matrix3 {
        m: [f32; 9],
    }

    /// CIE xy chromaticity coordinates of a color space's primaries and white point.
    #[derive(Debug, Clone, Copy, Default, PartialEq)]
    struct PrimariesDesc {
        red: [f32; 2],
        green: [f32; 2],
        blue: [f32; 2],
        white: [f32; 2],
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct PrimariesResult {
        found: bool,
        primaries: PrimariesDesc,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct MatrixResult {
        found: bool,
        matrix: Matrix3,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/color.hpp");

        // -- Foundation::Color -------------------------------------------------------------------
        // Space indices: 0 sRGB, 1 Linear, 2 XYZ, 3 AdobeRGB, 4 DisplayP3, 5 Rec2020, 6 HSL, 7 HSV,
        // 8 HWB, 9 Lab, 10 LCH, 11 Luv, 12 Oklab, 13 Oklch.
        fn color_convert(from: u8, to: u8, c: [f64; 4]) -> [f64; 4];
        /// Interpolates component-wise in `left_space` (`right` is converted into it first).
        fn color_lerp(left_space: u8, left: [f64; 4], right_space: u8, right: [f64; 4], t: f64) -> [f64; 4];
        /// Perceptual mix (interpolates in a mixing space the library chooses).
        fn color_mix(left_space: u8, left: [f64; 4], right_space: u8, right: [f64; 4], t: f64) -> [f64; 4];
        fn color_modulate(left_space: u8, left: [f64; 4], right_space: u8, right: [f64; 4]) -> [f64; 4];
        /// `foreground` composited over `background`, in the foreground's space.
        fn color_over(fg_space: u8, fg: [f64; 4], bg_space: u8, bg: [f64; 4]) -> [f64; 4];
        fn color_premultiply(space: u8, c: [f64; 4]) -> [f64; 4];
        fn color_unpremultiply(space: u8, c: [f64; 4]) -> [f64; 4];
        fn color_with_alpha(space: u8, c: [f64; 4], alpha: f64) -> [f64; 4];
        fn color_clamp_rgb(space: u8, c: [f64; 4], low: f64, high: f64) -> [f64; 4];
        fn color_srgb_to_linear_channel(value: f64) -> f64;
        fn color_linear_to_srgb_channel(value: f64) -> f64;

        // -- Engine color-space math ------------------------------------------------------------
        fn primaries_from_h273(code: u8) -> PrimariesResult;
        fn working_space_primaries() -> PrimariesDesc;
        fn rgb_to_xyz_matrix(primaries: &PrimariesDesc) -> Matrix3;
        fn xyz_to_rgb_matrix(primaries: &PrimariesDesc) -> Matrix3;
        fn chromatic_adaptation_matrix(from_white: [f32; 2], to_white: [f32; 2]) -> Matrix3;
        fn color_conversion_matrix(from: &PrimariesDesc, to: &PrimariesDesc) -> Matrix3;
        fn matrix3_multiply(lhs: &Matrix3, rhs: &Matrix3) -> Matrix3;
        fn matrix3_inverse(matrix: &Matrix3) -> MatrixResult;
        fn matrix3_apply(matrix: &Matrix3, rgb: [f32; 3]) -> [f32; 3];
        /// `mapping`: 0 = clip, 1 = desaturate.
        fn map_into_gamut(rgb: [f32; 3], mapping: u8, maximum: f32) -> [f32; 3];

        // -- HDR transfer functions -------------------------------------------------------------
        /// 0 sRGB/BT.709, 1 linear, 2 PQ, 3 HLG, 4 unsupported.
        fn transfer_function_from_h273(code: u8) -> u8;
        fn pq_eotf_to_linear(normalized: f32) -> f32;
        fn hlg_eotf_to_linear(normalized: f32) -> f32;
        fn float_to_half(value: f32) -> u16;
        fn half_to_float(value: u16) -> f32;
    }
}
