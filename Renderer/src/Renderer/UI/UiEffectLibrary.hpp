#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/UiColor.hpp>
#include <Renderer/UI/UiEffect.hpp>

// Ready-made effects for the things UI authors reach for constantly: filters (blur, brightness, contrast, saturation, grayscale, sepia,
// hue rotation, invert, opacity), drop shadows and glows, and gradient fills. Each builds a `UiEffect` out of the engine's stock shaders
// (Shaders/ui_effect_*.slang), so they are also worked examples of the effect shader contract in UI/UiEffect.hpp. Build one once and
// assign it to any number of `ElementDecl::effect`s:
//
//     const auto shadow = UI::Effects::drop_shadow({4.0f, 6.0f}, 8.0f, glm::vec4{0.0f, 0.0f, 0.0f, 0.5f});
//     ctx.element(UI::ElementDecl{ ..., .effect = shadow });
//
// Colours are the vector types shaders use. A `glm::vec4` is straight-alpha RGBA, a `glm::vec3` the same with alpha 1, and unless a
// `ColorSpace` argument says otherwise the components are sRGB-encoded like a CSS hex colour; pass `ColorSpace::Oklch` (or any other
// CSS space, UI/UiColor.hpp) to write the colour in that space instead. `UI::Color` (an `Srgb`) works wherever a vector does.
//
// Colours are always blended in Oklab unless told otherwise. Gradients take a `ColorInterpolation` (the space, and for a polar space the
// hue method) for the whole gradient, and every stop can override how the gradient travels from it to the next stop.
//
// A gradient (or any effect that recolours) uses the element's own drawing as its shape: give the element an opaque background and,
// if wanted, corner radii and shapes, and the gradient fills exactly that.
namespace SFT::UI::Effects {

    namespace detail {

        template <typename T>
        void append_bytes(std::vector<std::byte> &out, const T &value) {
            const usize at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }

        /// What the effect shaders take: straight-alpha colour in linear sRGB (out-of-gamut components kept; the render target clips).
        [[nodiscard]] inline glm::vec4 linear_rgba(glm::vec4 color, ColorSpace space) noexcept {
            return convert(color, space, ColorSpace::SrgbLinear);
        }

        /// A blur pass along one axis (see Shaders/ui_effect_blur.slang): `sigma` and `stride` are in the *source's* texels.
        [[nodiscard]] inline UiEffectPass blur_pass(bool horizontal, f32 sigma, f32 stride, f32 output_scale, UiEffectInput input) {
            UiEffectPass pass;
            pass.label = UString{horizontal ? "blur horizontal" : "blur vertical"};
            pass.shader_path = UString{"Shaders/ui_effect_blur.slang"};
            pass.module_name = UString{"ui_effect_blur"};
            pass.inputs = {input};
            pass.output.scale = output_scale;
            const glm::vec4 parameters{horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f, sigma, stride};
            append_bytes(pass.parameters, parameters);
            return pass;
        }

        /// The two passes of a Gaussian blur of `sigma_px` surface pixels, appended to `effect`; the second pass's index is returned.
        inline usize append_blur(UiEffect &effect, f32 sigma_px) {
            const f32 scale = sigma_px >= 2.0f ? 0.5f : 1.0f;
            const f32 stride0 = std::max(1.0f, sigma_px / 2.5f);
            const f32 sigma1 = sigma_px * scale;
            const f32 stride1 = std::max(1.0f, sigma1 / 2.5f);
            const usize first = effect.passes.size();
            effect.passes.push_back(blur_pass(true, sigma_px, stride0, scale, UiEffectInput{.kind = UiEffectInputKind::GroupContent}));
            effect.passes.push_back(blur_pass(false, sigma1, stride1, scale,
                                              UiEffectInput{.kind = UiEffectInputKind::PassOutput, .pass = static_cast<u32>(first)}));
            return first + 1;
        }

