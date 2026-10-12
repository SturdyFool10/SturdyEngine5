//! The engine's color library: conversion, interpolation and compositing across 14 color spaces,
//! primaries/gamut matrices, and HDR transfer functions. Pure math — no [`crate::Engine`] needed.
//!
//! A [`Color`] is a space plus four `f64` components in that space's own order (see [`Space`]).
//!
//! ```
//! use sturdy::color::{Color, Space};
//! let orange = Color::srgb(1.0, 0.5, 0.0, 1.0);
//! let oklch = orange.convert(Space::Oklch);
//! assert!(oklch.components[0] > 0.6); // perceptual lightness
//! ```

use glam::{Mat3, Vec2, Vec3};
use sturdy_sys::color::ffi;

/// A color space, in the engine's fixed order. Component order: sRGB/Linear/AdobeRGB/DisplayP3/
/// Rec2020 `r g b a`; XYZ `x y z alpha`; HSL `h s l a`; HSV `h s v a`; HWB `h w b a`; Lab
/// `l a b alpha`; LCH `l c h a`; Luv `l u v alpha`; Oklab `l a b alpha`; Oklch `l c h a`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Space {
    Srgb = 0,
    Linear,
    Xyz,
    AdobeRgb,
    DisplayP3,
    Rec2020,
    Hsl,
    Hsv,
    Hwb,
    Lab,
    Lch,
    Luv,
    Oklab,
    Oklch,
}

/// A color in a specific space.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Color {
    pub space: Space,
    pub components: [f64; 4],
}

impl Color {
    pub fn new(space: Space, components: [f64; 4]) -> Self {
        Self { space, components }
    }

    /// Straight-alpha sRGB (gamma-encoded), components 0..1.
    pub fn srgb(r: f64, g: f64, b: f64, a: f64) -> Self {
        Self::new(Space::Srgb, [r, g, b, a])
    }

    /// Scene-linear RGB (BT.709 primaries).
    pub fn linear(r: f64, g: f64, b: f64, a: f64) -> Self {
        Self::new(Space::Linear, [r, g, b, a])
    }

    pub fn alpha(&self) -> f64 {
        self.components[3]
    }

    /// The same color expressed in `to`.
    #[must_use]
    pub fn convert(&self, to: Space) -> Self {
        Self::new(to, ffi::color_convert(self.space as u8, to as u8, self.components))
    }

    /// Component-wise interpolation in `self`'s space (`other` is converted into it first).
    #[must_use]
    pub fn lerp(&self, other: &Color, t: f64) -> Self {
        Self::new(self.space, ffi::color_lerp(self.space as u8, self.components, other.space as u8, other.components, t))
    }

    /// The library's perceptual mix.
    #[must_use]
    pub fn mix(&self, other: &Color, t: f64) -> Self {
        Self::new(self.space, ffi::color_mix(self.space as u8, self.components, other.space as u8, other.components, t))
    }

    /// Component-wise product (tinting).
    #[must_use]
    pub fn modulate(&self, other: &Color) -> Self {
        Self::new(self.space, ffi::color_modulate(self.space as u8, self.components, other.space as u8, other.components))
    }

    /// `self` composited over `background`, in `self`'s space.
    #[must_use]
    pub fn over(&self, background: &Color) -> Self {
        Self::new(self.space, ffi::color_over(self.space as u8, self.components, background.space as u8, background.components))
    }

    #[must_use]
    pub fn premultiplied(&self) -> Self {
        Self::new(self.space, ffi::color_premultiply(self.space as u8, self.components))
    }

    #[must_use]
    pub fn unpremultiplied(&self) -> Self {
        Self::new(self.space, ffi::color_unpremultiply(self.space as u8, self.components))
    }

    #[must_use]
    pub fn with_alpha(&self, alpha: f64) -> Self {
        Self::new(self.space, ffi::color_with_alpha(self.space as u8, self.components, alpha))
    }

    /// Clamps the RGB channels of an RGB-family color into `low..=high`.
    #[must_use]
    pub fn clamp_rgb(&self, low: f64, high: f64) -> Self {
        Self::new(self.space, ffi::color_clamp_rgb(self.space as u8, self.components, low, high))
    }

    /// `[r, g, b, a]` as `f32` in sRGB — the form UI and materials take.
    pub fn to_srgb_f32(&self) -> [f32; 4] {
        let c = self.convert(Space::Srgb).components;
        [c[0] as f32, c[1] as f32, c[2] as f32, c[3] as f32]
    }
}

