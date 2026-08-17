#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#pragma endregion

#include "Context.hpp"
#include "Dropdown.hpp"
#include "Slider.hpp"
#include "Style.hpp"
#include "WidgetComposition.hpp"

using std::array;
using std::initializer_list;
using std::optional;
using std::span;
using std::string_view;
using std::variant;

/// Engine-native color picker rendered entirely through ordinary UI quads: a conventional sRGB-HSV
/// saturation/value plane, hue strip, optional alpha strip over a checkerboard, preview swatch, and
/// a dropdown selecting any color space Foundation defines. Persistent state retains the last
/// meaningful hue through grayscale/black, avoiding the common "desaturate blue, then resaturate and
/// get red" bug. No platform dialog or DOM is involved.
namespace SFT::UI {

    /// Runtime counterpart to Foundation::Color's concrete compile-time color types. Values stay in
    /// the same order as the dropdown and ColorPickerValue variant below.
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

    using ColorPickerValue = variant<
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

    /// One editable channel of a color space: its display label and slider range. Ranges are the
    /// conventional presentation ranges for each space (hue in degrees, CIELAB L in 0..100, Oklab
    /// a/b in roughly ±0.4, ...), not hard mathematical bounds — a value driven out of the sRGB
    /// gamut simply clamps when converted back, same as every browser picker.
    struct ColorPickerComponent {
        const char *label = "";
        f64 minimum = 0.0;
        f64 maximum = 1.0;
    };

    /// The three non-alpha channels of `color_space`, in the same order as the corresponding
    /// Foundation struct's own fields (alpha always exists and is handled by the picker's alpha bar,
    /// so it is deliberately not in this list).
    [[nodiscard]] span<const ColorPickerComponent> color_picker_components(ColorPickerColorSpace color_space) noexcept;

    [[nodiscard]] const char *color_picker_space_name(ColorPickerColorSpace color_space) noexcept;

    /// The typed value's channels flattened to {component0, component1, component2, alpha}, in the
    /// same order color_picker_components() describes them.
    [[nodiscard]] array<f64, 4> color_picker_component_values(const ColorPickerValue &value) noexcept;

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
        span<const ColorPickerKey> keys{};
        optional<ColorPickerPart> request_focus;
        /// Programmatic selection, useful for restoring serialized editor state. It does not emit a
        /// user-change event; selecting an entry from the dropdown does.
        optional<ColorPickerColorSpace> requested_color_space;
        /// Drives the dropdown trigger's ButtonStyle transition.
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
        /// color_space_text.font_id and color_space_dropdown.arrow_font_id must reference registered
        /// fonts, following DropdownStyle's normal text/arrow contract.
        DropdownStyle color_space_dropdown{};
        TextStyle color_space_text{.color = Color{0.92, 0.93, 0.95, 1.0}, .font_size = 14, .wrap_mode = TextWrapMode::None};
        f32 plane_cursor_size = 14.0f;
        f32 bar_cursor_width = 4.0f;
        Color cursor_color{1.0, 1.0, 1.0, 1.0};
        Color cursor_shadow{0.0, 0.0, 0.0, 0.85};
        /// The transparency checkerboard behind the alpha bar and the preview swatch: white and
        /// grey, the near-universal convention every image editor and browser picker shares — a
        /// transparent color should read as "over the checker", not as some flat composite.
        Color checker_light{1.0, 1.0, 1.0, 1.0};
        Color checker_dark{0.78, 0.78, 0.78, 1.0};
        /// Checker cell edge in pixels — a fixed fine grain, deliberately not scaled to the bar or
        /// preview's own height (which made big chunky stripes at typical control sizes).
        f32 checker_cell_size = 6.0f;
        BorderStyle border{.color = Color{0.08, 0.08, 0.10, 1.0}, .width = BorderWidth::all(1)};
        BorderStyle focused_border{.color = Color{0.55, 0.72, 1.0, 1.0}, .width = BorderWidth::all(2)};
        f64 disabled_opacity = 0.5;
        /// The per-channel component sliders shown when the color-space dropdown is enabled (they
        /// replace the fixed hue bar — see color_picker()'s own doc comment). Deliberately compact
        /// defaults so three of them stack into roughly the vertical space the hue bar occupied.
        SliderStyle component_slider{
            .track{0.16, 0.17, 0.21, 1.0},
            .fill{0.35, 0.55, 0.85, 1.0},
            .track_thickness = 6.0f,
            /// Matches bar_cursor_width-ish: thumb_size drives the slider's end-inset travel math,
            /// and the gradient track spans the full width — a small thumb keeps the cursor line's
            /// position and the gradient's value-at-that-pixel in agreement to within ~2px.
            .thumb_size = 5.0f,
            .focused_border = BorderStyle{},
        };
        /// Fixed label column ("R"/"G"/"B", "L"/"C"/"H", ...) to the left of each component slider,
        /// so the sliders themselves stay vertically aligned regardless of glyph width.
        f32 component_label_width = 18.0f;
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

