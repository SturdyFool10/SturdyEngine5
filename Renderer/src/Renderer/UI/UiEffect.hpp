#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <format>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Renderer/Handles.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/UiQuad.hpp>

// Shader effects on UI content. An `ElementDecl::effect` makes everything an element draws -- its own background, border and
// images, text, strokes, and every descendant -- one *group*. The group is drawn into an off-screen image, handed through the
// passes below, and the result is composited back where the group would have appeared. The group is expressed in SEClay's
// command stream as an EFFECT_START / EFFECT_END pair (UI/SEClay/SEClay.h, SEClay_CustomElementConfig::effectData) and
// resolved by Context::finish_frame() into a FrameSnapshot::effect_groups() entry; UiRenderer consumes it.
//
// An effect is a small graph: an ordered list of passes, each a Slang fragment shader that reads any of the group's content, an
// earlier pass's output, or a texture the application supplies, and writes its own render target. The last pass's output is what
// is composited. That is enough for a drop shadow (blur the group's alpha, tint, draw under the original), a bloom (threshold,
// downsample, blur, add), a colour grade, a ripple or a displacement, and for passes that build and sample their own mip chain.
//
// This header is deliberately free of SEClay and Vulkan types: it is the data an application builds, and it is checked (but
// not compiled) by `validate_effect` so a bad description is rejected with a reason before a frame ever uses it.
namespace SFT::UI {

    /// How a pass's input is filtered when sampled.
    enum class UiEffectFilter : u8 { Nearest, Linear };

    /// What a sample outside [0, 1] returns.
    enum class UiEffectAddress : u8 { Clamp, Repeat, Mirror, Border };

    /// The sampler a pass binds for one input. Defaults are bilinear, trilinear across mips, clamped to the edge.
    struct UiEffectSampler {
        UiEffectFilter min_filter = UiEffectFilter::Linear;
        UiEffectFilter mag_filter = UiEffectFilter::Linear;
        /// Blend between mip levels (Linear) or snap to the nearest one (Nearest). Irrelevant for an input with one mip.
        UiEffectFilter mip_filter = UiEffectFilter::Linear;
        UiEffectAddress address_u = UiEffectAddress::Clamp;
        UiEffectAddress address_v = UiEffectAddress::Clamp;
        /// Added to the computed mip level before sampling; positive blurs, negative sharpens.
        f32 lod_bias = 0.0f;
        /// The range of mip levels a sample may use. The defaults allow every level the image has.
        f32 min_lod = 0.0f;
        f32 max_lod = 1000.0f;
        /// 1 disables anisotropic filtering; the device's limit caps larger values.
        f32 max_anisotropy = 1.0f;
    };

    /// How a generated mip level is made from the one above it.
    enum class UiMipFilter : u8 {
        /// The average of each 2x2 block of the level above (one bilinear tap at the block's centre). Cheap and exact for flat content.
        Box,
        /// A 3x3 tent over the level above: smoother, so a blur read from a high level is free of the blockiness a box chain shows.
        Tent,
    };

    enum class UiEffectInputKind : u8 {
        /// What the element and its descendants drew, as premultiplied RGBA at the group's pixel size. The same image for every
        /// pass that asks for it.
        GroupContent,
        /// The render target an earlier pass of this effect wrote (`pass` is its index).
        PassOutput,
        /// A texture the application owns (`texture`).
        Texture,
        /// What is already drawn behind the group (the scene and earlier overlays), copied from the render target at the group's
        /// region, opaque. Not available inside nested groups; a group that asks for it where it is missing is drawn plainly.
        Backdrop,
    };

