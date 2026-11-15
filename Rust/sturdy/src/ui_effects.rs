//! Shader effects on whole UI elements, corner shapes and UI layers.
//!
//! An effect is made once, set on [`ElementDesc::effect`](crate::ui::ElementDesc::effect) and shared by any number of elements; the
//! engine draws the element and everything inside it into its own image, runs it through the effect's passes and composites the
//! result back where the element was. Library effects cover what a web developer reaches for (blur, drop and inset shadows, glow,
//! outline, colour filters, gradients in any CSS colour space, frosted glass); [`UiEffect::custom`] builds arbitrary multi-pass effects
//! out of your own Slang fragment shaders, with off-screen intermediates in any format and mip chains.
//!
//! ```no_run
//! use sturdy::ui::{ElementDesc, Sizing};
//! use sturdy::ui_effects::{CornerShape, CornerShapes, UiEffect};
//!
//! let shadow = UiEffect::drop_shadow(glam::vec2(0.0, 4.0), 8.0, glam::vec4(0.0, 0.0, 0.0, 0.5), 1.0).unwrap();
//! let card = ElementDesc::new()
//!     .width(Sizing::Fixed(200.0))
//!     .height(Sizing::Fixed(120.0))
//!     .background(glam::vec4(1.0, 1.0, 1.0, 1.0))
//!     .corner_shape(CornerShapes::all(CornerShape::SQUIRCLE))
//!     .effect(Some(shadow));
//! ```
//!
//! **Layers.** An effect with `layer >= 1` ([`EffectOptions::layer`], or [`UiEffect::layer`]) hoists its element into a later pass of the
//! frame, where effects can read what the lower layers drew ([`InputKind::Backdrop`]): frosted glass is the usual case.

use std::sync::Arc;

use sturdy_sys::ui::ffi;

use crate::ui::ElementDesc;

/// CSS Color Module 4 colour spaces. Components are in the space's reference ranges: the sRGB-style spaces 0..1, HSL/HWB saturation,
/// lightness, whiteness and blackness 0..100, hue in degrees, Lab L 0..100, Oklab L 0..1.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum ColorSpace {
    #[default]
    Srgb = 0,
    SrgbLinear = 1,
    DisplayP3 = 2,
    DisplayP3Linear = 3,
    A98Rgb = 4,
    ProphotoRgb = 5,
    Rec2020 = 6,
    XyzD50 = 7,
    XyzD65 = 8,
    Lab = 9,
    Oklab = 10,
    Hsl = 11,
    Hwb = 12,
    Lch = 13,
    Oklch = 14,
}

/// How the hue travels between two colours in a polar space (HSL, HWB, LCH, OKLCH).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum HueMethod {
    #[default]
    Shorter = 0,
    Longer = 1,
    Increasing = 2,
    Decreasing = 3,
}

/// The space (and hue method) colours are mixed in. Oklab by default, as everywhere in the engine's UI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Interpolation {
    pub space: ColorSpace,
    pub hue: HueMethod,
}

impl Default for Interpolation {
    fn default() -> Self {
        Self { space: ColorSpace::Oklab, hue: HueMethod::Shorter }
    }
}

/// How a finished effect is laid over what is already drawn.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum Composite {
    /// Premultiplied source-over.
    #[default]
    Normal = 0,
    /// Adds the result: glow, light.
    Additive = 1,
    /// Multiplies the destination by the result: shadows, vignettes.
    Multiply = 2,
    /// Overwrites the destination inside the region, so a transparent result cuts a hole.
    Replace = 3,
}

