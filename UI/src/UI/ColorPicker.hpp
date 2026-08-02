#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#pragma endregion

#include "Context.hpp"
#include "Dropdown.hpp"
#include "Style.hpp"
#include "WidgetComposition.hpp"

// Engine-native color picker rendered entirely through ordinary UI quads: a conventional sRGB-HSV
// saturation/value plane, hue strip, optional alpha strip over a checkerboard, preview swatch, and
// a dropdown selecting any color space Foundation defines. Persistent state retains the last
// meaningful hue through grayscale/black, avoiding the common "desaturate blue, then resaturate and
// get red" bug. No platform dialog or DOM is involved.
namespace SFT::UI {

    // Runtime counterpart to Foundation::Color's concrete compile-time color types. Values stay in
    // the same order as the dropdown and ColorPickerValue variant below.
    enum class ColorPickerColorSpace : u8 {
        Srgb,
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
        Count,
    };

    using ColorPickerValue = std::variant<
        Foundation::Color::Srgb,
        Foundation::Color::Linear,
        Foundation::Color::Xyz,
        Foundation::Color::AdobeRgb,
        Foundation::Color::DisplayP3,
        Foundation::Color::Rec2020,
        Foundation::Color::Hsl,
        Foundation::Color::Hsv,
        Foundation::Color::Hwb,
        Foundation::Color::Lab,
        Foundation::Color::Lch,
        Foundation::Color::Luv,
        Foundation::Color::Oklab,
        Foundation::Color::Oklch>;

    static_assert(std::variant_size_v<ColorPickerValue> == static_cast<usize>(ColorPickerColorSpace::Count));

    enum class ColorPickerPart : u8 { None,
                                      SaturationValue,
                                      Hue,
                                      Alpha };
    enum class ColorPickerKey : u8 { Left,
                                     Right,
                                     Up,
                                     Down,
                                     PageDecrement,
                                     PageIncrement,
                                     Minimum,
                                     Maximum };

    struct ColorPickerInput {
        std::span<const ColorPickerKey> keys{};
        std::optional<ColorPickerPart> request_focus;
        // Programmatic selection, useful for restoring serialized editor state. It does not emit a
        // user-change event; selecting an entry from the dropdown does.
        std::optional<ColorPickerColorSpace> requested_color_space;
        // Drives the dropdown trigger's ButtonStyle transition.
        f32 delta_seconds = 0.0f;
        bool request_blur = false;
    };

    struct ColorPickerConfig {
        bool show_color_space_dropdown = true;
        bool show_alpha = true;
        bool show_preview = true;
        f64 keyboard_step = 0.01;
        f64 page_step = 0.1;
    };

    struct ColorPickerStyle {
        glm::vec2 plane_size{220.0f, 160.0f};
        f32 bar_height = 18.0f;
        f32 preview_height = 28.0f;
        f32 color_space_dropdown_height = 32.0f;
        u16 gap = 10;
        // color_space_text.font_id and color_space_dropdown.arrow_font_id must reference registered
        // fonts, following DropdownStyle's normal text/arrow contract.
        DropdownStyle color_space_dropdown{};
        TextStyle color_space_text{.color = Color{0.92, 0.93, 0.95, 1.0}, .font_size = 14, .wrap_mode = TextWrapMode::None};
        f32 plane_cursor_size = 14.0f;
        f32 bar_cursor_width = 4.0f;
        Color cursor_color{1.0, 1.0, 1.0, 1.0};
        Color cursor_shadow{0.0, 0.0, 0.0, 0.85};
        Color checker_light{0.72, 0.72, 0.72, 1.0};
        Color checker_dark{0.42, 0.42, 0.42, 1.0};
        BorderStyle border{.color = Color{0.08, 0.08, 0.10, 1.0}, .width = BorderWidth::all(1)};
        BorderStyle focused_border{.color = Color{0.55, 0.72, 1.0, 1.0}, .width = BorderWidth::all(2)};
        f64 disabled_opacity = 0.5;
    };

    enum class ColorPickerVisualPart : u8 {
        Root,
        Dropdown,
        SaturationValue,
        SaturationValueMarker,
        Hue,
        HueMarker,
        Alpha,
        AlphaMarker,
        Preview,
        Label,
        Tooltip,
    };

    [[nodiscard]] inline UString color_picker_part_id(const UString &id, ColorPickerVisualPart part, ColorPickerVisualPart target = ColorPickerVisualPart::Root) {
        const char *suffix = part == ColorPickerVisualPart::Dropdown                ? "#color-space"
                             : part == ColorPickerVisualPart::SaturationValue       ? "#sv"
                             : part == ColorPickerVisualPart::SaturationValueMarker ? "#sv-marker"
                             : part == ColorPickerVisualPart::Hue                   ? "#hue"
                             : part == ColorPickerVisualPart::HueMarker             ? "#hue-marker"
                             : part == ColorPickerVisualPart::Alpha                 ? "#alpha"
                             : part == ColorPickerVisualPart::AlphaMarker           ? "#alpha-marker"
                             : part == ColorPickerVisualPart::Preview               ? "#preview"
                             : part == ColorPickerVisualPart::Tooltip               ? "#tooltip"
                             : part == ColorPickerVisualPart::Label                 ? "#label:"
                                                                                    : "";
        if (part == ColorPickerVisualPart::Root)
            return id;
        const bool targeted = part == ColorPickerVisualPart::Label;
        return UString{id.cpp_string() + suffix +
                       (targeted ? std::to_string(static_cast<u8>(target)) : std::string{})};
    }

    struct ColorPickerPartContext {
        ColorPickerVisualPart part = ColorPickerVisualPart::Root;
        // For Label this identifies the control being labeled; otherwise it equals part.
        ColorPickerVisualPart target = ColorPickerVisualPart::Root;
        PartVisualState visual{};
        UString id;
        Color color{};
        ColorPickerColorSpace color_space = ColorPickerColorSpace::Srgb;
        f64 hue = 0.0;
        f64 saturation = 0.0;
        f64 value = 0.0;
        f64 alpha = 1.0;
        glm::vec2 normalized_position{0.0f};
        std::optional<ElementBounds> bounds;
    };

    struct ColorPickerComposition {
        PartSlot<ColorPickerPartContext> root{};
        PartSlot<ColorPickerPartContext> dropdown{};
        PartSlot<ColorPickerPartContext> saturation_value{};
        PartSlot<ColorPickerPartContext> saturation_value_marker{};
        PartSlot<ColorPickerPartContext> hue{};
        PartSlot<ColorPickerPartContext> hue_marker{};
        PartSlot<ColorPickerPartContext> alpha{};
        PartSlot<ColorPickerPartContext> alpha_marker{};
        PartSlot<ColorPickerPartContext> preview{};
        PartSlot<ColorPickerPartContext> label{.visible = false, .render_default = false};
        PartSlot<ColorPickerPartContext> tooltip{.visible = false, .render_default = false};
    };