    [[nodiscard]] UString color_picker_part_id(const ustr &id, ColorPickerVisualPart part, ColorPickerVisualPart target = ColorPickerVisualPart::Root);

    [[nodiscard]] UString color_picker_part_id(const UString &id, ColorPickerVisualPart part, ColorPickerVisualPart target = ColorPickerVisualPart::Root);

    struct ColorPickerPartContext {
        ColorPickerVisualPart part = ColorPickerVisualPart::Root;
        /// For Label this identifies the control being labeled; otherwise it equals part.
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
        optional<ElementBounds> bounds;
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
        [[nodiscard]] ColorPickerPart active_part() const noexcept;
        [[nodiscard]] ColorPickerColorSpace color_space() const noexcept;
        void set_color_space(ColorPickerColorSpace color_space) noexcept;

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
        /// One per component slider (see ColorPickerStyle::component_slider) — indices match
        /// color_picker_components()' order for whichever space is currently selected.
        array<SliderState, 3> component_sliders_{};
        /// The user's authoritative typed-component values, kept alongside the sRGB color they
        /// produced. Re-deriving slider positions from the round-tripped color every frame snaps
        /// them around badly in spaces the sRGB round trip is lossy over: hue resets to 0 the moment
        /// chroma hits zero (Oklch/LCh/HSL/...), and a chroma dragged past the sRGB gamut clamps and
        /// yanks the slider back on release. As long as the picker's color still equals what these
        /// components produced (component_cache_source_), the sliders display these values verbatim;
        /// any outside change (plane drag, external color, space switch) invalidates the cache and
        /// re-derives. Space initialized to Count = "no cache yet".
        array<f64, 3> component_cache_{};
        Color component_cache_source_{};
        ColorPickerColorSpace component_cache_space_ = ColorPickerColorSpace::Count;
    };

    struct ColorPickerResult {
        /// Render-ready engine color plus the same color converted to the dropdown's selected
        /// Foundation type. Use std::get<Foundation::Color::Oklch>(value), etc. when typed channels
        /// are needed without repeating a conversion at every call site.
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
        static bool &initialized(ColorPickerState &s) noexcept;
        static f64 &hue(ColorPickerState &s) noexcept;
        static f64 &saturation(ColorPickerState &s) noexcept;
        static f64 &value(ColorPickerState &s) noexcept;
        static f64 &alpha(ColorPickerState &s) noexcept;
        static Color &last_output(ColorPickerState &s) noexcept;
        static ColorPickerPart &active(ColorPickerState &s) noexcept;
        static bool &gesture_changed(ColorPickerState &s) noexcept;
        static ColorPickerColorSpace &color_space(ColorPickerState &s) noexcept;
        static DropdownState &color_space_dropdown(ColorPickerState &s) noexcept;
        static array<SliderState, 3> &component_sliders(ColorPickerState &s) noexcept;
        static array<f64, 3> &component_cache(ColorPickerState &s) noexcept;
        static Color &component_cache_source(ColorPickerState &s) noexcept;
        static ColorPickerColorSpace &component_cache_space(ColorPickerState &s) noexcept;
    };

    namespace Detail {

        struct SrgbHsv {
            f64 h = 0.0;
            f64 s = 0.0;
            f64 v = 0.0;
        };

        [[nodiscard]] SrgbHsv srgb_to_picker_hsv(const Color &color) noexcept;

        [[nodiscard]] Color picker_hsv_to_srgb(f64 hue, f64 saturation, f64 value, f64 alpha = 1.0) noexcept;

        [[nodiscard]] Color with_opacity(Color color, f64 opacity) noexcept;

        [[nodiscard]] Color composite_opaque(const Color &foreground, const Color &background) noexcept;