/// Applied on top of any effect.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EffectOptions {
    pub composite: Composite,
    /// Multiplies the composited result, 0..1.
    pub opacity: f32,
    /// Pixels the result may spread beyond the element's box: left, right, top, bottom. All zero keeps a library effect's own (a blur or
    /// shadow sizes itself); a custom effect that draws outside its element must set it.
    pub outset: [f32; 4],
    /// Draw the result only inside the element's box plus the outset.
    pub clip_to_bounds: bool,
    /// 0 draws the element in place; 1 and up hoist it (top-level elements only) into a later pass of the frame.
    pub layer: u32,
    /// Mips made of the element's content for [`InputKind::GroupContent`] inputs read at several scales: 1 none, 0 the whole chain.
    pub content_mip_levels: u32,
}

impl Default for EffectOptions {
    fn default() -> Self {
        Self { composite: Composite::Normal, opacity: 1.0, outset: [0.0; 4], clip_to_bounds: true, layer: 0, content_mip_levels: 1 }
    }
}

impl EffectOptions {
    fn to_ffi(self) -> ffi::UiEffectOptions {
        ffi::UiEffectOptions {
            composite: self.composite as u32,
            opacity: self.opacity,
            outset: self.outset,
            clip_to_bounds: self.clip_to_bounds,
            layer: self.layer,
            content_mip_levels: self.content_mip_levels,
        }
    }
}

/// One colour stop of a gradient (two or three, ascending by position).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GradientStop {
    /// Straight alpha, components in `space`.
    pub color: glam::Vec4,
    /// 0..1 along the gradient.
    pub position: f32,
    pub space: ColorSpace,
    /// The stretch from this stop to the next one mixes like this instead of the gradient's own interpolation.
    pub toward_next: Option<Interpolation>,
}

impl GradientStop {
    /// An sRGB stop, like a hex colour.
    pub fn new(color: glam::Vec4, position: f32) -> Self {
        Self { color, position, space: ColorSpace::Srgb, toward_next: None }
    }

    /// A stop written in another colour space (`ColorSpace::Oklch`, `DisplayP3`, ...).
    pub fn in_space(color: glam::Vec4, position: f32, space: ColorSpace) -> Self {
        Self { color, position, space, toward_next: None }
    }

    /// Mix the stretch to the next stop in `interpolation`.
    #[must_use]
    pub fn toward_next(mut self, interpolation: Interpolation) -> Self {
        self.toward_next = Some(interpolation);
        self
    }

    fn to_ffi(self) -> ffi::UiEffectStop {
        let toward = self.toward_next.unwrap_or_default();
        ffi::UiEffectStop {
            color: self.color.into(),
            position: self.position,
            space: self.space as u32,
            has_toward_next: self.toward_next.is_some(),
            toward_space: toward.space as u32,
            toward_hue: toward.hue as u32,
        }
    }
}

/// Filters that change an element's colour as a whole. The defaults change nothing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorAdjust {
    pub brightness: f32,
    pub contrast: f32,
    pub saturation: f32,
    pub grayscale: f32,
    pub sepia: f32,
    pub invert: f32,
    pub hue_radians: f32,
    pub opacity: f32,
}

impl Default for ColorAdjust {
    fn default() -> Self {
        Self { brightness: 1.0, contrast: 1.0, saturation: 1.0, grayscale: 0.0, sepia: 0.0, invert: 0.0, hue_radians: 0.0, opacity: 1.0 }
    }
}

/// What an input of a custom pass reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum InputKind {
    /// What the element and everything inside it drew, premultiplied RGBA at the element's pixel size.
    #[default]
    GroupContent = 0,
    /// The image an earlier pass of the same effect wrote (`pass`).
    PassOutput = 1,
    /// A renderer texture you own (`texture`).
    Texture = 2,
    /// What is already drawn behind the element: the scene and the lower layers. The element must be in a layer above the first.
    Backdrop = 3,
}

/// One texture a custom pass samples: the first is `Texture2D sourceTexture` in the shader, the rest `extraTexture0`, ...
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct PassInput {
    pub kind: InputKind,
    pub pass: u32,
    pub texture: u64,
    pub base_mip: u32,
    pub mip_count: u32,
}