    /// One texture a pass samples. The first input is bound as `Texture2D sourceTexture` and the rest as `Texture2D extraTexture0`,
    /// `extraTexture1`, ... in set 0 of the pass's shader, all read through the pass's one `SamplerState sourceSampler`. A pass with no
    /// inputs gets `GroupContent` as its source, so a purely procedural shader still compiles against the same bindings.
    struct UiEffectInput {
        UiEffectInputKind kind = UiEffectInputKind::GroupContent;
        /// `PassOutput`: index of the pass whose target to read; must be lower than this pass's own index.
        u32 pass = 0;
        /// `Texture`: the image to read.
        Renderer::TextureHandle texture{};
        /// The mip levels the shader can see: from `base_mip`, `mip_count` of them (0 = all the rest). Lets a pass read one level of
        /// a chain as if it were a whole image. Only for `GroupContent` and `PassOutput`; an application texture is read whole.
        u32 base_mip = 0;
        u32 mip_count = 0;
    };

    /// The image a pass renders into.
    struct UiEffectTarget {
        /// Colour format. Half-float by default so intermediate HDR values (a bright blur source) are not clipped.
        RHI::Format format = RHI::Format::RGBA16Float;
        /// Size relative to the group's pixel size (0.5 = half width and height). Each axis is at least one pixel.
        f32 scale = 1.0f;
        /// Mip levels to allocate: 1 for none, 0 for the whole chain down to 1x1, otherwise that many (at most the full chain). The
        /// pass draws level 0; the renderer then makes each lower level from the one above with `mip_filter`, so a later pass can
        /// sample the chain (`SampleLevel`, or trilinear with a mip-aware sampler) for wide blurs and bloom pyramids at a few taps.
        u32 mip_levels = 1;
        UiMipFilter mip_filter = UiMipFilter::Tent;
        /// Clear before drawing. The clear colour is transparent black.
        bool clear = true;
    };

    /// The fixed prefix of every effect shader's push-constant block, written by the renderer; `UiEffectPass::parameters` follow
    /// it. Exactly 48 bytes (three 16-byte registers) so the first parameter starts on a register boundary with no implicit gap.
    /// Shaders declare it as the first members of their `[[push_constant]]` struct (see Shaders/sturdy_common.slang, `UiEffectConstants`).
    struct UiEffectConstants {
        /// Size of the pass's own render target in pixels.
        glm::vec2 target_size{0.0f};
        /// Size of the group's content image in pixels (what `GroupContent` inputs are).
        glm::vec2 content_size{0.0f};
        /// Where the group sits on the UI surface, and its size, in surface pixels.
        glm::vec2 group_origin{0.0f};
        glm::vec2 group_size{0.0f};
        /// Index of this pass within the effect.
        u32 pass_index = 0;
        /// Frames the effect has been drawn, so animated effects need no clock of their own.
        u32 frame_index = 0;
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
    };
    static_assert(sizeof(UiEffectConstants) == 48, "UiEffectConstants is three 16-byte registers; Shaders/sturdy_common.slang depends on it");

    /// One pass: a fragment shader drawn over a full-target triangle.
    struct UiEffectPass {
        /// Shown in diagnostics and GPU captures.
        UString label;
        /// Both entry points live in this Slang file (the convention CustomShaderRef and CustomPostProcessEffect use). The vertex
        /// stage is the engine's fullscreen triangle (`uiEffectVertex` in sturdy_common.slang) unless the file defines `vertexMain`.
        UString shader_path;
        UString module_name;
        UString fragment_entry_point{"fragmentMain"};
        std::vector<UiEffectInput> inputs;
        /// How every input of this pass is sampled.
        UiEffectSampler sampler{};
        UiEffectTarget output{};
        /// Appended after the `UiEffectConstants` prefix. Its size must equal the shader's reflected push-constant struct minus
        /// the prefix, or the pass is rejected when it is first prepared. A multiple of 4 bytes.
        std::vector<std::byte> parameters;
    };

    /// How the finished image is laid over what is already drawn (`UiQuadBlend`, UI/UiQuad.hpp): `Normal` is premultiplied source-over, the
    /// way to put a shaded widget on a background; `Additive` adds the result (glow, bloom, light); `Multiply` multiplies the destination by
    /// it (shadows, tint, vignettes); `Replace` overwrites the destination inside the region, so a transparent result cuts a hole.
    using UiEffectBlend = UiQuadBlend;