    class ColorPickerState {
      public:
        [[nodiscard]] ColorPickerPart active_part() const noexcept { return active_part_; }
        [[nodiscard]] ColorPickerColorSpace color_space() const noexcept { return color_space_; }
        void set_color_space(ColorPickerColorSpace color_space) noexcept {
            if (color_space < ColorPickerColorSpace::Count) {
                color_space_ = color_space;
            }
        }

      private:
        friend struct DetailColorPickerAccess;
        bool initialized_ = false;
        f64 hue_ = 0.0;
        f64 saturation_ = 0.0;
        f64 value_ = 0.0;
        f64 alpha_ = 1.0;
        Color last_output_{};
        ColorPickerPart active_part_ = ColorPickerPart::None;
        bool changed_during_gesture_ = false;
        ColorPickerColorSpace color_space_ = ColorPickerColorSpace::Srgb;
        DropdownState color_space_dropdown_{};
    };

    struct ColorPickerResult {
        // Render-ready engine color plus the same color converted to the dropdown's selected
        // Foundation type. Use std::get<Foundation::Color::Oklch>(value), etc. when typed channels
        // are needed without repeating a conversion at every call site.
        Color color{};
        ColorPickerValue value{Foundation::Color::Srgb{}};
        ColorPickerColorSpace color_space = ColorPickerColorSpace::Srgb;
        bool color_space_changed = false;
        bool changed = false;
        bool committed = false;
        bool cancelled = false;
        bool hovered = false;
        ColorPickerPart active_part = ColorPickerPart::None;
        ColorPickerPart focused_part = ColorPickerPart::None;
    };

    struct DetailColorPickerAccess {
        static bool &initialized(ColorPickerState &s) noexcept { return s.initialized_; }
        static f64 &hue(ColorPickerState &s) noexcept { return s.hue_; }
        static f64 &saturation(ColorPickerState &s) noexcept { return s.saturation_; }
        static f64 &value(ColorPickerState &s) noexcept { return s.value_; }
        static f64 &alpha(ColorPickerState &s) noexcept { return s.alpha_; }
        static Color &last_output(ColorPickerState &s) noexcept { return s.last_output_; }
        static ColorPickerPart &active(ColorPickerState &s) noexcept { return s.active_part_; }
        static bool &gesture_changed(ColorPickerState &s) noexcept { return s.changed_during_gesture_; }
        static ColorPickerColorSpace &color_space(ColorPickerState &s) noexcept { return s.color_space_; }
        static DropdownState &color_space_dropdown(ColorPickerState &s) noexcept { return s.color_space_dropdown_; }
    };

    namespace Detail {

        struct SrgbHsv {
            f64 h = 0.0;
            f64 s = 0.0;
            f64 v = 0.0;
        };

        [[nodiscard]] inline SrgbHsv srgb_to_picker_hsv(const Color &color) noexcept {
            const auto finite_channel = [](f64 channel) noexcept {
                return std::isfinite(channel) ? std::clamp(channel, 0.0, 1.0) : 0.0;
            };
            const f64 r = finite_channel(color.r);
            const f64 g = finite_channel(color.g);
            const f64 b = finite_channel(color.b);
            const f64 maximum = std::max({r, g, b});
            const f64 minimum = std::min({r, g, b});
            const f64 delta = maximum - minimum;
            SrgbHsv result{.s = maximum > 0.0 ? delta / maximum : 0.0, .v = maximum};
            if (delta <= 1.0e-12) {
                return result;
            }
            if (maximum == r) {
                result.h = 60.0 * std::fmod((g - b) / delta, 6.0);
            } else if (maximum == g) {
                result.h = 60.0 * ((b - r) / delta + 2.0);
            } else {
                result.h = 60.0 * ((r - g) / delta + 4.0);
            }
            if (result.h < 0.0)
                result.h += 360.0;
            return result;
        }

        [[nodiscard]] inline Color picker_hsv_to_srgb(f64 hue, f64 saturation, f64 value, f64 alpha = 1.0) noexcept {
            hue = std::isfinite(hue) ? std::fmod(hue, 360.0) : 0.0;
            if (hue < 0.0)
                hue += 360.0;
            saturation = std::isfinite(saturation) ? std::clamp(saturation, 0.0, 1.0) : 0.0;
            value = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
            alpha = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 1.0;
            const f64 chroma = value * saturation;
            const f64 sector = hue / 60.0;
            const f64 x = chroma * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
            f64 r = 0.0;
            f64 g = 0.0;
            f64 b = 0.0;
            if (sector < 1.0) {
                r = chroma;
                g = x;
            } else if (sector < 2.0) {
                r = x;
                g = chroma;
            } else if (sector < 3.0) {
                g = chroma;
                b = x;
            } else if (sector < 4.0) {
                g = x;
                b = chroma;
            } else if (sector < 5.0) {
                r = x;
                b = chroma;
            } else {
                r = chroma;
                b = x;
            }
            const f64 match = value - chroma;
            return Color{r + match, g + match, b + match, alpha};
        }

        [[nodiscard]] inline Color with_opacity(Color color, f64 opacity) noexcept {
            color.a *= std::clamp(opacity, 0.0, 1.0);
            return color;
        }

        [[nodiscard]] inline Color composite_opaque(const Color &foreground, const Color &background) noexcept {
            const f64 alpha = std::clamp(foreground.a, 0.0, 1.0);
            return Color{
                foreground.r * alpha + background.r * (1.0 - alpha),
                foreground.g * alpha + background.g * (1.0 - alpha),
                foreground.b * alpha + background.b * (1.0 - alpha),
                1.0,
            };
        }

        // Builds a custom_element() push-constant payload: an 8-byte gap (Slang pads a
        // [[push_constant]] struct's first trailing float4 field to the next 16-byte boundary,
        // which lands 8 bytes after UiElementConstants' 24-byte prefix, not immediately after it —
        // see CustomElement.hpp's own doc comment) followed by each vec4 packed contiguously. Every
        // Shaders/ui_color_picker_*.slang shader below declares its extra fields as plain float4s
        // for exactly this reason, so this one packer covers all three.
        [[nodiscard]] inline vector<std::byte> pack_gradient_shader_params(std::initializer_list<glm::vec4> vec4_fields) {
            vector<f32> words;
            words.reserve(2 + vec4_fields.size() * 4);
            words.push_back(0.0f);
            words.push_back(0.0f);
            for (const glm::vec4 &field : vec4_fields) {
                words.push_back(field.x);
                words.push_back(field.y);
                words.push_back(field.z);
                words.push_back(field.w);
            }
            vector<std::byte> bytes(words.size() * sizeof(f32));
            std::memcpy(bytes.data(), words.data(), bytes.size());
            return bytes;
        }

