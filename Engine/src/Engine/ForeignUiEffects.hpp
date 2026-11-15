#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <Engine/ForeignUi.hpp>
#include <Renderer/UI/UI.hpp>

namespace SFT::Engine {

    /// UI effects (shader effects on whole widgets: blur, shadows, gradients, frosted glass, layers, and custom multi-pass shaders) for code
    /// that cannot build `UI::UiEffect` values itself. An effect is made once, gets an id, and is named by that id in
    /// `ForeignUiElement::effect`; it lives until released. An effect must be finished (all its passes added, options set) before the first
    /// frame that uses it; after that it is read-only.

    /// 0 is "no effect".
    using ForeignUiEffectId = u64;

    /// Values match the C ABI.
    enum class ForeignUiEffectKind : u32 {
        /// values: brightness, contrast, saturation, grayscale, sepia, invert, hue (radians), opacity. Defaults change nothing.
        ColorAdjust = 0,
        /// color: multiplies the element's colour and alpha.
        Tint = 1,
        /// values[0]: sigma in pixels.
        Blur = 2,
        /// values[0]: sigma in pixels; a cheap wide blur through a mip chain.
        FastBlur = 3,
        /// values: offset x, offset y, blur sigma, strength; color.
        DropShadow = 4,
        /// values: sigma, strength; color.
        Glow = 5,
        /// values: width, offset; color.
        Outline = 6,
        /// values: offset x, offset y, blur sigma, strength; color.
        InsetShadow = 7,
        /// values: blur radius in pixels, tint strength, saturation; color: the tint (rgb). Reads the backdrop, so it belongs in a layer.
        FrostedGlass = 8,
        /// Only a layer marker (`options.layer`).
        Layer = 9,
        /// values[0]: angle in radians (0 = left to right, growing clockwise); stops.
        LinearGradient = 10,
        /// values: center x, center y (0..1), radius (fraction of the longer side); stops.
        RadialGradient = 11,
        /// values: center x, center y (0..1), start angle in radians; stops.
        ConicGradient = 12,
        /// values: translate x, translate y (pixels), angle (radians, clockwise), scale x, scale y, origin x, origin y (0..1 across the box).
        /// Draws anywhere on the surface unless `options.outset` gives the room it needs.
        Transform = 13,
        /// values: top, right, bottom, left (pixels), corner radius (pixels).
        ClipInset = 14,
        /// values: radius (pixels), centre x, centre y (0..1 across the box).
        ClipCircle = 15,
        /// values: radius x, radius y (pixels), centre x, centre y (0..1).
        ClipEllipse = 16,
        /// values: up to six points as x, y pairs (0..1 across the box); `value_count` says how many numbers are set (6..12).
        ClipPolygon = 17,
        /// values: angle (radians), alpha at start, alpha at end, start, end (0..1 along the line), invert (0 or 1).
        MaskLinear = 18,
        /// values: use luminance (0/1), invert (0/1); `texture`: a renderer texture id.
        MaskImage = 19,
    };

    /// `space` and `hue_method` carry `UI::ColorSpace` and `UI::HueInterpolation` values.
    struct ForeignUiGradientStop {
        f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        f32 position = 0.0f;
        u32 space = 0;
        /// When set, the stretch from this stop to the next mixes in `toward_space` / `toward_hue_method` instead of the gradient's own.
        bool has_toward_next = false;
        u32 toward_space = 0;
        u32 toward_hue_method = 0;
    };

    struct ForeignUiEffectSpec {
        ForeignUiEffectKind kind = ForeignUiEffectKind::Tint;
        f32 values[16] = {};
        /// How many of `values` are meaningful (for the polygon).
        u32 value_count = 8;
        /// A renderer texture id, for `MaskImage`.
        u64 texture = 0;
        /// Straight alpha, components in `color_space` (a `UI::ColorSpace` value; 0 = sRGB).
        f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        u32 color_space = 0;
        /// How a gradient mixes its colours: a `UI::ColorSpace` (10 = Oklab, the default) and a `UI::HueInterpolation`.
        u32 interpolation_space = 10;
        u32 interpolation_hue = 0;
        std::vector<ForeignUiGradientStop> stops;
    };