/// The sRGB transfer function's decode, one channel.
pub fn srgb_to_linear(value: f64) -> f64 {
    ffi::color_srgb_to_linear_channel(value)
}

/// The sRGB transfer function's encode, one channel.
pub fn linear_to_srgb(value: f64) -> f64 {
    ffi::color_linear_to_srgb_channel(value)
}

// -- Primaries and gamut ----------------------------------------------------------------------

/// RGB primaries and white point as CIE xy chromaticities.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Primaries {
    pub red: Vec2,
    pub green: Vec2,
    pub blue: Vec2,
    pub white: Vec2,
}

impl Primaries {
    fn to_ffi(self) -> ffi::PrimariesDesc {
        ffi::PrimariesDesc { red: self.red.into(), green: self.green.into(), blue: self.blue.into(), white: self.white.into() }
    }

    fn from_ffi(p: ffi::PrimariesDesc) -> Self {
        Self { red: p.red.into(), green: p.green.into(), blue: p.blue.into(), white: p.white.into() }
    }

    /// Primaries for an ITU-T H.273 color-primaries code (1 = BT.709/sRGB, 9 = BT.2020, 12 = P3, ...).
    pub fn from_h273(code: u8) -> Option<Self> {
        let r = ffi::primaries_from_h273(code);
        r.found.then(|| Self::from_ffi(r.primaries))
    }

    /// The engine's working space (BT.709/sRGB primaries).
    pub fn working_space() -> Self {
        Self::from_ffi(ffi::working_space_primaries())
    }

    /// Linear RGB -> CIE XYZ.
    pub fn rgb_to_xyz(&self) -> Mat3 {
        to_mat3(ffi::rgb_to_xyz_matrix(&self.to_ffi()))
    }

    pub fn xyz_to_rgb(&self) -> Mat3 {
        to_mat3(ffi::xyz_to_rgb_matrix(&self.to_ffi()))
    }

    /// Linear RGB in `self` -> linear RGB in `to`, chromatically adapted between the white points.
    pub fn conversion_to(&self, to: &Primaries) -> Mat3 {
        to_mat3(ffi::color_conversion_matrix(&self.to_ffi(), &to.to_ffi()))
    }
}

// The engine's matrices are row-major; glam's `Mat3` is column-major, so transpose on the way in/out.
fn to_mat3(m: ffi::Matrix3) -> Mat3 {
    Mat3::from_cols_array(&m.m).transpose()
}

/// Bradford-style chromatic adaptation between two white points.
pub fn chromatic_adaptation(from_white: Vec2, to_white: Vec2) -> Mat3 {
    to_mat3(ffi::chromatic_adaptation_matrix(from_white.into(), to_white.into()))
}

/// How out-of-gamut colors are brought into range.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GamutMapping {
    Clip,
    Desaturate,
}

/// Maps a linear RGB triple into `0..=maximum`.
pub fn map_into_gamut(rgb: Vec3, mapping: GamutMapping, maximum: f32) -> Vec3 {
    Vec3::from(ffi::map_into_gamut(rgb.into(), mapping as u8, maximum))
}

// -- HDR transfer -----------------------------------------------------------------------------

/// A transfer function identified from an ITU-T H.273 code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransferFunction {
    /// sRGB / BT.709 (codes 13 and 1).
    Srgb,
    Linear,
    /// SMPTE ST 2084 (code 16).
    Pq,
    /// ARIB STD-B67 (code 18).
    Hlg,
    /// A curve the engine doesn't implement.
    Unsupported,
}

impl TransferFunction {
    pub fn from_h273(code: u8) -> Self {
        match ffi::transfer_function_from_h273(code) {
            0 => Self::Srgb,
            1 => Self::Linear,
            2 => Self::Pq,
            3 => Self::Hlg,
            _ => Self::Unsupported,
        }
    }
}

/// PQ (ST 2084) EOTF: normalized signal `0..=1` to scene-linear light in the engine's units, where
/// 1.0 is the 100-nit reference white (so a full-scale 10 000-nit signal returns 100.0).
pub fn pq_to_linear(normalized: f32) -> f32 {
    ffi::pq_eotf_to_linear(normalized)
}

/// HLG EOTF: normalized signal to linear light.
pub fn hlg_to_linear(normalized: f32) -> f32 {
    ffi::hlg_eotf_to_linear(normalized)
}

/// IEEE binary16 bits of an `f32`.
pub fn f32_to_half(value: f32) -> u16 {
    ffi::float_to_half(value)
}

pub fn half_to_f32(bits: u16) -> f32 {
    ffi::half_to_float(bits)
}