impl PassInput {
    pub fn group_content() -> Self {
        Self::default()
    }
    pub fn pass_output(pass: u32) -> Self {
        Self { kind: InputKind::PassOutput, pass, ..Self::default() }
    }
    pub fn backdrop() -> Self {
        Self { kind: InputKind::Backdrop, ..Self::default() }
    }
    pub fn texture(texture: u64) -> Self {
        Self { kind: InputKind::Texture, texture, ..Self::default() }
    }
}

/// One pass of a custom effect: a fragment shader drawn over a full-target triangle. The Slang file defines `vertexMain` and
/// `fragmentMain`, declares its inputs, and one `[[push_constant]]` block that starts with the engine's 48-byte `UiEffectConstants`
/// (`Shaders/sturdy_common.slang`) followed by `parameters`.
#[derive(Debug, Clone, PartialEq)]
pub struct EffectPass {
    pub label: String,
    pub shader_path: String,
    pub module_name: String,
    pub fragment_entry_point: String,
    pub inputs: Vec<PassInput>,
    /// Bilinear (true) or nearest; trilinear between mips or nearest mip.
    pub linear_filter: bool,
    pub linear_mips: bool,
    /// Sample magnified inputs (a smaller source drawn larger) with `linear_mag_filter` instead of `linear_filter`, which then only
    /// applies to minified ones: nearest up and linear down, say.
    pub separate_mag_filter: bool,
    pub linear_mag_filter: bool,
    /// 0 clamp, 1 repeat, 2 mirror, 3 border: the horizontal mode, and the vertical one too unless `separate_address_v` is set.
    pub address: u32,
    /// Give the vertical axis its own mode, `address_v` (0 clamp, 1 repeat, 2 mirror, 3 border).
    pub separate_address_v: bool,
    pub address_v: u32,
    pub lod_bias: f32,
    pub min_lod: f32,
    pub max_lod: f32,
    pub max_anisotropy: f32,
    /// An `RHI::Format` value for the pass's target, or 0 for half-float RGBA.
    pub output_format: u32,
    /// Target size relative to the element's pixel size (0.5 = half width and height).
    pub output_scale: f32,
    /// Mips to allocate for the target: 1 none, 0 the whole chain.
    pub output_mip_levels: u32,
    /// How a lower mip is made: 0 box, 1 tent.
    pub output_mip_filter: u32,
    /// Pushed after the 48-byte prefix: a multiple of 4 bytes, at most 80.
    pub parameters: Vec<u8>,
}

impl EffectPass {
    /// A pass of `shader_path` (module `module_name`) that reads the element's content.
    pub fn new(shader_path: impl Into<String>, module_name: impl Into<String>) -> Self {
        Self {
            label: String::new(),
            shader_path: shader_path.into(),
            module_name: module_name.into(),
            fragment_entry_point: "fragmentMain".into(),
            inputs: vec![PassInput::group_content()],
            linear_filter: true,
            linear_mips: true,
            separate_mag_filter: false,
            linear_mag_filter: true,
            address: 0,
            separate_address_v: false,
            address_v: 0,
            lod_bias: 0.0,
            min_lod: 0.0,
            max_lod: 1000.0,
            max_anisotropy: 1.0,
            output_format: 0,
            output_scale: 1.0,
            output_mip_levels: 1,
            output_mip_filter: 1,
            parameters: Vec::new(),
        }
    }

    /// Appends the bytes of plain-old-data `value` (a `glam::Vec4`, a `[f32; 4]`, ...) to the push-constant parameters.
    #[must_use]
    pub fn parameter<T: bytemuck::NoUninit>(mut self, value: &T) -> Self {
        self.parameters.extend_from_slice(bytemuck::bytes_of(value));
        self
    }