    /// Applied on top of any effect, library or custom. Values match `UI::UiEffectBlend` for `composite`: 0 normal, 1 additive,
    /// 2 multiply, 3 replace.
    struct ForeignUiEffectOptions {
        u32 composite = 0;
        f32 opacity = 1.0f;
        /// Left, right, top, bottom, in pixels.
        f32 outset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool clip_to_bounds = true;
        /// 0 draws in place; higher hoists the element into a later overlay pass (see `UI::UiEffect::layer`).
        u32 layer = 0;
        /// Mips of the group's content for `GroupContent` inputs read at several scales: 1 none, 0 the whole chain.
        u32 content_mip_levels = 1;
    };

    struct ForeignUiEffectInput {
        /// 0 group content, 1 an earlier pass's output (`pass`), 2 an application texture (`texture`), 3 the backdrop.
        u32 kind = 0;
        u32 pass = 0;
        u64 texture = 0;
        u32 base_mip = 0;
        u32 mip_count = 0;
    };

    /// One pass of a custom effect: a fragment shader over a full-target triangle (the contract is in `Renderer/UI/UiEffect.hpp`).
    struct ForeignUiEffectPass {
        std::string label;
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
        std::vector<ForeignUiEffectInput> inputs;
        /// Sampling for every input.
        bool linear_filter = true;
        bool linear_mips = true;
        /// With `separate_mag_filter`, `linear_filter` is the minification filter and this the magnification filter (nearest up, linear down,
        /// say); without it `linear_filter` is both.
        bool separate_mag_filter = false;
        bool linear_mag_filter = true;
        /// 0 clamp, 1 repeat, 2 mirror, 3 border: the horizontal mode, and the vertical one too unless `separate_address_v` is set.
        u32 address = 0;
        bool separate_address_v = false;
        u32 address_v = 0;
        f32 lod_bias = 0.0f;
        f32 min_lod = 0.0f;
        f32 max_lod = 1000.0f;
        f32 max_anisotropy = 1.0f;
        /// An `RHI::Format` value; 0 selects the default (half-float RGBA).
        u32 output_format = 0;
        f32 output_scale = 1.0f;
        u32 output_mip_levels = 1;
        /// 0 box, 1 tent.
        u32 output_mip_filter = 1;
        /// Pushed after the engine's 48-byte prefix; a multiple of 4 bytes, at most 80.
        std::vector<std::byte> parameters;
    };

    /// Builds a library effect. Fails for an unknown kind or space, or a gradient with no stops.
    [[nodiscard]] ForeignUiResult<ForeignUiEffectId> create_foreign_ui_effect(const ForeignUiEffectSpec &spec,
                                                                              const ForeignUiEffectOptions &options = {});
    /// Starts an effect with no passes, for `add_foreign_ui_effect_pass`.
    [[nodiscard]] ForeignUiResult<ForeignUiEffectId> create_custom_foreign_ui_effect(std::string_view name, const ForeignUiEffectOptions &options = {});
    [[nodiscard]] ForeignUiResult<void> add_foreign_ui_effect_pass(ForeignUiEffectId effect, const ForeignUiEffectPass &pass);
    [[nodiscard]] ForeignUiResult<void> set_foreign_ui_effect_options(ForeignUiEffectId effect, const ForeignUiEffectOptions &options);
    /// The id stays invalid afterwards; elements of frames already built keep drawing with the effect until those frames are gone.
    [[nodiscard]] ForeignUiResult<void> release_foreign_ui_effect(ForeignUiEffectId effect);
    /// The effect behind an id, or null.
    [[nodiscard]] std::shared_ptr<const UI::UiEffect> find_foreign_ui_effect(ForeignUiEffectId effect);

} // namespace SFT::Engine