        // (topLeft, topRight, bottomLeft, bottomRight) — matches Style.hpp's CornerRadius field
        // order and roundedBoxSDF's own expected layout (sturdy_common.slang).
        [[nodiscard]] inline glm::vec4 corner_radius_vec4(const CornerRadius &radius) noexcept {
            return glm::vec4{radius.top_left, radius.top_right, radius.bottom_left, radius.bottom_right};
        }

        // Per-pixel saturation/value gradient (Shaders/ui_color_picker_sv_plane.slang) — replaces
        // the old up-to-32x32 grid of flat-colored elements with one real shader draw. `corner_radius`
        // should be whatever the surrounding container's own (possibly app-customized via
        // ColorPickerComposition::saturation_value.alter_decl) ElementDecl::corner_radius resolved
        // to, so the gradient's own rounding always matches its container's border exactly.
        [[nodiscard]] inline CustomShaderRef sv_plane_shader(f64 hue, f64 opacity, const CornerRadius &corner_radius) {
            return CustomShaderRef{
                .shader_path = "Shaders/ui_color_picker_sv_plane.slang",
                .module_name = "ui_color_picker_sv_plane",
                .push_constants = pack_gradient_shader_params({
                    glm::vec4{static_cast<f32>(hue), static_cast<f32>(opacity), 0.0f, 0.0f},
                    corner_radius_vec4(corner_radius),
                }),
            };
        }

        // Per-pixel hue spectrum gradient (Shaders/ui_color_picker_hue_bar.slang) — replaces the
        // old up-to-64-segment strip of flat colors with one real shader draw.
        [[nodiscard]] inline CustomShaderRef hue_bar_shader(f64 opacity, const CornerRadius &corner_radius) {
            return CustomShaderRef{
                .shader_path = "Shaders/ui_color_picker_hue_bar.slang",
                .module_name = "ui_color_picker_hue_bar",
                .push_constants = pack_gradient_shader_params({
                    glm::vec4{static_cast<f32>(opacity), 0.0f, 0.0f, 0.0f},
                    corner_radius_vec4(corner_radius),
                }),
            };
        }

        // Per-pixel alpha gradient over a checkerboard (Shaders/ui_color_picker_alpha_bar.slang) —
        // replaces the old flat-segment-over-checker-cell approximation with one real shader draw.
        [[nodiscard]] inline CustomShaderRef alpha_bar_shader(const Color &foreground, f64 opacity,
                                                               const Color &checker_light, const Color &checker_dark,
                                                               const CornerRadius &corner_radius) {
            return CustomShaderRef{
                .shader_path = "Shaders/ui_color_picker_alpha_bar.slang",
                .module_name = "ui_color_picker_alpha_bar",
                .push_constants = pack_gradient_shader_params({
                    glm::vec4{static_cast<f32>(foreground.r), static_cast<f32>(foreground.g), static_cast<f32>(foreground.b),
                              static_cast<f32>(opacity)},
                    glm::vec4{static_cast<f32>(checker_light.r), static_cast<f32>(checker_light.g),
                              static_cast<f32>(checker_light.b), static_cast<f32>(checker_light.a)},
                    glm::vec4{static_cast<f32>(checker_dark.r), static_cast<f32>(checker_dark.g), static_cast<f32>(checker_dark.b),
                              static_cast<f32>(checker_dark.a)},
                    corner_radius_vec4(corner_radius),
                }),
            };
        }

        [[nodiscard]] inline ColorPickerValue color_picker_value(const Color &color,
                                                                 ColorPickerColorSpace color_space) noexcept {
            switch (color_space) {
                case ColorPickerColorSpace::Srgb:
                    return color;
                case ColorPickerColorSpace::Linear:
                    return Foundation::Color::convert_to<Foundation::Color::Linear>(color);
                case ColorPickerColorSpace::Xyz:
                    return Foundation::Color::convert_to<Foundation::Color::Xyz>(color);
                case ColorPickerColorSpace::AdobeRgb:
                    return Foundation::Color::convert_to<Foundation::Color::AdobeRgb>(color);
                case ColorPickerColorSpace::DisplayP3:
                    return Foundation::Color::convert_to<Foundation::Color::DisplayP3>(color);
                case ColorPickerColorSpace::Rec2020:
                    return Foundation::Color::convert_to<Foundation::Color::Rec2020>(color);
                case ColorPickerColorSpace::Hsl:
                    return Foundation::Color::convert_to<Foundation::Color::Hsl>(color);
                case ColorPickerColorSpace::Hsv:
                    return Foundation::Color::convert_to<Foundation::Color::Hsv>(color);
                case ColorPickerColorSpace::Hwb:
                    return Foundation::Color::convert_to<Foundation::Color::Hwb>(color);
                case ColorPickerColorSpace::Lab:
                    return Foundation::Color::convert_to<Foundation::Color::Lab>(color);
                case ColorPickerColorSpace::Lch:
                    return Foundation::Color::convert_to<Foundation::Color::Lch>(color);
                case ColorPickerColorSpace::Luv:
                    return Foundation::Color::convert_to<Foundation::Color::Luv>(color);
                case ColorPickerColorSpace::Oklab:
                    return Foundation::Color::convert_to<Foundation::Color::Oklab>(color);
                case ColorPickerColorSpace::Oklch:
                    return Foundation::Color::convert_to<Foundation::Color::Oklch>(color);
                case ColorPickerColorSpace::Count:
                    break;
            }
            return color;
        }

