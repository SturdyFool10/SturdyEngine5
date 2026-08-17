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


namespace SFT::UI {


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


    struct ColorPickerComponent {
        const char *label = "";
        f64 minimum = 0.0;
        f64 maximum = 1.0;
    };


    /// Performs the color picker components operation using the supplied arguments.
    ///
    /// @param color_space `color_space` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] span<const ColorPickerComponent> color_picker_components(ColorPickerColorSpace color_space) noexcept;

    /// Returns a human-readable name for the supplied color picker space value.
    ///
    /// @param color_space `color_space` value used by the operation.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *color_picker_space_name(ColorPickerColorSpace color_space) noexcept;


    /// Performs the color picker component values operation using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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


        optional<ColorPickerColorSpace> requested_color_space;

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


        DropdownStyle color_space_dropdown{};
        TextStyle color_space_text{.color = Color{0.92, 0.93, 0.95, 1.0}, .font_size = 14, .wrap_mode = TextWrapMode::None};
        f32 plane_cursor_size = 14.0f;
        f32 bar_cursor_width = 4.0f;
        Color cursor_color{1.0, 1.0, 1.0, 1.0};
        Color cursor_shadow{0.0, 0.0, 0.0, 0.85};


        Color checker_light{1.0, 1.0, 1.0, 1.0};
        Color checker_dark{0.78, 0.78, 0.78, 1.0};


        f32 checker_cell_size = 6.0f;
        BorderStyle border{.color = Color{0.08, 0.08, 0.10, 1.0}, .width = BorderWidth::all(1)};
        BorderStyle focused_border{.color = Color{0.55, 0.72, 1.0, 1.0}, .width = BorderWidth::all(2)};
        f64 disabled_opacity = 0.5;


        SliderStyle component_slider{
            .track{0.16, 0.17, 0.21, 1.0},
            .fill{0.35, 0.55, 0.85, 1.0},
            .track_thickness = 6.0f,


            .thumb_size = 5.0f,
            .focused_border = BorderStyle{},
        };


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

    /// Performs the color picker part ID operation using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param target `target` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString color_picker_part_id(const ustr &id, ColorPickerVisualPart part, ColorPickerVisualPart target = ColorPickerVisualPart::Root);

    /// Performs the color picker part ID operation using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param part `part` value used by the operation.
    /// @param target `target` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString color_picker_part_id(const UString &id, ColorPickerVisualPart part, ColorPickerVisualPart target = ColorPickerVisualPart::Root);

    struct ColorPickerPartContext {
        ColorPickerVisualPart part = ColorPickerVisualPart::Root;

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
        /// Returns the current or globally available active part value.
        ///
        /// @return Returns the current active part value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ColorPickerPart active_part() const noexcept;
        /// Returns the current or globally available color space value.
        ///
        /// @return Returns the current color space value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ColorPickerColorSpace color_space() const noexcept;
        /// Sets the color space for this `ColorPickerState`.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
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


        array<SliderState, 3> component_sliders_{};


        array<f64, 3> component_cache_{};
        Color component_cache_source_{};
        ColorPickerColorSpace component_cache_space_ = ColorPickerColorSpace::Count;
    };

    struct ColorPickerResult {


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
        /// Performs the initialized operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static bool &initialized(ColorPickerState &s) noexcept;
        /// Performs the hue operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static f64 &hue(ColorPickerState &s) noexcept;
        /// Performs the saturation operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static f64 &saturation(ColorPickerState &s) noexcept;
        /// Performs the value operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static f64 &value(ColorPickerState &s) noexcept;
        /// Performs the alpha operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static f64 &alpha(ColorPickerState &s) noexcept;
        /// Performs the last output operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static Color &last_output(ColorPickerState &s) noexcept;
        /// Performs the active operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static ColorPickerPart &active(ColorPickerState &s) noexcept;
        /// Performs the gesture changed operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static bool &gesture_changed(ColorPickerState &s) noexcept;
        /// Performs the color space operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static ColorPickerColorSpace &color_space(ColorPickerState &s) noexcept;
        /// Performs the color space dropdown operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static DropdownState &color_space_dropdown(ColorPickerState &s) noexcept;
        /// Performs the component sliders operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static array<SliderState, 3> &component_sliders(ColorPickerState &s) noexcept;
        /// Performs the component cache operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static array<f64, 3> &component_cache(ColorPickerState &s) noexcept;
        /// Performs the component cache source operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static Color &component_cache_source(ColorPickerState &s) noexcept;
        /// Performs the component cache space operation for `DetailColorPickerAccess` using the supplied arguments.
        ///
        /// @param s `s` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        static ColorPickerColorSpace &component_cache_space(ColorPickerState &s) noexcept;
    };

    namespace Detail {

        struct SrgbHsv {
            f64 h = 0.0;
            f64 s = 0.0;
            f64 v = 0.0;
        };