    fn to_ffi(&self) -> ffi::UiEffectPassDesc {
        ffi::UiEffectPassDesc {
            label: self.label.clone(),
            shader_path: self.shader_path.clone(),
            module_name: self.module_name.clone(),
            fragment_entry_point: self.fragment_entry_point.clone(),
            inputs: self
                .inputs
                .iter()
                .map(|i| ffi::UiEffectInput { kind: i.kind as u32, pass: i.pass, texture: i.texture, base_mip: i.base_mip, mip_count: i.mip_count })
                .collect(),
            linear_filter: self.linear_filter,
            linear_mips: self.linear_mips,
            separate_mag_filter: self.separate_mag_filter,
            linear_mag_filter: self.linear_mag_filter,
            address: self.address,
            separate_address_v: self.separate_address_v,
            address_v: self.address_v,
            lod_bias: self.lod_bias,
            min_lod: self.min_lod,
            max_lod: self.max_lod,
            max_anisotropy: self.max_anisotropy,
            output_format: self.output_format,
            output_scale: self.output_scale,
            output_mip_levels: self.output_mip_levels,
            output_mip_filter: self.output_mip_filter,
            parameters: self.parameters.clone(),
        }
    }
}

struct EffectHandle(u64);

impl Drop for EffectHandle {
    fn drop(&mut self) {
        // Frames already built keep drawing with it; only new elements can no longer name it.
        let _ = ffi::ui_effect_release(self.0);
    }
}

/// A UI effect. Cheap to clone (a shared handle); released when the last clone is dropped.
#[derive(Clone)]
pub struct UiEffect(Arc<EffectHandle>);

impl PartialEq for UiEffect {
    fn eq(&self, other: &Self) -> bool {
        self.id() == other.id()
    }
}

impl core::fmt::Debug for UiEffect {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "UiEffect({})", self.id())
    }
}

fn outcome(result: ffi::UiEffectOutcome) -> Result<u64, String> {
    if result.ok {
        Ok(result.id)
    } else {
        Err(result.message)
    }
}

impl UiEffect {
    pub(crate) fn id(&self) -> u64 {
        self.0 .0
    }

    fn make(spec: ffi::UiEffectSpec, options: EffectOptions) -> Result<Self, String> {
        outcome(ffi::ui_effect_create(&spec, &options.to_ffi())).map(|id| Self(Arc::new(EffectHandle(id))))
    }

    fn library(kind: u32, values: &[f32], color: glam::Vec4, space: ColorSpace) -> Result<Self, String> {
        let mut v = [0.0f32; 16];
        v[..values.len()].copy_from_slice(values);
        Self::make(
            ffi::UiEffectSpec {
                kind,
                values: v,
                value_count: values.len() as u32,
                texture: 0,
                color: color.into(),
                color_space: space as u32,
                interpolation_space: ColorSpace::Oklab as u32,
                interpolation_hue: HueMethod::Shorter as u32,
                stops: Vec::new(),
            },
            EffectOptions::default(),
        )
    }

    /// Replaces the effect's options (composite mode, opacity, outset, layer, ...). Do this before a frame uses the effect.
    pub fn with_options(self, options: EffectOptions) -> Result<Self, String> {
        outcome(ffi::ui_effect_set_options(self.id(), &options.to_ffi()))?;
        Ok(self)
    }

    /// Puts the element in `layer` (1 and up) so its effects can read what lower layers drew.
    pub fn in_layer(self, layer: u32) -> Result<Self, String> {
        self.with_options(EffectOptions { layer, ..EffectOptions::default() })
    }

    pub fn color_adjust(adjust: ColorAdjust) -> Result<Self, String> {
        Self::library(
            0,
            &[adjust.brightness, adjust.contrast, adjust.saturation, adjust.grayscale, adjust.sepia, adjust.invert, adjust.hue_radians, adjust.opacity],
            glam::Vec4::ONE,
            ColorSpace::Srgb,
        )
    }

    /// Multiplies the element's colour and alpha.
    pub fn tint(color: glam::Vec4) -> Result<Self, String> {
        Self::library(1, &[], color, ColorSpace::Srgb)
    }