        inline void render_color_space_label(Context &ctx, ColorPickerColorSpace color_space, const TextStyle &style) {
            switch (color_space) {
                case ColorPickerColorSpace::Srgb:
                    ctx.text(u8"sRGB"_ustr, style);
                    break;
                case ColorPickerColorSpace::Linear:
                    ctx.text(u8"Linear RGB"_ustr, style);
                    break;
                case ColorPickerColorSpace::Xyz:
                    ctx.text(u8"CIE XYZ"_ustr, style);
                    break;
                case ColorPickerColorSpace::AdobeRgb:
                    ctx.text(u8"Adobe RGB"_ustr, style);
                    break;
                case ColorPickerColorSpace::DisplayP3:
                    ctx.text(u8"Display P3"_ustr, style);
                    break;
                case ColorPickerColorSpace::Rec2020:
                    ctx.text(u8"Rec. 2020"_ustr, style);
                    break;
                case ColorPickerColorSpace::Hsl:
                    ctx.text(u8"HSL"_ustr, style);
                    break;
                case ColorPickerColorSpace::Hsv:
                    ctx.text(u8"HSV"_ustr, style);
                    break;
                case ColorPickerColorSpace::Hwb:
                    ctx.text(u8"HWB"_ustr, style);
                    break;
                case ColorPickerColorSpace::Lab:
                    ctx.text(u8"CIELAB"_ustr, style);
                    break;
                case ColorPickerColorSpace::Lch:
                    ctx.text(u8"CIELCh"_ustr, style);
                    break;
                case ColorPickerColorSpace::Luv:
                    ctx.text(u8"CIELUV"_ustr, style);
                    break;
                case ColorPickerColorSpace::Oklab:
                    ctx.text(u8"Oklab"_ustr, style);
                    break;
                case ColorPickerColorSpace::Oklch:
                    ctx.text(u8"Oklch"_ustr, style);
                    break;
                case ColorPickerColorSpace::Count:
                    break;
            }
        }

        [[nodiscard]] inline UString picker_part_id(const UString &id, ColorPickerPart part) {
            const char *suffix = part == ColorPickerPart::SaturationValue ? "#sv"
                                 : part == ColorPickerPart::Hue           ? "#hue"
                                 : part == ColorPickerPart::Alpha         ? "#alpha"
                                                                          : "";
            return UString{id.cpp_string() + suffix};
        }

        [[nodiscard]] inline ColorPickerPart focused_picker_part(const Context &ctx, const UString &id, bool show_sv, bool show_hue, bool show_alpha) noexcept {
            if (show_sv && ctx.has_focus(picker_part_id(id, ColorPickerPart::SaturationValue)))
                return ColorPickerPart::SaturationValue;
            if (show_hue && ctx.has_focus(picker_part_id(id, ColorPickerPart::Hue)))
                return ColorPickerPart::Hue;
            if (show_alpha && ctx.has_focus(picker_part_id(id, ColorPickerPart::Alpha)))
                return ColorPickerPart::Alpha;
            return ColorPickerPart::None;
        }

    } // namespace Detail