    /// Extra pixels around the element that the effect may draw into: a blur or shadow spreads past the widget's own bounds.
    struct UiEffectOutset {
        f32 left = 0.0f;
        f32 right = 0.0f;
        f32 top = 0.0f;
        f32 bottom = 0.0f;

        [[nodiscard]] static constexpr UiEffectOutset all(f32 pixels) noexcept { return UiEffectOutset{pixels, pixels, pixels, pixels}; }
    };

    /// The mip chain made from the group's content image, for passes that read `GroupContent` at several scales.
    struct UiEffectContent {
        /// 1 for none, 0 for the whole chain, otherwise that many (at most the full chain).
        u32 mip_levels = 1;
        UiMipFilter mip_filter = UiMipFilter::Tent;
    };

    /// A 2D transform as `Effects::transform` makes it (CSS `transform` + `transform-origin`): scale, then rotate (clockwise), then translate
    /// by `translate` surface pixels, about `origin` (0..1 across the element's box). Carried on the effect so hit testing can follow it.
    struct UiTransform2D {
        glm::vec2 translate{0.0f};
        f32 angle_radians = 0.0f;
        glm::vec2 scale{1.0f};
        glm::vec2 origin{0.5f};
    };

    /// The shape of a `Effects::clip_*` effect (CSS `clip-path`), kept on the effect so hit testing can cut an element's (and its children's)
    /// reactive area to it too. Lengths are surface pixels, `center` and `points` fractions across the element's box.
    struct UiClipShape {
        enum class Kind : u8 { Inset, Circle, Ellipse, Polygon, RoundedBox };
        Kind kind = Kind::Inset;
        /// Inset: top, right, bottom, left.
        glm::vec4 inset{0.0f};
        f32 radius = 0.0f;
        glm::vec2 center{0.5f};
        /// Ellipse radii.
        glm::vec2 radii{0.0f};
        glm::vec2 points[6]{};
        u32 point_count = 0;
        /// RoundedBox: corner radii in surface pixels (top left, top right, bottom left, bottom right) and the corners' `CornerShape` bytes.
        glm::vec4 corner_radii{0.0f};
        u32 corner_codes = 0;
    };

    /// The mask of a `Effects::mask_*` effect (CSS `mask-image`), kept on the effect so `HitTestMode::Fixed` can make masks reactive: where the mask
    /// hides the element, it does not react.
    struct UiMaskShape {
        bool image = false;
        /// Linear: the alpha ramp (angle clockwise from left to right, alpha at `start` and at `end`, both 0..1 along the line).
        f32 angle_radians = 0.0f;
        f32 alpha_start = 1.0f;
        f32 alpha_end = 0.0f;
        f32 start = 0.0f;
        f32 end = 1.0f;
        bool invert = false;
        /// Image: the texture, and whether its luminance (rather than alpha) is the mask.
        Renderer::TextureHandle texture{};
        bool luminance = false;
    };

    /// A full effect: what `ElementDecl::effect` points at. Build once and share it between frames and elements; it is read-only
    /// while in use (change it by making a new one) and is kept alive by the frame that draws it.
    struct UiEffect {
        UString name;
        /// Mips of the group's content (`GroupContent` inputs); none by default.
        UiEffectContent content{};
        std::vector<UiEffectPass> passes;
        UiEffectBlend composite = UiEffectBlend::Normal;
        /// Multiplies the composited result.
        f32 opacity = 1.0f;
        /// Region the group's content image and the result cover beyond the element's own box.
        UiEffectOutset outset{};
        /// Draw the result only inside the element's own box (plus the outset) rather than the whole surface.
        bool clip_to_bounds = true;
        /// 0 draws the group where it sits in paint order. A higher value hoists the element and everything inside it out of the main
        /// pass into a later one (`UiSurface::finish_overlays` returns one overlay pass per layer, lowest first), so what it draws can
        /// read, as `Backdrop`, everything the lower layers drew. Only a top-level group (not nested inside another effect group) is
        /// hoisted. An effect with no passes and a layer is just a layer marker (`UI::Effects::layer`). Drawn through
        /// `finish_overlay` (one pass) the layer is ignored: everything is drawn in place.
        u8 layer = 0;
        /// Set by `Effects::transform`: the element is drawn transformed, and the UI's hover and click tests (`Context::hovered`,
        /// `clicked`) map the pointer back through it so a rotated button reacts where it is drawn.
        std::optional<UiTransform2D> transform{};
        /// Set by `Effects::clip_*`: the reactive area is cut to this shape (CSS `clip-path` affects pointer events).
        std::optional<UiClipShape> clip_shape{};
        /// Set by `Effects::mask_*`: with `HitTestMode::Fixed` the reactive area follows the mask.
        std::optional<UiMaskShape> mask{};
    };