    pub fn blur(sigma_px: f32) -> Result<Self, String> {
        Self::library(2, &[sigma_px], glam::Vec4::ONE, ColorSpace::Srgb)
    }

    /// A cheap wide blur through a mip chain; use for large radii.
    pub fn fast_blur(sigma_px: f32) -> Result<Self, String> {
        Self::library(3, &[sigma_px], glam::Vec4::ONE, ColorSpace::Srgb)
    }

    pub fn drop_shadow(offset: glam::Vec2, blur_sigma: f32, color: glam::Vec4, strength: f32) -> Result<Self, String> {
        Self::library(4, &[offset.x, offset.y, blur_sigma, strength], color, ColorSpace::Srgb)
    }

    pub fn glow(color: glam::Vec4, radius_sigma: f32, strength: f32) -> Result<Self, String> {
        Self::library(5, &[radius_sigma, strength], color, ColorSpace::Srgb)
    }

    /// A ring `width` px wide, `offset` px away from the element's edge.
    pub fn outline(width: f32, color: glam::Vec4, offset: f32) -> Result<Self, String> {
        Self::library(6, &[width, offset], color, ColorSpace::Srgb)
    }

    pub fn inset_shadow(offset: glam::Vec2, blur_sigma: f32, color: glam::Vec4, strength: f32) -> Result<Self, String> {
        Self::library(7, &[offset.x, offset.y, blur_sigma, strength], color, ColorSpace::Srgb)
    }

    /// Fills the element's shape with a blurred copy of what is behind it. Lives in a layer (`layer` 1 or more) so there is a backdrop.
    pub fn frosted_glass(radius_px: f32, tint: glam::Vec3, tint_strength: f32, saturation: f32, layer: u32) -> Result<Self, String> {
        Self::library(8, &[radius_px, tint_strength, saturation], tint.extend(1.0), ColorSpace::Srgb)?.with_options(EffectOptions {
            layer,
            ..EffectOptions::default()
        })
    }

    /// Only puts the element in `layer`.
    pub fn layer(layer: u32) -> Result<Self, String> {
        Self::library(9, &[], glam::Vec4::ONE, ColorSpace::Srgb)?.in_layer(layer)
    }

    fn gradient(kind: u32, values: &[f32], stops: &[GradientStop], interpolation: Interpolation) -> Result<Self, String> {
        let mut v = [0.0f32; 16];
        v[..values.len()].copy_from_slice(values);
        Self::make(
            ffi::UiEffectSpec {
                kind,
                values: v,
                value_count: values.len() as u32,
                texture: 0,
                color: [1.0; 4],
                color_space: 0,
                interpolation_space: interpolation.space as u32,
                interpolation_hue: interpolation.hue as u32,
                stops: stops.iter().map(|s| s.to_ffi()).collect(),
            },
            EffectOptions::default(),
        )
    }

    /// A linear gradient across the element (CSS `linear-gradient`); `angle_radians` 0 runs left to right and grows clockwise. Two or
    /// three stops. The element's own background is the shape it fills.
    pub fn linear_gradient(angle_radians: f32, stops: &[GradientStop], interpolation: Interpolation) -> Result<Self, String> {
        Self::gradient(10, &[angle_radians], stops, interpolation)
    }

    pub fn radial_gradient(center: glam::Vec2, radius: f32, stops: &[GradientStop], interpolation: Interpolation) -> Result<Self, String> {
        Self::gradient(11, &[center.x, center.y, radius], stops, interpolation)
    }

    pub fn conic_gradient(center: glam::Vec2, start_radians: f32, stops: &[GradientStop], interpolation: Interpolation) -> Result<Self, String> {
        Self::gradient(12, &[center.x, center.y, start_radians], stops, interpolation)
    }