    // `id` is the stable identity of the composite picker; `decl` controls the outer container while
    // plane/bar dimensions come from style. The returned full RGBA color is always populated; hiding
    // alpha preserves the incoming alpha instead of forcing opacity.
    [[nodiscard]] inline ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input, bool enabled, const ColorPickerComposition &composition) {
        bool &initialized = DetailColorPickerAccess::initialized(state);
        f64 &hue = DetailColorPickerAccess::hue(state);
        f64 &saturation = DetailColorPickerAccess::saturation(state);
        f64 &brightness = DetailColorPickerAccess::value(state);
        f64 &alpha = DetailColorPickerAccess::alpha(state);
        Color &last_output = DetailColorPickerAccess::last_output(state);
        ColorPickerPart &active = DetailColorPickerAccess::active(state);
        bool &gesture_changed = DetailColorPickerAccess::gesture_changed(state);
        ColorPickerColorSpace &color_space = DetailColorPickerAccess::color_space(state);
        DropdownState &color_space_dropdown = DetailColorPickerAccess::color_space_dropdown(state);

        if (input.requested_color_space.has_value() && *input.requested_color_space < ColorPickerColorSpace::Count) {
            color_space = *input.requested_color_space;
        }

        if (!initialized || !(color == last_output)) {
            const Detail::SrgbHsv external = Detail::srgb_to_picker_hsv(color);
            // Hue is undefined at zero chroma. Preserve the previous meaningful hue after initial
            // synchronization so grayscale/black edits do not unexpectedly reset it to red.
            if (!initialized || (external.s > 1.0e-8 && external.v > 1.0e-8)) {
                hue = external.h;
            }
            saturation = external.s;
            brightness = external.v;
            alpha = std::isfinite(color.a) ? std::clamp(color.a, 0.0, 1.0) : 1.0;
            initialized = true;
            last_output = color;
        }

        const UString sv_id = color_picker_part_id(id, ColorPickerVisualPart::SaturationValue);
        const UString hue_id = color_picker_part_id(id, ColorPickerVisualPart::Hue);
        const UString alpha_id = color_picker_part_id(id, ColorPickerVisualPart::Alpha);
        const UString color_space_id = color_picker_part_id(id, ColorPickerVisualPart::Dropdown);

        const bool root_visible = composition.root.visible;
        const bool picker_enabled = enabled && root_visible && composition.root.enabled;
        const bool show_sv = composition.saturation_value.visible;
        const bool show_hue = composition.hue.visible;
        const bool show_alpha = config.show_alpha && composition.alpha.visible;
        const bool show_dropdown = config.show_color_space_dropdown && composition.dropdown.visible;
        const bool show_preview = config.show_preview && composition.preview.visible;
        const bool sv_enabled = picker_enabled && show_sv && composition.saturation_value.enabled;
        const bool hue_enabled = picker_enabled && show_hue && composition.hue.enabled;
        const bool alpha_enabled = picker_enabled && show_alpha && composition.alpha.enabled;
        const bool dropdown_enabled = picker_enabled && show_dropdown && composition.dropdown.enabled;

        ColorPickerResult result;
        result.color_space = color_space;
        result.hovered = (sv_enabled && ctx.hovered(sv_id)) ||
                         (hue_enabled && ctx.hovered(hue_id)) ||
                         (alpha_enabled && ctx.hovered(alpha_id)) ||
                         (dropdown_enabled && ctx.hovered(color_space_id));

        const auto id_for_part = [&](ColorPickerPart part) -> const UString & {
            if (part == ColorPickerPart::SaturationValue)
                return sv_id;
            if (part == ColorPickerPart::Hue)
                return hue_id;
            return alpha_id;
        };

        const auto cancel_unavailable_part = [&](ColorPickerPart part, const UString &part_id, bool available) {
            if (available)
                return;
            ctx.clear_focus(part_id);
            if (active == part) {
                ctx.release_pointer(part_id);
                active = ColorPickerPart::None;
                gesture_changed = false;
                // Preserve the legacy whole-widget disabled behavior (capture is dropped without a
                // cancellation event); per-part visibility/enablement changes do report cancellation.
                result.cancelled = enabled;
            }
        };
        cancel_unavailable_part(ColorPickerPart::SaturationValue, sv_id, sv_enabled);
        cancel_unavailable_part(ColorPickerPart::Hue, hue_id, hue_enabled);
        cancel_unavailable_part(ColorPickerPart::Alpha, alpha_id, alpha_enabled);

        if (!picker_enabled || id.empty()) {
            if (active != ColorPickerPart::None) {
                ctx.release_pointer(id_for_part(active));
            }
            ctx.clear_focus(sv_id);
            ctx.clear_focus(hue_id);
            ctx.clear_focus(alpha_id);
            active = ColorPickerPart::None;
            gesture_changed = false;
        } else {
            if (input.request_blur) {
                ctx.clear_focus(sv_id);
                ctx.clear_focus(hue_id);
                ctx.clear_focus(alpha_id);
            }
            const auto part_is_enabled = [&](ColorPickerPart part) {
                return part == ColorPickerPart::SaturationValue ? sv_enabled
                       : part == ColorPickerPart::Hue           ? hue_enabled
                       : part == ColorPickerPart::Alpha         ? alpha_enabled
                                                                : false;
            };
            if (input.request_focus.has_value() && part_is_enabled(*input.request_focus)) {
                ctx.focus(id_for_part(*input.request_focus));
            }

            const auto begin_part = [&](ColorPickerPart part, const UString &part_id, bool part_enabled) {
                if (part_enabled && ctx.clicked(part_id) && ctx.element_bounds(part_id).has_value() && ctx.try_capture_pointer(part_id)) {
                    ctx.focus(part_id);
                    active = part;
                    gesture_changed = false;
                    return true;
                }
                return false;
            };
            (void)(begin_part(ColorPickerPart::SaturationValue, sv_id, sv_enabled) ||
                   begin_part(ColorPickerPart::Hue, hue_id, hue_enabled) ||
                   begin_part(ColorPickerPart::Alpha, alpha_id, alpha_enabled));

            if (active != ColorPickerPart::None) {
                const UString &active_id = id_for_part(active);
                if (!ctx.has_pointer_capture(active_id)) {
                    active = ColorPickerPart::None;
                    gesture_changed = false;
                } else if (ctx.pointer_cancelled_this_frame()) {
                    result.cancelled = true;
                    ctx.release_pointer(active_id);
                    active = ColorPickerPart::None;
                    gesture_changed = false;
                } else if (const std::optional<ElementBounds> bounds = ctx.element_bounds(active_id); bounds.has_value()) {
                    const f64 x = bounds->size.x > 0.0f
                                      ? std::clamp(static_cast<f64>((ctx.pointer_position().x - bounds->position.x) / bounds->size.x), 0.0, 1.0)
                                      : 0.0;
                    const f64 y = bounds->size.y > 0.0f
                                      ? std::clamp(static_cast<f64>((ctx.pointer_position().y - bounds->position.y) / bounds->size.y), 0.0, 1.0)
                                      : 0.0;
                    const f64 before_hue = hue;
                    const f64 before_saturation = saturation;
                    const f64 before_brightness = brightness;
                    const f64 before_alpha = alpha;
                    if (active == ColorPickerPart::SaturationValue) {
                        saturation = x;
                        brightness = 1.0 - y;
                    } else if (active == ColorPickerPart::Hue) {
                        hue = std::min(x * 360.0, std::nextafter(360.0, 0.0));
                    } else {
                        alpha = x;
                    }
                    const bool component_changed = active == ColorPickerPart::SaturationValue
                                                       ? saturation != before_saturation || brightness != before_brightness
                                                   : active == ColorPickerPart::Hue
                                                       ? hue != before_hue
                                                       : alpha != before_alpha;
                    if (component_changed) {
                        result.changed = true;
                        gesture_changed = true;
                    }
                }

                if (active != ColorPickerPart::None &&
                    (ctx.pointer_released_this_frame() || (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame()))) {
                    result.committed = gesture_changed;
                    ctx.release_pointer(id_for_part(active));
                    active = ColorPickerPart::None;
                    gesture_changed = false;
                }
            }

            ColorPickerPart focused = Detail::focused_picker_part(ctx, id, sv_enabled, hue_enabled, alpha_enabled);
            if (ctx.pointer_pressed_this_frame() && !result.hovered && active == ColorPickerPart::None) {
                if (focused != ColorPickerPart::None)
                    ctx.clear_focus(id_for_part(focused));
                focused = ColorPickerPart::None;
            }
            if (focused != ColorPickerPart::None && active == ColorPickerPart::None) {
                const f64 small = std::isfinite(config.keyboard_step) && config.keyboard_step > 0.0
                                      ? config.keyboard_step
                                      : 0.01;
                const f64 page = std::isfinite(config.page_step) && config.page_step > 0.0
                                     ? config.page_step
                                     : 0.1;
                for (ColorPickerKey key : input.keys) {
                    const f64 before_hue = hue;
                    const f64 before_saturation = saturation;
                    const f64 before_brightness = brightness;
                    const f64 before_alpha = alpha;
                    if (focused == ColorPickerPart::SaturationValue) {
                        switch (key) {
                            case ColorPickerKey::Left:
                                saturation -= small;
                                break;
                            case ColorPickerKey::Right:
                                saturation += small;
                                break;
                            case ColorPickerKey::Up:
                                brightness += small;
                                break;
                            case ColorPickerKey::Down:
                                brightness -= small;
                                break;
                            case ColorPickerKey::PageDecrement:
                                brightness -= page;
                                break;
                            case ColorPickerKey::PageIncrement:
                                brightness += page;
                                break;
                            case ColorPickerKey::Minimum:
                                saturation = 0.0;
                                brightness = 0.0;
                                break;
                            case ColorPickerKey::Maximum:
                                saturation = 1.0;
                                brightness = 1.0;
                                break;
                        }
                        saturation = std::clamp(saturation, 0.0, 1.0);
                        brightness = std::clamp(brightness, 0.0, 1.0);
                    } else if (focused == ColorPickerPart::Hue) {
                        const f64 degree_step = small * 360.0;
                        const f64 degree_page = page * 360.0;
                        switch (key) {
                            case ColorPickerKey::Left:
                            case ColorPickerKey::Down:
                                hue -= degree_step;
                                break;
                            case ColorPickerKey::Right:
                            case ColorPickerKey::Up:
                                hue += degree_step;
                                break;
                            case ColorPickerKey::PageDecrement:
                                hue -= degree_page;
                                break;
                            case ColorPickerKey::PageIncrement:
                                hue += degree_page;
                                break;
                            case ColorPickerKey::Minimum:
                                hue = 0.0;
                                break;
                            case ColorPickerKey::Maximum:
                                hue = std::nextafter(360.0, 0.0);
                                break;
                        }
                        hue = std::fmod(hue, 360.0);
                        if (hue < 0.0)
                            hue += 360.0;
                    } else {
                        switch (key) {
                            case ColorPickerKey::Left:
                            case ColorPickerKey::Down:
                                alpha -= small;
                                break;
                            case ColorPickerKey::Right:
                            case ColorPickerKey::Up:
                                alpha += small;
                                break;
                            case ColorPickerKey::PageDecrement:
                                alpha -= page;
                                break;
                            case ColorPickerKey::PageIncrement:
                                alpha += page;
                                break;
                            case ColorPickerKey::Minimum:
                                alpha = 0.0;
                                break;
                            case ColorPickerKey::Maximum:
                                alpha = 1.0;
                                break;
                        }
                        alpha = std::clamp(alpha, 0.0, 1.0);
                    }
                    const bool component_changed = focused == ColorPickerPart::SaturationValue
                                                       ? saturation != before_saturation || brightness != before_brightness
                                                   : focused == ColorPickerPart::Hue
                                                       ? hue != before_hue
                                                       : alpha != before_alpha;
                    if (component_changed) {
                        result.changed = true;
                        result.committed = true;
                    }
                }
            }
        }

        result.color = Detail::picker_hsv_to_srgb(hue, saturation, brightness, alpha);
        last_output = result.color;
        result.value = Detail::color_picker_value(result.color, color_space);
        result.color_space = color_space;
        result.active_part = active;
        result.focused_part = picker_enabled
                                  ? Detail::focused_picker_part(ctx, id, sv_enabled, hue_enabled, alpha_enabled)
                                  : ColorPickerPart::None;

        if (!root_visible) {
            result.color_space = color_space;
            result.value = Detail::color_picker_value(result.color, color_space);
            return result;
        }

        const auto make_context = [&](ColorPickerVisualPart part, const UString &part_id, const PartVisualState &visual, glm::vec2 normalized = glm::vec2{0.0f}, ColorPickerVisualPart target = ColorPickerVisualPart::Root) {
            return ColorPickerPartContext{
                .part = part,
                .target = target == ColorPickerVisualPart::Root ? part : target,
                .visual = visual,
                .id = part_id,
                .color = result.color,
                .color_space = color_space,
                .hue = hue,
                .saturation = saturation,
                .value = brightness,
                .alpha = alpha,
                .normalized_position = normalized,
                .bounds = ctx.element_bounds(part_id),
            };
        };
        const auto part_opacity = [&](bool part_enabled) {
            return part_enabled ? 1.0 : style.disabled_opacity;
        };

        PartVisualState root_visual{
            .enabled = picker_enabled,
            .hovered = result.hovered,
            .active = active != ColorPickerPart::None,
            .focused = result.focused_part != ColorPickerPart::None,
        };
        ColorPickerPartContext root_context = make_context(
            ColorPickerVisualPart::Root,
            id,
            root_visual);
        ElementDecl root = decl;
        root.direction = LayoutDirection::TopToBottom;
        root.child_gap = style.gap;
        if (!composition.root.render_default)
            clear_element_visual(root);
        apply_part_visual(root, composition.root.visual, root_visual);
        if (composition.root.alter_decl)
            composition.root.alter_decl(root, root_context);
        root.id = id;
        auto root_scope = ctx.element(root);
        (void)root_scope;

        const auto render_label = [&](ColorPickerVisualPart target, bool target_enabled) {
            if (!composition.label.visible)
                return;
            const UString label_id = color_picker_part_id(id, ColorPickerVisualPart::Label, target);
            PartVisualState visual{.enabled = picker_enabled && composition.label.enabled && target_enabled};
            ColorPickerPartContext label_context = make_context(
                ColorPickerVisualPart::Label,
                label_id,
                visual,
                glm::vec2{0.0f},
                target);
            ElementDecl label_decl{.sizing = {SizingAxis::fit(), SizingAxis::fit()}};
            if (!composition.label.render_default)
                clear_element_visual(label_decl);
            apply_part_visual(label_decl, composition.label.visual, visual);
            if (composition.label.alter_decl)
                composition.label.alter_decl(label_decl, label_context);
            label_decl.id = label_id;
            auto label_scope = ctx.element(label_decl);
            (void)label_scope;
            if (composition.label.build)
                composition.label.build(ctx, label_context);
        };

        if (show_dropdown) {
            render_label(ColorPickerVisualPart::Dropdown, dropdown_enabled);
            constexpr usize color_space_count = static_cast<usize>(ColorPickerColorSpace::Count);
            std::array<DropdownOption, color_space_count> options;
            for (usize i = 0; i < color_space_count; ++i) {
                const ColorPickerColorSpace option_space = static_cast<ColorPickerColorSpace>(i);
                options[i].build = [option_space, &style](Context &option_ctx) {
                    Detail::render_color_space_label(option_ctx, option_space, style.color_space_text);
                };
            }

            DropdownComposition dropdown_composition;
            dropdown_composition.trigger.enabled = composition.dropdown.enabled;
            dropdown_composition.trigger.render_default = composition.dropdown.render_default;
            dropdown_composition.trigger.visual = composition.dropdown.visual;
            dropdown_composition.trigger.alter_decl = [&](ElementDecl &dropdown_decl,
                                                          const DropdownPartContext &dropdown_context) {
                if (!composition.dropdown.alter_decl)
                    return;
                ColorPickerPartContext picker_context = make_context(
                    ColorPickerVisualPart::Dropdown,
                    color_space_id,
                    dropdown_context.visual);
                composition.dropdown.alter_decl(dropdown_decl, picker_context);
            };
            dropdown_composition.trigger.build = [&](Context &dropdown_ctx,
                                                     const DropdownPartContext &dropdown_context) {
                if (!composition.dropdown.build)
                    return;
                ColorPickerPartContext picker_context = make_context(
                    ColorPickerVisualPart::Dropdown,
                    color_space_id,
                    dropdown_context.visual);
                composition.dropdown.build(dropdown_ctx, picker_context);
            };

            const DropdownResult dropdown_result = dropdown(
                ctx,
                color_space_id,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(style.plane_size.x),
                               SizingAxis::fixed(style.color_space_dropdown_height)},
                    .padding = Padding::symmetric(10, 4),
                    .child_alignment = {AlignX::Left, AlignY::Center},
                },
                style.color_space_dropdown,
                color_space_dropdown,
                input.delta_seconds,
                static_cast<usize>(color_space),
                options,
                dropdown_enabled,
                dropdown_composition);
            if (dropdown_result.changed && dropdown_result.selected_index < color_space_count) {
                color_space = static_cast<ColorPickerColorSpace>(dropdown_result.selected_index);
                result.color_space = color_space;
                result.value = Detail::color_picker_value(result.color, color_space);
                result.color_space_changed = true;
                result.changed = true;
                result.committed = true;
            }
        } else {
            color_space_dropdown.open = false;
        }

        if (show_sv) {
            render_label(ColorPickerVisualPart::SaturationValue, sv_enabled);
            const PartVisualState plane_visual{
                .enabled = sv_enabled,
                .hovered = sv_enabled && ctx.hovered(sv_id),
                .pressed = sv_enabled && ctx.pointer_down(sv_id),
                .active = active == ColorPickerPart::SaturationValue,
                .focused = result.focused_part == ColorPickerPart::SaturationValue,
            };
            ColorPickerPartContext plane_context = make_context(
                ColorPickerVisualPart::SaturationValue,
                sv_id,
                plane_visual,
                glm::vec2{static_cast<f32>(saturation), static_cast<f32>(1.0 - brightness)});
            ElementDecl plane_decl{
                .sizing = {SizingAxis::fixed(style.plane_size.x), SizingAxis::fixed(style.plane_size.y)},
                .direction = LayoutDirection::TopToBottom,
                .border = plane_visual.focused ? style.focused_border : style.border,
            };
            if (!composition.saturation_value.render_default)
                clear_element_visual(plane_decl);
            apply_part_visual(plane_decl, composition.saturation_value.visual, plane_visual);
            if (composition.saturation_value.alter_decl)
                composition.saturation_value.alter_decl(plane_decl, plane_context);
            plane_decl.id = sv_id;
            auto plane = ctx.element(plane_decl);
            (void)plane;

            if (composition.saturation_value.render_default) {
                // Real per-pixel gradient (Shaders/ui_color_picker_sv_plane.slang) — smooth at any
                // size, one draw call instead of what used to be up to 32x32 flat-colored elements.
                auto gradient = ctx.custom_element(
                    ElementDecl{.sizing = {SizingAxis::fixed(style.plane_size.x), SizingAxis::fixed(style.plane_size.y)}},
                    Detail::sv_plane_shader(hue, part_opacity(sv_enabled), plane_decl.corner_radius));
                (void)gradient;
            }
            if (composition.saturation_value.build)
                composition.saturation_value.build(ctx, plane_context);

            if (composition.saturation_value_marker.visible) {
                const UString marker_id = color_picker_part_id(id, ColorPickerVisualPart::SaturationValueMarker);
                const bool marker_enabled = picker_enabled && composition.saturation_value_marker.enabled;
                PartVisualState marker_visual{.enabled = marker_enabled, .active = plane_visual.active};
                ColorPickerPartContext marker_context = make_context(
                    ColorPickerVisualPart::SaturationValueMarker,
                    marker_id,
                    marker_visual,
                    plane_context.normalized_position);
                const f32 cursor = std::max(style.plane_cursor_size, 2.0f);
                ElementDecl marker_decl{
                    .sizing = {SizingAxis::fixed(cursor), SizingAxis::fixed(cursor)},
                    .background_color = Color{0.0, 0.0, 0.0, 0.0},
                    .corner_radius = CornerRadius::all(cursor * 0.5f),
                    .border = BorderStyle{
                        .color = Detail::with_opacity(style.cursor_color, part_opacity(marker_enabled)),
                        .width = BorderWidth::all(2)},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::CenterCenter,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {static_cast<f32>(saturation) * style.plane_size.x, static_cast<f32>(1.0 - brightness) * style.plane_size.y},
                        .capture_pointer = false,
                    },
                };
                if (!composition.saturation_value_marker.render_default)
                    clear_element_visual(marker_decl);
                apply_part_visual(marker_decl, composition.saturation_value_marker.visual, marker_visual);
                if (composition.saturation_value_marker.alter_decl)
                    composition.saturation_value_marker.alter_decl(marker_decl, marker_context);
                marker_decl.id = marker_id;
                auto marker = ctx.element(marker_decl);
                (void)marker;
                if (composition.saturation_value_marker.build)
                    composition.saturation_value_marker.build(ctx, marker_context);
            }
        }

        const auto render_bar = [&](ColorPickerVisualPart part, ColorPickerVisualPart marker_part, const UString &part_id, const PartSlot<ColorPickerPartContext> &part_slot, const PartSlot<ColorPickerPartContext> &marker_slot, bool part_visible, bool part_enabled, ColorPickerPart interaction_part, f64 normalized, bool alpha_bar) {
            if (!part_visible)
                return;
            render_label(part, part_enabled);
            PartVisualState visual{
                .enabled = part_enabled,
                .hovered = part_enabled && ctx.hovered(part_id),
                .pressed = part_enabled && ctx.pointer_down(part_id),
                .active = active == interaction_part,
                .focused = result.focused_part == interaction_part,
            };
            ColorPickerPartContext part_context = make_context(
                part,
                part_id,
                visual,
                glm::vec2{static_cast<f32>(normalized), 0.5f});
            ElementDecl bar_decl{
                .sizing = {SizingAxis::fixed(style.plane_size.x), SizingAxis::fixed(style.bar_height)},
                .direction = LayoutDirection::LeftToRight,
                .border = visual.focused ? style.focused_border : style.border,
            };
            if (!part_slot.render_default)
                clear_element_visual(bar_decl);
            apply_part_visual(bar_decl, part_slot.visual, visual);
            if (part_slot.alter_decl)
                part_slot.alter_decl(bar_decl, part_context);
            bar_decl.id = part_id;
            auto bar = ctx.element(bar_decl);
            (void)bar;

            if (part_slot.render_default) {
                // Real per-pixel gradient (Shaders/ui_color_picker_hue_bar.slang /
                // ui_color_picker_alpha_bar.slang) — smooth at any width, one draw call instead of
                // what used to be up to 64 flat-colored segments.
                const f64 opacity = part_opacity(part_enabled);
                const CustomShaderRef shader =
                    alpha_bar ? Detail::alpha_bar_shader(Detail::picker_hsv_to_srgb(hue, saturation, brightness), opacity,
                                                         style.checker_light, style.checker_dark, bar_decl.corner_radius)
                              : Detail::hue_bar_shader(opacity, bar_decl.corner_radius);
                auto gradient = ctx.custom_element(
                    ElementDecl{.sizing = {SizingAxis::fixed(style.plane_size.x), SizingAxis::fixed(style.bar_height)}}, shader);
                (void)gradient;
            }
            if (part_slot.build)
                part_slot.build(ctx, part_context);

            if (marker_slot.visible) {
                const UString marker_id = color_picker_part_id(id, marker_part);
                const bool marker_enabled = picker_enabled && marker_slot.enabled;
                PartVisualState marker_visual{.enabled = marker_enabled, .active = visual.active};
                ColorPickerPartContext marker_context = make_context(
                    marker_part,
                    marker_id,
                    marker_visual,
                    glm::vec2{static_cast<f32>(normalized), 0.5f});
                ElementDecl marker_decl{
                    .sizing = {SizingAxis::fixed(style.bar_cursor_width), SizingAxis::fixed(style.bar_height + 4.0f)},
                    .background_color = Color{0.0, 0.0, 0.0, 0.0},
                    .border = BorderStyle{
                        .color = Detail::with_opacity(style.cursor_color, part_opacity(marker_enabled)),
                        .width = BorderWidth::all(1)},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::CenterCenter,
                        .parent_attach_point = FloatingAttachPoint::LeftCenter,
                        .offset = {static_cast<f32>(normalized) * style.plane_size.x, 0.0f},
                        .capture_pointer = false,
                    },
                };
                if (!marker_slot.render_default)
                    clear_element_visual(marker_decl);
                apply_part_visual(marker_decl, marker_slot.visual, marker_visual);
                if (marker_slot.alter_decl)
                    marker_slot.alter_decl(marker_decl, marker_context);
                marker_decl.id = marker_id;
                auto marker = ctx.element(marker_decl);
                (void)marker;
                if (marker_slot.build)
                    marker_slot.build(ctx, marker_context);
            }
        };

        render_bar(ColorPickerVisualPart::Hue, ColorPickerVisualPart::HueMarker, hue_id, composition.hue, composition.hue_marker, show_hue, hue_enabled, ColorPickerPart::Hue, hue / 360.0, false);
        render_bar(ColorPickerVisualPart::Alpha, ColorPickerVisualPart::AlphaMarker, alpha_id, composition.alpha, composition.alpha_marker, show_alpha, alpha_enabled, ColorPickerPart::Alpha, alpha, true);

        if (show_preview) {
            render_label(ColorPickerVisualPart::Preview, picker_enabled && composition.preview.enabled);
            const UString preview_id = color_picker_part_id(id, ColorPickerVisualPart::Preview);
            const bool preview_enabled = picker_enabled && composition.preview.enabled;
            PartVisualState preview_visual{.enabled = preview_enabled};
            ColorPickerPartContext preview_context = make_context(
                ColorPickerVisualPart::Preview,
                preview_id,
                preview_visual);
            ElementDecl preview_decl{
                .sizing = {SizingAxis::fixed(style.plane_size.x), SizingAxis::fixed(style.preview_height)},
                .background_color = Detail::with_opacity(
                    Detail::composite_opaque(result.color, style.checker_dark),
                    part_opacity(preview_enabled)),
                .border = style.border,
            };
            if (!composition.preview.render_default)
                clear_element_visual(preview_decl);
            apply_part_visual(preview_decl, composition.preview.visual, preview_visual);
            if (composition.preview.alter_decl)
                composition.preview.alter_decl(preview_decl, preview_context);
            preview_decl.id = preview_id;
            auto preview = ctx.element(preview_decl);
            (void)preview;
            if (composition.preview.build)
                composition.preview.build(ctx, preview_context);
        }

        ColorPickerVisualPart tooltip_target = ColorPickerVisualPart::Root;
        if (active == ColorPickerPart::SaturationValue)
            tooltip_target = ColorPickerVisualPart::SaturationValue;
        else if (active == ColorPickerPart::Hue)
            tooltip_target = ColorPickerVisualPart::Hue;
        else if (active == ColorPickerPart::Alpha)
            tooltip_target = ColorPickerVisualPart::Alpha;
        else if (sv_enabled && ctx.hovered(sv_id))
            tooltip_target = ColorPickerVisualPart::SaturationValue;
        else if (hue_enabled && ctx.hovered(hue_id))
            tooltip_target = ColorPickerVisualPart::Hue;
        else if (alpha_enabled && ctx.hovered(alpha_id))
            tooltip_target = ColorPickerVisualPart::Alpha;
        else if (dropdown_enabled && ctx.hovered(color_space_id))
            tooltip_target = ColorPickerVisualPart::Dropdown;
        else if (result.focused_part == ColorPickerPart::SaturationValue)
            tooltip_target = ColorPickerVisualPart::SaturationValue;
        else if (result.focused_part == ColorPickerPart::Hue)
            tooltip_target = ColorPickerVisualPart::Hue;
        else if (result.focused_part == ColorPickerPart::Alpha)
            tooltip_target = ColorPickerVisualPart::Alpha;

        if (tooltip_target != ColorPickerVisualPart::Root && composition.tooltip.visible &&
            composition.tooltip.build) {
            const UString tooltip_id = color_picker_part_id(id, ColorPickerVisualPart::Tooltip);
            const UString target_id = color_picker_part_id(id, tooltip_target);
            PartVisualState tooltip_visual{
                .enabled = picker_enabled && composition.tooltip.enabled,
                .hovered = result.hovered,
                .active = active != ColorPickerPart::None,
                .focused = result.focused_part != ColorPickerPart::None,
            };
            ColorPickerPartContext tooltip_context = make_context(
                ColorPickerVisualPart::Tooltip,
                tooltip_id,
                tooltip_visual,
                glm::vec2{0.0f},
                tooltip_target);
            ElementDecl tooltip_decl{
                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::ElementWithId,
                    .parent_id = target_id,
                    .element_attach_point = FloatingAttachPoint::LeftBottom,
                    .parent_attach_point = FloatingAttachPoint::LeftTop,
                    .offset = {0.0f, -4.0f},
                    .z_index = 100,
                    .capture_pointer = false,
                },
            };
            if (!composition.tooltip.render_default)
                clear_element_visual(tooltip_decl);
            apply_part_visual(tooltip_decl, composition.tooltip.visual, tooltip_visual);
            if (composition.tooltip.alter_decl)
                composition.tooltip.alter_decl(tooltip_decl, tooltip_context);
            tooltip_decl.id = tooltip_id;
            auto tooltip = ctx.element(tooltip_decl);
            (void)tooltip;
            composition.tooltip.build(ctx, tooltip_context);
        }

        if (composition.root.build) {
            root_context.color_space = color_space;
            composition.root.build(ctx, root_context);
        }
        // Keep the typed representation synchronized even when a programmatic selection changed
        // this frame while the dropdown was hidden.
        result.color_space = color_space;
        result.value = Detail::color_picker_value(result.color, color_space);
        return result;
    }

    [[nodiscard]] inline ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input = {}, bool enabled = true) {
        return color_picker(ctx, id, decl, config, style, state, color, input, enabled, ColorPickerComposition{});
    }

} // namespace SFT::UI