        /// Performs the sRGB to picker hsv operation using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SrgbHsv srgb_to_picker_hsv(const Color &color) noexcept;

        /// Performs the picker hsv to sRGB operation using the supplied arguments.
        ///
        /// @param hue `hue` value used by the operation.
        /// @param saturation `saturation` value used by the operation.
        /// @param value Value consumed by the operation.
        /// @param alpha `alpha` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color picker_hsv_to_srgb(f64 hue, f64 saturation, f64 value, f64 alpha = 1.0) noexcept;

        /// Returns a copy or derived value with opacity applied.
        ///
        /// @param color `color` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color with_opacity(Color color, f64 opacity) noexcept;

        /// Performs the composite opaque operation using the supplied arguments.
        ///
        /// @param foreground `foreground` value used by the operation.
        /// @param background `background` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color composite_opaque(const Color &foreground, const Color &background) noexcept;


        /// Packs gradient shader params using the supplied arguments and current state.
        ///
        /// @param vec4_fields `vec4_fields` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<std::byte> pack_gradient_shader_params(initializer_list<glm::vec4> vec4_fields);


        /// Performs the corner radius vec4 operation using the supplied arguments.
        ///
        /// @param radius `radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec4 corner_radius_vec4(const CornerRadius &radius) noexcept;


        /// Performs the sv plane shader operation using the supplied arguments.
        ///
        /// @param hue `hue` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        /// @param corner_radius `corner_radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CustomShaderRef sv_plane_shader(f64 hue, f64 opacity, const CornerRadius &corner_radius);


        /// Performs the hue bar shader operation using the supplied arguments.
        ///
        /// @param opacity `opacity` value used by the operation.
        /// @param corner_radius `corner_radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CustomShaderRef hue_bar_shader(f64 opacity, const CornerRadius &corner_radius);


        /// Performs the alpha bar shader operation using the supplied arguments.
        ///
        /// @param foreground `foreground` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        /// @param checker_light `checker_light` value used by the operation.
        /// @param checker_dark `checker_dark` value used by the operation.
        /// @param checker_cell_size Requested or available size for the operation.
        /// @param corner_radius `corner_radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CustomShaderRef alpha_bar_shader(const Color &foreground, f64 opacity, const Color &checker_light, const Color &checker_dark, f32 checker_cell_size, const CornerRadius &corner_radius);


        /// Performs the component bar shader operation using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        /// @param component_index Zero-based index of the target element or entry.
        /// @param component Component used or affected by the operation.
        /// @param values `values` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        /// @param corner_radius `corner_radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CustomShaderRef component_bar_shader(ColorPickerColorSpace color_space, usize component_index, const ColorPickerComponent &component, const array<f64, 3> &values, f64 opacity, const CornerRadius &corner_radius);


        /// Performs the preview shader operation using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        /// @param checker_light `checker_light` value used by the operation.
        /// @param checker_dark `checker_dark` value used by the operation.
        /// @param checker_cell_size Requested or available size for the operation.
        /// @param corner_radius `corner_radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CustomShaderRef preview_shader(const Color &color, f64 opacity, const Color &checker_light, const Color &checker_dark, f32 checker_cell_size, const CornerRadius &corner_radius);

        /// Performs the color picker value operation using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ColorPickerValue color_picker_value(const Color &color,
                                                                 ColorPickerColorSpace color_space) noexcept;


        /// Renders color space label using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param color_space `color_space` value used by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void render_color_space_label(Context &ctx, ColorPickerColorSpace color_space, const TextStyle &style);


        /// Performs the color from components operation using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        /// @param c0 `c0` value used by the operation.
        /// @param c1 `c1` value used by the operation.
        /// @param c2 `c2` value used by the operation.
        /// @param alpha `alpha` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Color color_from_components(ColorPickerColorSpace color_space, f64 c0, f64 c1, f64 c2, f64 alpha) noexcept;

        /// Performs the picker part ID operation using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        /// @param part `part` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString picker_part_id(const ustr &id, ColorPickerPart part);

        /// Performs the focused picker part operation using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param id Identifier of the target object or resource.
        /// @param show_sv `show_sv` value used by the operation.
        /// @param show_hue `show_hue` value used by the operation.
        /// @param show_alpha `show_alpha` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ColorPickerPart focused_picker_part(const Context &ctx, const ustr &id, bool show_sv, bool show_hue, bool show_alpha) noexcept;

    } // namespace Detail


    /// Performs the color picker operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param decl `decl` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param color `color` value used by the operation.
    /// @param input `input` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param composition `composition` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input, bool enabled, const ColorPickerComposition &composition);

    [[nodiscard]] ColorPickerResult color_picker(Context &ctx, const UString &id, const ElementDecl &decl, const ColorPickerConfig &config, const ColorPickerStyle &style, ColorPickerState &state, const Color &color, const ColorPickerInput &input = {}, bool enabled = true);

} // namespace SFT::UI