    /// A 2D transform (CSS `transform` + `transform-origin`): scaled, then rotated (`angle_radians`, clockwise), then moved by `translate`
    /// pixels, about `origin` (0..1 across the box). Layout and hit testing are unchanged. Draws anywhere on the surface unless `outset`
    /// (left, right, top, bottom pixels) bounds it.
    pub fn transform(translate: glam::Vec2, angle_radians: f32, scale: glam::Vec2, origin: glam::Vec2, outset: [f32; 4]) -> Result<Self, String> {
        Self::library(13, &[translate.x, translate.y, angle_radians, scale.x, scale.y, origin.x, origin.y], glam::Vec4::ONE, ColorSpace::Srgb)?
            .with_options(EffectOptions { outset, clip_to_bounds: outset != [0.0; 4], ..EffectOptions::default() })
    }

    fn shaped(kind: u32, values: &[f32], texture: u64) -> Result<Self, String> {
        let mut v = [0.0f32; 16];
        v[..values.len()].copy_from_slice(values);
        Self::make(
            ffi::UiEffectSpec {
                kind,
                values: v,
                value_count: values.len() as u32,
                texture,
                color: [1.0; 4],
                color_space: 0,
                interpolation_space: ColorSpace::Oklab as u32,
                interpolation_hue: HueMethod::Shorter as u32,
                stops: Vec::new(),
            },
            EffectOptions::default(),
        )
    }

    /// CSS `clip-path: inset()`: the element cut to its box shrunk by the given pixels, with rounded corners of `radius`.
    pub fn clip_inset(top: f32, right: f32, bottom: f32, left: f32, radius: f32) -> Result<Self, String> {
        Self::shaped(14, &[top, right, bottom, left, radius], 0)
    }

    /// CSS `clip-path: circle()`: radius in pixels, centre across the box (0..1).
    pub fn clip_circle(radius_px: f32, center: glam::Vec2) -> Result<Self, String> {
        Self::shaped(15, &[radius_px, center.x, center.y], 0)
    }

    /// CSS `clip-path: ellipse()`: radii in pixels, centre across the box (0..1).
    pub fn clip_ellipse(radii_px: glam::Vec2, center: glam::Vec2) -> Result<Self, String> {
        Self::shaped(16, &[radii_px.x, radii_px.y, center.x, center.y], 0)
    }

    /// CSS `clip-path: polygon()`: three to six points across the box (0..1 on each axis, y down).
    pub fn clip_polygon(points: &[glam::Vec2]) -> Result<Self, String> {
        let flat: Vec<f32> = points.iter().take(6).flat_map(|p| [p.x, p.y]).collect();
        Self::shaped(17, &flat, 0)
    }

    /// CSS `mask-image: linear-gradient()` in alpha: the element is multiplied by an alpha that runs from `alpha_start` at `start` to
    /// `alpha_end` at `end` (0..1 along the line at `angle_radians`, 0 = left to right, clockwise).
    pub fn mask_linear(angle_radians: f32, alpha_start: f32, alpha_end: f32, start: f32, end: f32, invert: bool) -> Result<Self, String> {
        Self::shaped(18, &[angle_radians, alpha_start, alpha_end, start, end, if invert { 1.0 } else { 0.0 }], 0)
    }

    /// CSS `mask-image: url()`: the element multiplied by a renderer texture's alpha (or luminance), stretched across its box.
    pub fn mask_image(texture: u64, use_luminance: bool, invert: bool) -> Result<Self, String> {
        Self::shaped(19, &[if use_luminance { 1.0 } else { 0.0 }, if invert { 1.0 } else { 0.0 }], texture)
    }

    /// Builds an effect out of your own passes, run in order; the last pass's output is composited.
    pub fn custom(name: &str, options: EffectOptions, passes: &[EffectPass]) -> Result<Self, String> {
        let id = outcome(ffi::ui_effect_create_custom(name, &options.to_ffi()))?;
        let effect = Self(Arc::new(EffectHandle(id)));
        for pass in passes {
            outcome(ffi::ui_effect_add_pass(id, &pass.to_ffi()))?;
        }
        Ok(effect)
    }
}