        [[nodiscard]] inline std::shared_ptr<UiEffect> shadow_like(const char *name, glm::vec2 offset, f32 blur_sigma, glm::vec4 color,
                                                                   ColorSpace space, f32 strength) {
            auto effect = std::make_shared<UiEffect>();
            effect->name = UString{name};
            const f32 sigma = std::max(blur_sigma, 0.0f);
            const usize blurred = append_blur(*effect, std::max(sigma, 0.5f));
            UiEffectPass compose;
            compose.label = UString{"compose"};
            compose.shader_path = UString{"Shaders/ui_effect_shadow.slang"};
            compose.module_name = UString{"ui_effect_shadow"};
            compose.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent},
                              UiEffectInput{.kind = UiEffectInputKind::PassOutput, .pass = static_cast<u32>(blurred)}};
            append_bytes(compose.parameters, linear_rgba(color, space));
            append_bytes(compose.parameters, glm::vec4{offset.x, offset.y, strength, 0.0f});
            effect->passes.push_back(std::move(compose));
            effect->outset = UiEffectOutset::all(std::ceil(3.0f * sigma + std::max(std::abs(offset.x), std::abs(offset.y)) + 2.0f));
            return effect;
        }

    } // namespace detail

    /// Filters that change an element's colour as a whole. The defaults change nothing; set only what you want.
    struct ColorAdjust {
        f32 brightness = 1.0f; ///< multiplies the colour; 0 is black, 1 unchanged
        f32 contrast = 1.0f;   ///< stretches around mid grey; 0 is flat grey, 1 unchanged
        f32 saturation = 1.0f; ///< 0 is grey, 1 unchanged, above 1 more vivid
        f32 grayscale = 0.0f;  ///< 0..1 blend towards greyscale
        f32 sepia = 0.0f;      ///< 0..1 blend towards sepia
        f32 invert = 0.0f;     ///< 0..1 blend towards the inverted colour
        f32 hue_radians = 0.0f;
        f32 opacity = 1.0f; ///< multiplies the whole element, alpha included
    };

    [[nodiscard]] inline std::shared_ptr<UiEffect> color_adjust(const ColorAdjust &adjust) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"color adjust"};
        UiEffectPass pass;
        pass.label = UString{"color adjust"};
        pass.shader_path = UString{"Shaders/ui_effect_color.slang"};
        pass.module_name = UString{"ui_effect_color"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, glm::vec4{adjust.brightness, adjust.contrast, adjust.saturation, adjust.grayscale});
        detail::append_bytes(pass.parameters, glm::vec4{adjust.sepia, adjust.invert, adjust.hue_radians, adjust.opacity});
        effect->passes.push_back(std::move(pass));
        return effect;
    }

    /// Multiplies colour and alpha: a tint, and (with alpha below 1) a fade.
    [[nodiscard]] inline std::shared_ptr<UiEffect> tint(glm::vec4 color, ColorSpace space = ColorSpace::Srgb) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"tint"};
        UiEffectPass pass;
        pass.label = UString{"tint"};
        pass.shader_path = UString{"Shaders/ui_effect_tint.slang"};
        pass.module_name = UString{"ui_effect_tint"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, detail::linear_rgba(color, space));
        detail::append_bytes(pass.parameters, glm::vec4{0.0f});
        effect->passes.push_back(std::move(pass));
        return effect;
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> tint(glm::vec3 color, ColorSpace space = ColorSpace::Srgb) {
        return tint(glm::vec4{color, 1.0f}, space);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> tint(const Color &color) { return tint(color.vec4(), ColorSpace::Srgb); }

    /// A Gaussian blur of `sigma_px` surface pixels over the element and everything in it (CSS `filter: blur()`), spreading outside the
    /// element by about three sigma.
    [[nodiscard]] inline std::shared_ptr<UiEffect> blur(f32 sigma_px) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"blur"};
        const f32 sigma = std::max(sigma_px, 0.5f);
        detail::append_blur(*effect, sigma);
        effect->outset = UiEffectOutset::all(std::ceil(3.0f * sigma + 2.0f));
        return effect;
    }

    /// Puts an element and everything inside it in a later layer: drawn in its own overlay pass above the lower layers (see
    /// `UiSurface::finish_overlays`), where an effect (`frosted_glass`, any pass that reads `Backdrop`) sees what they drew. Combine with
    /// an effect by setting `UiEffect::layer` on it instead. A layer has no cost of its own beyond the extra pass.
    [[nodiscard]] inline std::shared_ptr<UiEffect> layer(u8 index = 1) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"layer"};
        effect->layer = index;
        return effect;
    }

    /// A 2D transform (CSS `transform` + `transform-origin`): the element and everything in it is drawn scaled, then rotated, then moved
    /// by `translate` pixels, about `origin` (0..1 across the element's box; the centre by default). The layout is unchanged and so is
    /// hit testing: this is a visual transform. `angle_radians` grows clockwise (y points down). Anything the transform moves outside the
    /// element's box needs room to be drawn in: give `max_box_size` (the largest box the effect will be used on, in pixels) to size that
    /// room exactly, or leave it zero to let the effect draw anywhere on the surface (simpler, uses a surface-sized image).
    [[nodiscard]] inline std::shared_ptr<UiEffect> transform(glm::vec2 translate, f32 angle_radians = 0.0f, glm::vec2 scale = glm::vec2{1.0f},
                                                               glm::vec2 origin = glm::vec2{0.5f}, glm::vec2 max_box_size = glm::vec2{0.0f}) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"transform"};
        effect->transform = UiTransform2D{.translate = translate, .angle_radians = angle_radians, .scale = scale, .origin = origin};
        const f32 c = std::cos(angle_radians);
        const f32 s = std::sin(angle_radians);
        // Forward linear part L = R * S (scale first, then rotate); the shader needs its inverse.
        const f32 l00 = c * scale.x, l01 = -s * scale.y, l10 = s * scale.x, l11 = c * scale.y;
        const f32 det = l00 * l11 - l01 * l10;
        const f32 safe = std::abs(det) < 1e-12f ? 1e-12f : det;
        const glm::vec4 inverse{l11 / safe, -l01 / safe, -l10 / safe, l00 / safe};
        UiEffectPass pass;
        pass.label = UString{"transform"};
        pass.shader_path = UString{"Shaders/ui_effect_transform.slang"};
        pass.module_name = UString{"ui_effect_transform"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, inverse);
        detail::append_bytes(pass.parameters, glm::vec4{translate.x, translate.y, origin.x, origin.y});
        effect->passes.push_back(std::move(pass));
        if (max_box_size.x > 0.0f && max_box_size.y > 0.0f) {
            // Where the box's corners end up, relative to the box: the bounding box of that is the room the result needs.
            const glm::vec2 corners[4] = {{0.0f, 0.0f}, {max_box_size.x, 0.0f}, {0.0f, max_box_size.y}, max_box_size};
            const glm::vec2 pivot = max_box_size * origin;
            glm::vec2 low{0.0f};
            glm::vec2 high = max_box_size;
            for (const glm::vec2 &corner : corners) {
                const glm::vec2 d = corner - pivot;
                const glm::vec2 moved = pivot + glm::vec2{l00 * d.x + l01 * d.y, l10 * d.x + l11 * d.y} + translate;
                low = glm::min(low, moved);
                high = glm::max(high, moved);
            }
            effect->outset = UiEffectOutset{std::ceil(-low.x) + 1.0f, std::ceil(high.x - max_box_size.x) + 1.0f, std::ceil(-low.y) + 1.0f,
                                            std::ceil(high.y - max_box_size.y) + 1.0f};
        } else {
            effect->clip_to_bounds = false;
        }
        return effect;
    }

    namespace detail {
        [[nodiscard]] inline std::shared_ptr<UiEffect> clip_effect(const char *name, f32 mode, f32 points, f32 radius, glm::vec4 a,
                                                                   const glm::vec4 (&rows)[3]) {
            auto effect = std::make_shared<UiEffect>();
            effect->name = UString{name};
            UiEffectPass pass;
            pass.label = UString{name};
            pass.shader_path = UString{"Shaders/ui_effect_clip_path.slang"};
            pass.module_name = UString{"ui_effect_clip_path"};
            pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
            append_bytes(pass.parameters, glm::vec4{mode, points, radius, 0.0f});
            append_bytes(pass.parameters, a);
            for (const glm::vec4 &row : rows) {
                append_bytes(pass.parameters, row);
            }
            effect->passes.push_back(std::move(pass));
            return effect;
        }
    } // namespace detail

    /// CSS `clip-path: inset(...)`: cuts the element to its box shrunk by the given pixels on each side, with rounded corners of `radius`.
    [[nodiscard]] inline std::shared_ptr<UiEffect> clip_inset(f32 top, f32 right, f32 bottom, f32 left, f32 radius = 0.0f) {
        const glm::vec4 none[3]{};
        auto effect = detail::clip_effect("clip inset", 0.0f, 0.0f, radius, glm::vec4{top, right, bottom, left}, none);
        effect->clip_shape = UiClipShape{.kind = UiClipShape::Kind::Inset, .inset = {top, right, bottom, left}, .radius = radius};
        return effect;
    }

    /// CSS `clip-path: circle(...)`: a circle of `radius_px` centred at (`center.x`, `center.y`) across the box (0..1; 0.5, 0.5 is the middle).
    [[nodiscard]] inline std::shared_ptr<UiEffect> clip_circle(f32 radius_px, glm::vec2 center = glm::vec2{0.5f}) {
        const glm::vec4 none[3]{};
        auto effect = detail::clip_effect("clip circle", 1.0f, 0.0f, 0.0f, glm::vec4{center.x, center.y, radius_px, 0.0f}, none);
        effect->clip_shape = UiClipShape{.kind = UiClipShape::Kind::Circle, .radius = radius_px, .center = center};
        return effect;
    }

    /// CSS `clip-path: ellipse(...)`: radii in pixels, centre across the box (0..1).
    [[nodiscard]] inline std::shared_ptr<UiEffect> clip_ellipse(glm::vec2 radii_px, glm::vec2 center = glm::vec2{0.5f}) {
        const glm::vec4 none[3]{};
        auto effect = detail::clip_effect("clip ellipse", 2.0f, 0.0f, 0.0f, glm::vec4{center.x, center.y, radii_px.x, radii_px.y}, none);
        effect->clip_shape = UiClipShape{.kind = UiClipShape::Kind::Ellipse, .center = center, .radii = radii_px};
        return effect;
    }

    /// CSS `clip-path: polygon(...)`: three to six points across the box (0..1 on each axis, y down). More points are ignored.
    [[nodiscard]] inline std::shared_ptr<UiEffect> clip_polygon(std::span<const glm::vec2> points) {
        glm::vec4 rows[3]{};
        const usize count = std::min<usize>(points.size(), 6);
        for (usize i = 0; i < count; ++i) {
            glm::vec4 &row = rows[i / 2];
            (i % 2 == 0 ? row.x : row.z) = points[i].x;
            (i % 2 == 0 ? row.y : row.w) = points[i].y;
        }
        auto effect = detail::clip_effect("clip polygon", 3.0f, static_cast<f32>(std::max<usize>(count, 3)), 0.0f, glm::vec4{0.0f}, rows);
        UiClipShape shape{.kind = UiClipShape::Kind::Polygon};
        for (usize i = 0; i < count; ++i) {
            shape.points[i] = points[i];
        }
        shape.point_count = static_cast<u32>(count);
        effect->clip_shape = shape;
        return effect;
    }

    /// Clips to the element's own box with corner radii (surface pixels: top left, top right, bottom left, bottom right) and corner profiles
    /// (`pack_corner_shapes`): what `ClipConfig::follow_corners` builds.
    [[nodiscard]] inline std::shared_ptr<UiEffect> clip_box(glm::vec4 radii_px, u32 shape_codes) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"clip box"};
        UiEffectPass pass;
        pass.label = UString{"clip box"};
        pass.shader_path = UString{"Shaders/ui_effect_clip_box.slang"};
        pass.module_name = UString{"ui_effect_clip_box"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, radii_px);
        detail::append_bytes(pass.parameters, glm::vec4{static_cast<f32>(shape_codes & 0xFFu), static_cast<f32>((shape_codes >> 8u) & 0xFFu),
                                                          static_cast<f32>((shape_codes >> 16u) & 0xFFu), static_cast<f32>((shape_codes >> 24u) & 0xFFu)});
        effect->passes.push_back(std::move(pass));
        effect->clip_shape = UiClipShape{.kind = UiClipShape::Kind::RoundedBox, .corner_radii = radii_px, .corner_codes = shape_codes};
        return effect;
    }

    /// `base` (may be null) followed by the passes of `clip`: the clip's passes read what the base's last pass produced (or the element's content
    /// when there is no base). The result keeps the base's composite, opacity, outset, layer and transform, and the clip's shape for hit testing.
    [[nodiscard]] inline std::shared_ptr<UiEffect> then_clip(const std::shared_ptr<const UiEffect> &base, const UiEffect &clip) {
        auto out = base ? std::make_shared<UiEffect>(*base) : std::make_shared<UiEffect>();
        if (!base) {
            out->name = clip.name;
        }
        const usize first = out->passes.size();
        for (usize i = 0; i < clip.passes.size(); ++i) {
            UiEffectPass pass = clip.passes[i];
            if (first > 0 && i == 0 && !pass.inputs.empty() && pass.inputs.front().kind == UiEffectInputKind::GroupContent) {
                pass.inputs.front() = UiEffectInput{.kind = UiEffectInputKind::PassOutput, .pass = static_cast<u32>(first - 1)};
            } else {
                for (UiEffectInput &input : pass.inputs) {
                    if (input.kind == UiEffectInputKind::PassOutput) {
                        input.pass += static_cast<u32>(first);
                    }
                }
            }
            out->passes.push_back(std::move(pass));
        }
        if (clip.clip_shape) {
            out->clip_shape = clip.clip_shape;
        }
        return out;
    }

    namespace detail {
        [[nodiscard]] inline std::shared_ptr<UiEffect> mask_effect(const char *name, f32 mode, bool invert, bool luminance, glm::vec4 ramp,
                                                                   glm::vec2 place, std::optional<Renderer::TextureHandle> texture) {
            auto effect = std::make_shared<UiEffect>();
            effect->name = UString{name};
            UiEffectPass pass;
            pass.label = UString{name};
            pass.shader_path = UString{"Shaders/ui_effect_mask.slang"};
            pass.module_name = UString{"ui_effect_mask"};
            pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
            if (texture) {
                pass.inputs.push_back(UiEffectInput{.kind = UiEffectInputKind::Texture, .texture = *texture});
            } else {
                // A pass reads the same number of inputs whatever the mode, so the shader's second texture is always bound.
                pass.inputs.push_back(UiEffectInput{.kind = UiEffectInputKind::GroupContent});
            }
            append_bytes(pass.parameters, glm::vec4{mode, invert ? 1.0f : 0.0f, luminance ? 1.0f : 0.0f, 0.0f});
            append_bytes(pass.parameters, ramp);
            append_bytes(pass.parameters, glm::vec4{place.x, place.y, 0.0f, 0.0f});
            effect->passes.push_back(std::move(pass));
            return effect;
        }
    } // namespace detail

    /// CSS `mask-image: linear-gradient(...)` in alpha only: the element is multiplied by an alpha that runs from `alpha_start` at
    /// `start` to `alpha_end` at `end` (0..1 along the gradient line) and holds beyond them. `angle_radians` 0 runs left to right and grows
    /// clockwise. Fading an element out toward an edge is (angle 0, 1, 0, 0.6, 1).
    [[nodiscard]] inline std::shared_ptr<UiEffect> mask_linear(f32 angle_radians, f32 alpha_start, f32 alpha_end, f32 start = 0.0f, f32 end = 1.0f,
                                                               bool invert = false) {
        auto effect = detail::mask_effect("mask linear", 0.0f, invert, false, glm::vec4{angle_radians, alpha_start, alpha_end, 0.0f}, {start, end}, std::nullopt);
        effect->mask = UiMaskShape{.angle_radians = angle_radians, .alpha_start = alpha_start, .alpha_end = alpha_end, .start = start, .end = end, .invert = invert};
        return effect;
    }

    /// CSS `mask-image: url(...)`: the element is multiplied by the image's alpha (or its luminance), the image stretched across the
    /// element's box. `invert` keeps what the mask hides.
    [[nodiscard]] inline std::shared_ptr<UiEffect> mask_image(Renderer::TextureHandle texture, bool use_luminance = false, bool invert = false) {
        auto effect = detail::mask_effect("mask image", 1.0f, invert, use_luminance, glm::vec4{0.0f}, {0.0f, 1.0f}, texture);
        effect->mask = UiMaskShape{.image = true, .invert = invert, .texture = texture, .luminance = use_luminance};
        return effect;
    }

    /// Frosted glass (CSS `backdrop-filter: blur()`): fills the element's shape with a blurred copy of what is behind it, mixed toward
    /// `tint_color` (linear-light sRGB components) by `tint_strength`, and draws the element's own content on top. `saturation` scales
    /// the backdrop's colour (1 = unchanged). Needs a UI that has a backdrop: a top-level element of an overlay drawn over a scene or
    /// earlier overlay.
    [[nodiscard]] inline std::shared_ptr<UiEffect> frosted_glass(f32 radius_px, glm::vec3 tint_color = glm::vec3{1.0f},
                                                                   f32 tint_strength = 0.15f, f32 saturation = 1.0f) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"frosted glass"};
        UiEffectPass pass;
        pass.label = UString{"frosted glass"};
        pass.shader_path = UString{"Shaders/ui_effect_frosted.slang"};
        pass.module_name = UString{"ui_effect_frosted"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::Backdrop}, UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, glm::vec4{tint_color, tint_strength});
        detail::append_bytes(pass.parameters, glm::vec4{radius_px, saturation, 0.0f, 0.0f});
        effect->passes.push_back(std::move(pass));
        return effect;
    }

    /// A wide blur at a fixed, small cost: the element's content gets a mip chain and one pass reads it at the level whose texels are about
    /// as wide as `sigma_px` calls for. Use this for large radii (a frosted-glass panel, a big glow source); `blur` is better for small
    /// radii, where this one is blockier. Spreads outside the element by about three sigma.
    [[nodiscard]] inline std::shared_ptr<UiEffect> fast_blur(f32 sigma_px) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"fast blur"};
        const f32 sigma = std::max(sigma_px, 1.0f);
        effect->content.mip_levels = 0; // the whole chain: any radius the surface can hold
        UiEffectPass pass;
        pass.label = UString{"mip blur"};
        pass.shader_path = UString{"Shaders/ui_effect_mipblur.slang"};
        pass.module_name = UString{"ui_effect_mipblur"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        // A box of width w has sigma w / sqrt(12), and level L's texels are 2^L pixels wide.
        const f32 level = std::max(0.0f, std::log2(sigma * 3.4641016f));
        detail::append_bytes(pass.parameters, glm::vec4{level, 0.5f, 0.0f, 0.0f});
        effect->passes.push_back(std::move(pass));
        effect->outset = UiEffectOutset::all(std::ceil(3.0f * sigma + 2.0f));
        return effect;
    }

    /// A soft shadow shaped like the element (CSS `box-shadow` / `drop-shadow()`): the element's silhouette blurred by `blur_sigma`
    /// pixels, coloured `color`, moved by `offset` pixels, and drawn under the element. `strength` scales how dense it is (1 as is).
    [[nodiscard]] inline std::shared_ptr<UiEffect> drop_shadow(glm::vec2 offset, f32 blur_sigma, glm::vec4 color, f32 strength = 1.0f,
                                                               ColorSpace space = ColorSpace::Srgb) {
        return detail::shadow_like("drop shadow", offset, blur_sigma, color, space, strength);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> drop_shadow(glm::vec2 offset, f32 blur_sigma, glm::vec3 color, f32 strength = 1.0f,
                                                               ColorSpace space = ColorSpace::Srgb) {
        return drop_shadow(offset, blur_sigma, glm::vec4{color, 1.0f}, strength, space);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> drop_shadow(glm::vec2 offset, f32 blur_sigma, const Color &color, f32 strength = 1.0f) {
        return drop_shadow(offset, blur_sigma, color.vec4(), strength, ColorSpace::Srgb);
    }

    /// A halo of `color` around the element's shape, `radius_sigma` pixels soft. A shadow with no offset and a dense core.
    [[nodiscard]] inline std::shared_ptr<UiEffect> glow(glm::vec4 color, f32 radius_sigma, f32 strength = 2.0f, ColorSpace space = ColorSpace::Srgb) {
        return detail::shadow_like("glow", glm::vec2{0.0f}, radius_sigma, color, space, strength);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> glow(glm::vec3 color, f32 radius_sigma, f32 strength = 2.0f, ColorSpace space = ColorSpace::Srgb) {
        return glow(glm::vec4{color, 1.0f}, radius_sigma, strength, space);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> glow(const Color &color, f32 radius_sigma, f32 strength = 2.0f) {
        return glow(color.vec4(), radius_sigma, strength, ColorSpace::Srgb);
    }

    /// A ring around the element's shape, `width` pixels wide and starting `offset` pixels out from its edge (CSS `outline` with
    /// `outline-offset`). Follows the shape: corner radii and shapes, an image's alpha, text.
    [[nodiscard]] inline std::shared_ptr<UiEffect> outline(f32 width, glm::vec4 color, f32 offset = 0.0f, ColorSpace space = ColorSpace::Srgb) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"outline"};
        UiEffectPass pass;
        pass.label = UString{"outline"};
        pass.shader_path = UString{"Shaders/ui_effect_outline.slang"};
        pass.module_name = UString{"ui_effect_outline"};
        pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
        detail::append_bytes(pass.parameters, detail::linear_rgba(color, space));
        detail::append_bytes(pass.parameters, glm::vec4{std::max(width, 0.0f), std::max(offset, 0.0f), 0.0f, 0.0f});
        effect->passes.push_back(std::move(pass));
        effect->outset = UiEffectOutset::all(std::ceil(std::max(width, 0.0f) + std::max(offset, 0.0f) + 2.0f));
        return effect;
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> outline(f32 width, glm::vec3 color, f32 offset = 0.0f, ColorSpace space = ColorSpace::Srgb) {
        return outline(width, glm::vec4{color, 1.0f}, offset, space);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> outline(f32 width, const Color &color, f32 offset = 0.0f) {
        return outline(width, color.vec4(), offset, ColorSpace::Srgb);
    }

    /// A shadow inside the element's shape, darkest along its edges (CSS `box-shadow: inset`), `blur_sigma` pixels soft and moved by
    /// `offset`. `strength` scales how dense it is.
    [[nodiscard]] inline std::shared_ptr<UiEffect> inset_shadow(glm::vec2 offset, f32 blur_sigma, glm::vec4 color, f32 strength = 1.0f,
                                                                ColorSpace space = ColorSpace::Srgb) {
        auto effect = std::make_shared<UiEffect>();
        effect->name = UString{"inset shadow"};
        const usize blurred = detail::append_blur(*effect, std::max(blur_sigma, 0.5f));
        UiEffectPass compose;
        compose.label = UString{"compose"};
        compose.shader_path = UString{"Shaders/ui_effect_inset_shadow.slang"};
        compose.module_name = UString{"ui_effect_inset_shadow"};
        compose.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent},
                          UiEffectInput{.kind = UiEffectInputKind::PassOutput, .pass = static_cast<u32>(blurred)}};
        detail::append_bytes(compose.parameters, detail::linear_rgba(color, space));
        detail::append_bytes(compose.parameters, glm::vec4{offset.x, offset.y, strength, 0.0f});
        effect->passes.push_back(std::move(compose));
        // The blur must see the transparent surroundings or the shadow would not darken the edges: a little room beyond the box.
        effect->outset = UiEffectOutset::all(std::ceil(3.0f * std::max(blur_sigma, 0.5f) + 2.0f));
        return effect;
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> inset_shadow(glm::vec2 offset, f32 blur_sigma, glm::vec3 color, f32 strength = 1.0f,
                                                                ColorSpace space = ColorSpace::Srgb) {
        return inset_shadow(offset, blur_sigma, glm::vec4{color, 1.0f}, strength, space);
    }
    [[nodiscard]] inline std::shared_ptr<UiEffect> inset_shadow(glm::vec2 offset, f32 blur_sigma, const Color &color, f32 strength = 1.0f) {
        return inset_shadow(offset, blur_sigma, color.vec4(), strength, ColorSpace::Srgb);
    }

    /// One colour stop of a gradient.
    struct GradientStop {
        /// Straight-alpha components in `space`.
        glm::vec4 color{1.0f};
        /// 0..1 along the gradient; stops must be in ascending order.
        f32 position = 0.0f;
        /// The space `color` is written in. sRGB for a hex-style colour; `Oklch`, `DisplayP3`, ... for a colour in that space.
        ColorSpace space = ColorSpace::Srgb;
        /// How the gradient travels from this stop to the next one, when that differs from the gradient's own interpolation.
        std::optional<ColorInterpolation> toward_next{};

        constexpr GradientStop() noexcept = default;
        constexpr GradientStop(glm::vec4 stop_color, f32 stop_position, ColorSpace stop_space = ColorSpace::Srgb,
                               std::optional<ColorInterpolation> to_next = std::nullopt) noexcept
            : color(stop_color), position(stop_position), space(stop_space), toward_next(to_next) {}
        constexpr GradientStop(glm::vec3 stop_color, f32 stop_position, ColorSpace stop_space = ColorSpace::Srgb,
                               std::optional<ColorInterpolation> to_next = std::nullopt) noexcept
            : color(stop_color, 1.0f), position(stop_position), space(stop_space), toward_next(to_next) {}
        constexpr GradientStop(const Color &stop_color, f32 stop_position, std::optional<ColorInterpolation> to_next = std::nullopt) noexcept
            : color(stop_color.vec4()), position(stop_position), space(ColorSpace::Srgb), toward_next(to_next) {}
    };

    namespace detail {

        [[nodiscard]] inline u32 pack_interpolation(const ColorInterpolation &method) noexcept {
            return static_cast<u32>(method.space) | (static_cast<u32>(method.hue) << 8u);
        }

        [[nodiscard]] inline std::shared_ptr<UiEffect> gradient(const char *name, f32 kind, glm::vec4 geometry, std::span<const GradientStop> stops,
                                                                const ColorInterpolation &interpolation) {
            auto effect = std::make_shared<UiEffect>();
            effect->name = UString{name};
            UiEffectPass pass;
            pass.label = UString{name};
            pass.shader_path = UString{"Shaders/ui_effect_gradient.slang"};
            pass.module_name = UString{"ui_effect_gradient"};
            pass.inputs = {UiEffectInput{.kind = UiEffectInputKind::GroupContent}};
            glm::vec4 colors[3]{};
            f32 positions[3]{};
            ColorInterpolation segments[2]{interpolation, interpolation};
            const usize count = std::min<usize>(stops.size(), 3);
            if (count == 0) {
                colors[0] = colors[1] = colors[2] = glm::vec4{0.0f};
                positions[0] = 0.0f;
                positions[1] = positions[2] = 1.0f;
            }
            for (usize i = 0; i < 3 && count > 0; ++i) {
                // Missing stops repeat the last one, so a two-stop gradient is a three-stop gradient that holds its end colour.
                const GradientStop &stop = stops[std::min(i, count - 1)];
                colors[i] = linear_rgba(stop.color, stop.space);
                positions[i] = i < count ? stop.position : 1.0f;
                if (i < 2 && i < count && stop.toward_next) {
                    segments[i] = *stop.toward_next;
                }
            }
            for (const glm::vec4 &color : colors) {
                append_bytes(pass.parameters, color);
            }
            append_bytes(pass.parameters, glm::vec4{positions[0], positions[1], positions[2], kind});
            geometry.w = std::bit_cast<f32>(pack_interpolation(segments[0]) | (pack_interpolation(segments[1]) << 16u));
            append_bytes(pass.parameters, geometry);
            effect->passes.push_back(std::move(pass));
            return effect;
        }

    } // namespace detail

    /// A linear gradient across the element (CSS `linear-gradient`). `angle_radians` 0 runs left to right and grows clockwise, so
    /// pi/2 runs top to bottom. Two or three stops (extra stops are ignored). `interpolation` is the colour space (and hue method) the
    /// colours are mixed in; Oklab unless you say otherwise.
    [[nodiscard]] inline std::shared_ptr<UiEffect> linear_gradient(f32 angle_radians, std::span<const GradientStop> stops,
                                                                   const ColorInterpolation &interpolation = {}) {
        return detail::gradient("linear gradient", 0.0f, glm::vec4{0.0f, 0.0f, angle_radians, 0.0f}, stops, interpolation);
    }

    /// A radial gradient (CSS `radial-gradient`) centred at `center` (0..1 across the element) reaching its last stop at `radius`, a
    /// fraction of the element's longer side.
    [[nodiscard]] inline std::shared_ptr<UiEffect> radial_gradient(glm::vec2 center, f32 radius, std::span<const GradientStop> stops,
                                                                   const ColorInterpolation &interpolation = {}) {
        return detail::gradient("radial gradient", 1.0f, glm::vec4{center.x, center.y, radius, 0.0f}, stops, interpolation);
    }

    /// A conic gradient (CSS `conic-gradient`): the colours sweep once around `center`, starting at `start_radians` (0 = to the right,
    /// growing clockwise).
    [[nodiscard]] inline std::shared_ptr<UiEffect> conic_gradient(glm::vec2 center, f32 start_radians, std::span<const GradientStop> stops,
                                                                  const ColorInterpolation &interpolation = {}) {
        return detail::gradient("conic gradient", 2.0f, glm::vec4{center.x, center.y, start_radians, 0.0f}, stops, interpolation);
    }

} // namespace SFT::UI::Effects