    /// The most inputs one pass may bind, passes one effect may hold, and parameter bytes one pass may carry (128-byte push-constant
    /// budget minus the 48-byte prefix).
    inline constexpr usize kUiEffectMaxInputs = 8;
    inline constexpr usize kUiEffectMaxPasses = 16;
    inline constexpr usize kUiEffectMaxParameterBytes = 80;

    /// Checks `effect` for mistakes that would otherwise show up as a black rectangle: an empty pass list, a shader without a
    /// path, an input naming a pass that has not run yet, an out-of-range scale or mip count, parameters that are not a whole
    /// number of 4-byte words, a texture input with no texture. Returns the reason for the first problem, or nothing when the
    /// description is usable. It does not compile shaders (that happens when the effect is first prepared) and cannot know whether
    /// a texture handle is still alive.
    [[nodiscard]] inline std::optional<UString> validate_effect(const UiEffect &effect) {
        const auto problem = [](std::string text) { return std::optional<UString>{UString{text}}; };
        const std::string name = effect.name.empty() ? std::string{"(unnamed effect)"} : "'" + effect.name.cpp_string() + "'";
        if (effect.passes.empty() && effect.layer == 0) {
            return problem(name + " has no passes (and is not a layer).");
        }
        if (effect.passes.size() > kUiEffectMaxPasses) {
            return problem(std::format("{} has {} passes; the limit is {}.", name, effect.passes.size(), kUiEffectMaxPasses));
        }
        if (effect.content.mip_levels > 16) {
            return problem(std::format("{} asks for {} content mip levels; the limit is 16.", name, effect.content.mip_levels));
        }
        if (!(effect.opacity >= 0.0f && effect.opacity <= 1.0f)) {
            return problem(std::format("{} has opacity {}; it must be between 0 and 1.", name, effect.opacity));
        }
        const auto outset_ok = [](f32 value) { return value >= 0.0f && value <= 4096.0f; };
        if (!outset_ok(effect.outset.left) || !outset_ok(effect.outset.right) || !outset_ok(effect.outset.top) ||
            !outset_ok(effect.outset.bottom)) {
            return problem(name + " has an outset outside 0..4096 pixels.");
        }
        for (usize index = 0; index < effect.passes.size(); ++index) {
            const UiEffectPass &pass = effect.passes[index];
            const std::string where = std::format("{} pass {}{}", name, index,
                                                  pass.label.empty() ? std::string{} : " ('" + pass.label.cpp_string() + "')");
            if (pass.shader_path.empty()) {
                return problem(where + " has no shader_path.");
            }
            if (pass.module_name.empty()) {
                return problem(where + " has no module_name.");
            }
            if (pass.fragment_entry_point.empty()) {
                return problem(where + " has no fragment_entry_point.");
            }
            if (pass.inputs.size() > kUiEffectMaxInputs) {
                return problem(std::format("{} binds {} inputs; the limit is {}.", where, pass.inputs.size(), kUiEffectMaxInputs));
            }
            for (usize input_index = 0; input_index < pass.inputs.size(); ++input_index) {
                const UiEffectInput &input = pass.inputs[input_index];
                const std::string input_where = std::format("{} input {}", where, input_index);
                if (input.kind == UiEffectInputKind::PassOutput && input.pass >= index) {
                    return problem(std::format("{} reads the output of pass {}, which has not run yet (a pass may only read earlier ones).",
                                               input_where, input.pass));
                }
                if (input.kind == UiEffectInputKind::Texture && !input.texture) {
                    return problem(input_where + " is a Texture input with no texture.");
                }
                if (input.kind == UiEffectInputKind::Texture && (input.base_mip != 0 || input.mip_count != 0)) {
                    return problem(input_where + " restricts the mip range of an application texture; only GroupContent and PassOutput inputs can.");
                }
            }
            if (!(pass.output.scale > 0.0f && pass.output.scale <= 4.0f)) {
                return problem(std::format("{} has output scale {}; it must be in (0, 4].", where, pass.output.scale));
            }
            if (pass.output.mip_levels > 16) {
                return problem(std::format("{} asks for {} mip levels; the limit is 16.", where, pass.output.mip_levels));
            }
            if (!(pass.sampler.max_anisotropy >= 1.0f && pass.sampler.max_anisotropy <= 16.0f)) {
                return problem(where + " has max_anisotropy outside 1..16.");
            }
            if (pass.output.format == RHI::Format::Undefined) {
                return problem(where + " has an undefined output format.");
            }
            if (pass.parameters.size() % 4 != 0) {
                return problem(std::format("{} has {} parameter bytes; they must be a multiple of 4.", where, pass.parameters.size()));
            }
            if (pass.parameters.size() > kUiEffectMaxParameterBytes) {
                return problem(std::format("{} has {} parameter bytes; the limit is {}.", where, pass.parameters.size(), kUiEffectMaxParameterBytes));
            }
        }
        return std::nullopt;
    }