/// The profile of one corner, after CSS `corner-shape`: a superellipse `|x|^n + |y|^n = 1` with `n = 2^k`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CornerShape(pub f32);

impl CornerShape {
    /// The ordinary circular arc.
    pub const ROUND: Self = Self(1.0);
    pub const SQUIRCLE: Self = Self(2.0);
    /// A straight diagonal.
    pub const BEVEL: Self = Self(0.0);
    /// Curves into the box.
    pub const SCOOP: Self = Self(-1.0);
    /// A sharp corner (the radius has no effect).
    pub const SQUARE: Self = Self(f32::INFINITY);
    pub const NOTCH: Self = Self(f32::NEG_INFINITY);

    /// Any CSS `superellipse(k)`.
    pub const fn superellipse(k: f32) -> Self {
        Self(k)
    }
}

impl Default for CornerShape {
    fn default() -> Self {
        Self::ROUND
    }
}

/// One [`CornerShape`] per corner, in the order of `CornerRadius`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct CornerShapes {
    pub top_left: CornerShape,
    pub top_right: CornerShape,
    pub bottom_left: CornerShape,
    pub bottom_right: CornerShape,
}

impl CornerShapes {
    pub fn all(shape: CornerShape) -> Self {
        Self { top_left: shape, top_right: shape, bottom_left: shape, bottom_right: shape }
    }

    pub(crate) fn to_array(self) -> [f32; 4] {
        [self.top_left.0, self.top_right.0, self.bottom_left.0, self.bottom_right.0]
    }
}

/// How one border edge is drawn (CSS `border-style`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum BorderLineStyle {
    /// A continuous line that follows rounded and shaped corners.
    #[default]
    Solid = 0,
    /// Dashes three widths long with gaps two widths long (straight between the corners).
    Dashed = 1,
    /// Round dots one width across, two widths apart (straight between the corners).
    Dotted = 2,
    /// Two lines a third of the width each with a third between.
    Double = 3,
}

/// One border edge: colour (straight-alpha sRGB) and style.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct BorderEdge {
    pub color: glam::Vec4,
    pub style: BorderLineStyle,
}

impl BorderEdge {
    pub fn new(color: glam::Vec4, style: BorderLineStyle) -> Self {
        Self { color, style }
    }
}

/// A border whose edges differ. Set it on [`ElementDesc::border_edges`](crate::ui::ElementDesc::border_edges); the widths come from the
/// element's `border`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct BorderEdges {
    pub left: BorderEdge,
    pub right: BorderEdge,
    pub top: BorderEdge,
    pub bottom: BorderEdge,
}

impl BorderEdges {
    pub fn all(edge: BorderEdge) -> Self {
        Self { left: edge, right: edge, top: edge, bottom: edge }
    }

    pub(crate) fn colors(&self) -> [f32; 16] {
        let mut out = [0.0; 16];
        for (i, edge) in [self.left, self.right, self.top, self.bottom].iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&edge.color.to_array());
        }
        out
    }

    pub(crate) fn styles(&self) -> [u32; 4] {
        [self.left.style as u32, self.right.style as u32, self.top.style as u32, self.bottom.style as u32]
    }
}

impl ElementDesc {
    /// Per-edge border colours and styles.
    #[must_use]
    pub fn border_edges(mut self, edges: Option<BorderEdges>) -> Self {
        self.border_edges = edges;
        self
    }

    /// Draws this element and everything inside it through `effect`.
    #[must_use]
    pub fn effect(mut self, effect: Option<UiEffect>) -> Self {
        self.effect = effect;
        self
    }

    /// The profile of each corner.
    #[must_use]
    pub fn corner_shape(mut self, shapes: CornerShapes) -> Self {
        self.corner_shape = shapes;
        self
    }
}