        /// Builds a custom_element() push-constant payload: each vec4 packed contiguously, starting
        /// immediately at byte 32 — UiElementConstants' prefix is a clean 32 bytes (two full 16-byte
        /// cbuffer registers, see its own doc comment), so unlike the legacy 24-byte prefix this needs
        /// no manual gap before the first trailing float4 field. Every Shaders/ui_color_picker_*.slang
        /// shader below declares its extra fields as plain float4s, so this one packer covers all of
        /// them.
        [[nodiscard]] vector<std::byte> pack_gradient_shader_params(initializer_list<glm::vec4> vec4_fields);

        /// (topLeft, topRight, bottomLeft, bottomRight) — matches Style.hpp's CornerRadius field
        /// order and roundedBoxSDF's own expected layout (sturdy_common.slang).
        [[nodiscard]] glm::vec4 corner_radius_vec4(const CornerRadius &radius) noexcept;

        /// Per-pixel saturation/value gradient (Shaders/ui_color_picker_sv_plane.slang) — replaces
        /// the old up-to-32x32 grid of flat-colored elements with one real shader draw. `corner_radius`
        /// should be whatever the surrounding container's own (possibly app-customized via
        /// ColorPickerComposition::saturation_value.alter_decl) ElementDecl::corner_radius resolved
        /// to, so the gradient's own rounding always matches its container's border exactly.
        [[nodiscard]] CustomShaderRef sv_plane_shader(f64 hue, f64 opacity, const CornerRadius &corner_radius);

        /// Per-pixel hue spectrum gradient (Shaders/ui_color_picker_hue_bar.slang) — replaces the
        /// old up-to-64-segment strip of flat colors with one real shader draw.
        [[nodiscard]] CustomShaderRef hue_bar_shader(f64 opacity, const CornerRadius &corner_radius);

        /// Per-pixel alpha gradient over a checkerboard (Shaders/ui_color_picker_alpha_bar.slang) —
        /// replaces the old flat-segment-over-checker-cell approximation with one real shader draw.
        [[nodiscard]] CustomShaderRef alpha_bar_shader(const Color &foreground, f64 opacity, const Color &checker_light, const Color &checker_dark, f32 checker_cell_size, const CornerRadius &corner_radius);

        /// One component slider's gradient track (Shaders/ui_color_picker_component_bar.slang):
        /// sweeps `component_index` across [minimum, maximum] with the other two channels held at
        /// `values` — the exact "this slider is the independent variable" preview, converted
        /// per-pixel through sturdy_common's GPU mirror of Foundation::Color.
        [[nodiscard]] CustomShaderRef component_bar_shader(ColorPickerColorSpace color_space, usize component_index, const ColorPickerComponent &component, const array<f64, 3> &values, f64 opacity, const CornerRadius &corner_radius);

        /// The preview swatch: the picked color at its *actual* alpha over a checkerboard
        /// (Shaders/ui_color_picker_preview.slang) — replaces the old flat composite_opaque()
        /// against a single checker color, which erased the transparency cue entirely.
        [[nodiscard]] CustomShaderRef preview_shader(const Color &color, f64 opacity, const Color &checker_light, const Color &checker_dark, f32 checker_cell_size, const CornerRadius &corner_radius);

        [[nodiscard]] ColorPickerValue color_picker_value(const Color &color,
                                                                 ColorPickerColorSpace color_space) noexcept;

        /// The dropdown row's display text — the same names color_picker_space_name() reports, so
        /// the trigger, the option list, and any app-side readout can never drift apart.
        void render_color_space_label(Context &ctx, ColorPickerColorSpace color_space, const TextStyle &style);

        /// Rebuilds an sRGB Color from the three color_picker_components() channels of `color_space`
        /// plus `alpha` — the write-side inverse of color_picker_component_values(). Out-of-gamut
        /// combinations clamp on conversion, same as any CSS color() rule would.
        [[nodiscard]] Color color_from_components(ColorPickerColorSpace color_space, f64 c0, f64 c1, f64 c2, f64 alpha) noexcept;

        [[nodiscard]] UString picker_part_id(const ustr &id, ColorPickerPart part);

        [[nodiscard]] ColorPickerPart focused_picker_part(const Context &ctx, const ustr &id, bool show_sv, bool show_hue, bool show_alpha) noexcept;

    } // namespace Detail

    /// `id` is the stable identity of the composite picker; `decl` controls the outer container while
    /// plane/bar dimensions come from style. The returned full RGBA color is always populated; hiding
    /// alpha preserves the incoming alpha instead of forcing opacity.
    [[nodiscard]] ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input, bool enabled, const ColorPickerComposition &composition);

    [[nodiscard]] ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input = {}, bool enabled = true);

} // namespace SFT::UI