    /// One resolved effect group in a FrameSnapshot (see `FrameSnapshot::effect_groups()`): the element's bounds and which draws
    /// are inside it. Draws belong to the group when their `PaintKey::order` is in (`paint.order`, `last_order`); the group's own
    /// START and END commands hold `paint.order` and `last_order`, and nothing else uses those numbers.
    struct EffectGroupDraw;
    /// The groups that do real work: layer markers (an effect with no passes) are removed and the groups nested in them are re-parented
    /// to the nearest group that remains.
    [[nodiscard]] inline std::vector<EffectGroupDraw> flatten_effect_groups(std::span<const EffectGroupDraw> groups);

    struct EffectGroupDraw {
        const UiEffect *effect = nullptr;
        /// The element's box, in surface pixels.
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        RHI::Rect2D scissor{};
        PaintKey paint{};
        u32 last_order = 0;
        /// Index in the snapshot's groups of the group this one is nested in, or -1.
        i32 parent = -1;
        /// Pixels per logical unit this frame was laid out at. `position`, `size` and `scissor` are already in pixels; the effect's
        /// own outset (and anything else the application states in logical units) is multiplied by this.
        f32 pixel_scale = 1.0f;
    };

    inline std::vector<EffectGroupDraw> flatten_effect_groups(std::span<const EffectGroupDraw> groups) {
        std::vector<EffectGroupDraw> out;
        out.reserve(groups.size());
        std::vector<i32> new_index(groups.size(), -1);
        std::vector<i32> parent_after(groups.size(), -1); // what a child of group i should be parented to once markers are gone
        for (usize i = 0; i < groups.size(); ++i) {
            const i32 parent = groups[i].parent < 0 ? -1 : parent_after[static_cast<usize>(groups[i].parent)];
            if (groups[i].effect != nullptr && groups[i].effect->passes.empty()) {
                parent_after[i] = parent;
                continue;
            }
            EffectGroupDraw copy = groups[i];
            copy.parent = parent;
            new_index[i] = static_cast<i32>(out.size());
            parent_after[i] = new_index[i];
            out.push_back(copy);
        }
        return out;
    }

} // namespace SFT::UI
