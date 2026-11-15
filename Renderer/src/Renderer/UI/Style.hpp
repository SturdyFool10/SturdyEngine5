#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#pragma endregion

#include <Renderer/UI/UiColor.hpp>


namespace SFT::UI {


    /// A UI colour: non-linear sRGB with straight alpha, exactly `Foundation::Color::Srgb` (it *is* one, so everything that takes an `Srgb`
    /// takes this) plus construction from, and conversion to, the vector types shaders use. A `glm::vec3`/`glm::vec4` read as a colour
    /// here is sRGB-encoded, red/green/blue/alpha in 0..1, like a CSS hex colour; a vector in another space says so with a `ColorSpace`
    /// (UI/UiColor.hpp), which every API that accepts a colour as a vector also takes.
    struct Color : Foundation::Color::Srgb {
        constexpr Color() noexcept = default;
        constexpr Color(f64 red, f64 green, f64 blue, f64 alpha = 1.0) noexcept : Srgb{red, green, blue, alpha} {}
        constexpr Color(const Srgb &color) noexcept : Srgb{color} {} // NOLINT: deliberately implicit, Color is an Srgb
        constexpr Color(glm::vec3 rgb) noexcept : Srgb{rgb.x, rgb.y, rgb.z, 1.0} {}
        constexpr Color(glm::vec4 rgba) noexcept : Srgb{rgba.x, rgba.y, rgba.z, rgba.w} {}

        [[nodiscard]] constexpr glm::vec4 vec4() const noexcept {
            return glm::vec4{static_cast<f32>(r), static_cast<f32>(g), static_cast<f32>(b), static_cast<f32>(a)};
        }
        [[nodiscard]] constexpr glm::vec3 vec3() const noexcept { return glm::vec3{static_cast<f32>(r), static_cast<f32>(g), static_cast<f32>(b)}; }

        /// `Srgb::from_linear` returning a `Color`, which is what Foundation's `ColorSpace` concept asks of a colour type.
        [[nodiscard]] static Color from_linear(const Foundation::Color::Linear &color) noexcept { return Color{Srgb::from_linear(color)}; }

        friend constexpr bool operator==(const Color &, const Color &) noexcept = default;
        friend constexpr auto operator<=>(const Color &, const Color &) noexcept = default;
    };

    struct UiEffect; // UI/UiEffect.hpp


    using EasingFn = f32 (*)(f32);



    struct CornerRadius {
        f32 top_left = 0.0f;
        f32 top_right = 0.0f;
        f32 bottom_left = 0.0f;
        f32 bottom_right = 0.0f;

        /// Performs the all operation for `CornerRadius` using the supplied arguments.
        ///
        /// @param radius `radius` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr CornerRadius all(f32 radius) noexcept {
            return CornerRadius{radius, radius, radius, radius};
        }
    };

    /// The profile of one corner, after CSS `corner-shape`: a superellipse `|x|^n + |y|^n = 1` with `n = 2^k`, where `k` is the
    /// CSS `superellipse()` parameter. `k = 1` is the ordinary circular arc (`round`, what every corner was before this type
    /// existed), `k = 2` the smoother `squircle`, `k = 0` a straight diagonal `bevel`, `k -> +inf` a sharp `square` corner (the
    /// radius has no effect), and negative `k` mirrors the profile through the corner square so it curves *into* the box:
    /// `k = -1` is a circular `scoop`, `k -> -inf` a square `notch`. The size of the corner is still `CornerRadius`.
    struct CornerShape {
        f32 k = 1.0f;

        [[nodiscard]] static constexpr CornerShape round() noexcept { return CornerShape{1.0f}; }
        [[nodiscard]] static constexpr CornerShape squircle() noexcept { return CornerShape{2.0f}; }
        [[nodiscard]] static constexpr CornerShape bevel() noexcept { return CornerShape{0.0f}; }
        [[nodiscard]] static constexpr CornerShape scoop() noexcept { return CornerShape{-1.0f}; }
        [[nodiscard]] static constexpr CornerShape square() noexcept { return CornerShape{std::numeric_limits<f32>::infinity()}; }
        [[nodiscard]] static constexpr CornerShape notch() noexcept { return CornerShape{-std::numeric_limits<f32>::infinity()}; }
        /// Any CSS `superellipse(k)`.
        [[nodiscard]] static constexpr CornerShape superellipse(f32 k) noexcept { return CornerShape{k}; }

        friend constexpr bool operator==(const CornerShape &, const CornerShape &) noexcept = default;
    };

    /// One `CornerShape` per corner; value-initialised it is four round corners, i.e. an ordinary rounded rectangle.
    struct CornerShapes {
        CornerShape top_left{};
        CornerShape top_right{};
        CornerShape bottom_left{};
        CornerShape bottom_right{};

        [[nodiscard]] static constexpr CornerShapes all(CornerShape shape) noexcept {
            return CornerShapes{shape, shape, shape, shape};
        }

        friend constexpr bool operator==(const CornerShapes &, const CornerShapes &) noexcept = default;
    };

    /// One corner as the byte the renderer receives. `0` always means round (so zeroed memory is an ordinary rounded
    /// rectangle); other values map `k` through `t = 2/pi * atan(k)`, which squeezes the whole line, infinities included, into
    /// [-1, 1] with 126 steps, centred on bevel (`k = 0`) at 127. Square is 253, notch 1.
    [[nodiscard]] inline u8 encode_corner_shape(CornerShape shape) noexcept {
        if (std::isnan(shape.k)) {
            return 0;
        }
        const f32 t = std::isinf(shape.k) ? (shape.k > 0.0f ? 1.0f : -1.0f)
                                           : static_cast<f32>(std::atan(static_cast<double>(shape.k)) * (2.0 / std::numbers::pi));
        const i32 code = static_cast<i32>(std::lround(t * 126.0f)) + 127;
        // The code for exactly round (k = 1, t = 0.5, code 190) is the default byte.
        return code == 190 ? u8{0} : static_cast<u8>(std::clamp(code, 1, 253));
    }

    /// The `k` a byte from `encode_corner_shape` stands for (infinities for the two end codes). Used by tests; the shader has the
    /// same decode.
    [[nodiscard]] inline f32 decode_corner_shape_k(u8 code) noexcept {
        if (code == 0 || code == 190) {
            return 1.0f;
        }
        if (code >= 253) {
            return std::numeric_limits<f32>::infinity();
        }
        if (code <= 1) {
            return -std::numeric_limits<f32>::infinity();
        }
        const double t = (static_cast<double>(code) - 127.0) / 126.0;
        return static_cast<f32>(std::tan(t * std::numbers::pi * 0.5));
    }

    /// The four corners' bytes in one word: bits 0-7 top left, 8-15 top right, 16-23 bottom left, 24-31 bottom right.
    [[nodiscard]] inline u32 pack_corner_shapes(const CornerShapes &shapes) noexcept {
        return static_cast<u32>(encode_corner_shape(shapes.top_left)) | (static_cast<u32>(encode_corner_shape(shapes.top_right)) << 8u) |
               (static_cast<u32>(encode_corner_shape(shapes.bottom_left)) << 16u) |
               (static_cast<u32>(encode_corner_shape(shapes.bottom_right)) << 24u);
    }

    struct Padding {
        u16 left = 0;
        u16 right = 0;
        u16 top = 0;
        u16 bottom = 0;

        /// Performs the all operation for `Padding` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr Padding all(u16 value) noexcept { return Padding{value, value, value, value}; }
        /// Performs the symmetric operation for `Padding` using the supplied arguments.
        ///
        /// @param horizontal `horizontal` value used by the operation.
        /// @param vertical `vertical` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr Padding symmetric(u16 horizontal, u16 vertical) noexcept {
            return Padding{horizontal, horizontal, vertical, vertical};
        }
    };

    enum class SizingKind : u8 { Fit, Grow, Fixed, Percent };


    struct SizingAxis {
        SizingKind kind = SizingKind::Fit;
        f32 min = 0.0f;
        f32 max = 3.4028235e38f;
        f32 value = 0.0f;

        /// Performs the fit operation for `SizingAxis` using the supplied arguments.
        ///
        /// @param min_size Requested or available size for the operation.
        /// @param max_size Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr SizingAxis fit(f32 min_size = 0.0f, f32 max_size = 3.4028235e38f) noexcept {
            return SizingAxis{.kind = SizingKind::Fit, .min = min_size, .max = max_size};
        }
        /// Grows the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param min_size Requested or available size for the operation.
        /// @param max_size Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr SizingAxis grow(f32 min_size = 0.0f, f32 max_size = 3.4028235e38f) noexcept {
            return SizingAxis{.kind = SizingKind::Grow, .min = min_size, .max = max_size};
        }
        /// Performs the fixed operation for `SizingAxis` using the supplied arguments.
        ///
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr SizingAxis fixed(f32 size) noexcept {
            return SizingAxis{.kind = SizingKind::Fixed, .min = size, .max = size, .value = size};
        }
        /// Performs the percent operation for `SizingAxis` using the supplied arguments.
        ///
        /// @param fraction `fraction` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr SizingAxis percent(f32 fraction) noexcept {
            return SizingAxis{.kind = SizingKind::Percent, .value = fraction};
        }
    };

    struct Sizing {
        SizingAxis width{};
        SizingAxis height{};
    };

    enum class LayoutDirection : u8 { LeftToRight, TopToBottom };
    enum class AlignX : u8 { Left, Center, Right };
    enum class AlignY : u8 { Top, Center, Bottom };

    struct ChildAlignment {
        AlignX x = AlignX::Left;
        AlignY y = AlignY::Top;
    };

    struct BorderWidth {
        u16 left = 0;
        u16 right = 0;
        u16 top = 0;
        u16 bottom = 0;


        u16 between_children = 0;

        /// Performs the all operation for `BorderWidth` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr BorderWidth all(u16 value) noexcept {
            return BorderWidth{.left = value, .right = value, .top = value, .bottom = value};
        }
    };

    struct BorderStyle {
        Color color{0.0, 0.0, 0.0, 0.0};
        BorderWidth width{};
    };

    /// How one border edge is drawn (CSS `border-style`).
    enum class BorderLineStyle : u8 {
        /// A continuous line that follows the element's rounded or shaped corners.
        Solid,
        /// Dashes three widths long with gaps two widths long. Drawn as straight segments between the corners.
        Dashed,
        /// Round dots one width across, spaced two widths apart. Drawn as straight segments between the corners.
        Dotted,
        /// Two lines a third of the width each with a third between (a plain line under three units wide). Straight between the corners.
        Double,
    };

    /// One edge of a border: its own colour and style (the width is `ElementDecl::border.width`'s for that edge).
    struct BorderEdge {
        Color color{0.0, 0.0, 0.0, 0.0};
        BorderLineStyle style = BorderLineStyle::Solid;
    };

    /// A border whose edges differ (CSS `border-top-color`, `border-left-style`, ...). Set `ElementDecl::border_edges`; the widths still
    /// come from `ElementDecl::border.width` (which also drives layout), and `ElementDecl::border.color` is ignored while this is set.
    struct BorderEdges {
        BorderEdge left;
        BorderEdge right;
        BorderEdge top;
        BorderEdge bottom;

        [[nodiscard]] static BorderEdges all(BorderEdge edge) noexcept { return BorderEdges{edge, edge, edge, edge}; }
    };


    struct ClipConfig {
        bool horizontal = false;
        bool vertical = false;
        /// Clip to the element's own rounded, bevelled, scooped or notched corners (`corner_radius` / `corner_shapes`) instead of only its
        /// rectangle: CSS `overflow: hidden` with `border-radius`. Everything inside is cut by the shape, text and images included. Costs an
        /// off-screen pass for the element (like any effect), so use it where the corners matter.
        bool follow_corners = false;
        /// Clip to an arbitrary shape instead (an `Effects::clip_circle / clip_ellipse / clip_polygon / clip_inset` effect, or any effect whose
        /// last pass cuts the content): the element and everything in it is cut by it. Combines with `follow_corners` and with `ElementDecl::effect`
        /// (this one runs after it).
        std::shared_ptr<const UiEffect> path;
    };


    struct ScrollSettings {


        bool click_and_drag_scroll = false;


        bool smooth_scrolling = true;


        f32 smoothing_rate = 18.0f;


        f32 wheel_multiplier = 3.0f;
    };


    enum class FloatingAttachPoint : u8 { LeftTop, LeftCenter, LeftBottom, CenterTop, CenterCenter, CenterBottom, RightTop, RightCenter, RightBottom };

    enum class FloatingAttachTo : u8 {
        None,
        Parent,
        ElementWithId,
        Root,
    };


    enum class FloatingClipTo : u8 { None, AttachedParent };

    struct FloatingConfig {
        FloatingAttachTo attach_to = FloatingAttachTo::None;


        UString parent_id;
        FloatingAttachPoint element_attach_point = FloatingAttachPoint::LeftTop;
        FloatingAttachPoint parent_attach_point = FloatingAttachPoint::LeftBottom;
        glm::vec2 offset{0.0f};

        i16 z_index = 0;


        bool capture_pointer = true;

        FloatingClipTo clip_to = FloatingClipTo::None;
    };


    enum class CursorIcon : u8 {
        Auto,
        Default,
        Pointer,
        Text,
        Grab,
        Grabbing,
        ResizeHorizontal,
        ResizeVertical,
        ResizeNwse,
        ResizeNesw,
        NotAllowed,
    };


    struct ElementDecl {
        Sizing sizing{};
        Padding padding{};
        u16 child_gap = 0;
        ChildAlignment child_alignment{};
        LayoutDirection direction = LayoutDirection::LeftToRight;

        Color background_color{0.0, 0.0, 0.0, 0.0};
        CornerRadius corner_radius{};
        /// Profile of each corner (CSS `corner-shape`); round by default. The size still comes from `corner_radius`.
        CornerShapes corner_shapes{};
        BorderStyle border{};
        ClipConfig clip{};
        FloatingConfig floating{};


        i32 z = 0;


        CursorIcon cursor = CursorIcon::Auto;


        UString debug_label;


        UString id;

        /// Runs a shader effect over this element and everything inside it (UI/UiEffect.hpp). Null for none. Shared and
        /// read-only: build the effect once and point many elements, and many frames, at it.
        std::shared_ptr<const UiEffect> effect;
        /// Per-edge border colours and styles; null for the single `border.color` solid border. Shared and read-only like `effect`.
        std::shared_ptr<const BorderEdges> border_edges;
        /// Edge-aware layout: grows the padding just enough that a child's corner cannot poke out through this element's rounded, bevelled,
        /// scooped or notched corners (`corner_radius` and `corner_shapes`), so content is inset by the shape instead of overlapping it.
        /// The declared padding is a floor: the larger of the two wins on each side.
        bool inset_for_corners = false;
    };

    /// How far in from a corner of the given radius and profile a child's corner point must sit (on both axes) to lie inside the shape:
    /// 0.29 r for a round corner, r / 2 for a bevel, r for a notch, 0 for a square corner, 0.71 r for a circular scoop.
    [[nodiscard]] inline f32 corner_content_inset(f32 radius, CornerShape shape) noexcept {
        if (!(radius > 0.0f) || std::isnan(shape.k)) {
            return 0.0f;
        }
        if (shape.k >= 0.0f) {
            // Inside the superellipse |x|^n + |y|^n <= 1 with n = 2^k: the diagonal point (r - p)/r on both axes needs 2 * t^n <= 1.
            if (std::isinf(shape.k)) {
                return 0.0f;
            }
            const double n = std::exp2(static_cast<double>(shape.k));
            return static_cast<f32>(radius * (1.0 - std::pow(0.5, 1.0 / n)));
        }
        // A concave corner removes the superellipse |x|^m + |y|^m <= 1 (m = 2^-k) around the box corner; the child's corner must be outside it.
        if (std::isinf(shape.k)) {
            return radius;
        }
        const double m = std::exp2(static_cast<double>(-shape.k));
        return static_cast<f32>(radius * std::pow(0.5, 1.0 / m));
    }


    struct PaintKey {
        i32 z = 0;
        u32 order = 0;

        /// Implements `operator<` for `UI`.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        friend bool operator<(const PaintKey &lhs, const PaintKey &rhs) noexcept;
    };

    enum class TextWrapMode : u8 { Words, Newlines, None };
    enum class TextAlign : u8 { Left, Center, Right };


    using FontId = u16;

    /// Which lines a text decoration draws (CSS `text-decoration-line`); combine with `|`.
    enum class TextDecorationLine : u8 {
        None = 0,
        Underline = 1,
        Overline = 2,
        LineThrough = 4,
    };
    [[nodiscard]] constexpr TextDecorationLine operator|(TextDecorationLine a, TextDecorationLine b) noexcept {
        return static_cast<TextDecorationLine>(static_cast<u8>(a) | static_cast<u8>(b));
    }

    /// CSS `text-decoration`: lines drawn along each line of text.
    struct TextDecoration {
        TextDecorationLine lines = TextDecorationLine::None;
        /// The line's colour; the text's own when unset (CSS `text-decoration-color`).
        std::optional<Color> color{};
        /// Solid, dashed, dotted or double (CSS `text-decoration-style`).
        BorderLineStyle style = BorderLineStyle::Solid;
        /// Thickness in pixels; 0 picks one from the font size (CSS `text-decoration-thickness: auto`).
        f32 thickness = 0.0f;
    };

    /// CSS `text-shadow` without the blur: the text drawn a second time under itself, moved by `offset` pixels, in `color`. For a blurred
    /// shadow put the text in an element with `Effects::drop_shadow`.
    struct TextShadow {
        bool enabled = false;
        glm::vec2 offset{1.0f, 1.0f};
        Color color{0.0, 0.0, 0.0, 0.5};
    };

    struct TextStyle {
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;
        u16 font_size = 16;
        u16 letter_spacing = 0;
        u16 line_height = 0;
        TextWrapMode wrap_mode = TextWrapMode::Words;
        TextAlign alignment = TextAlign::Left;
        TextDecoration decoration{};
        TextShadow shadow{};
    };

} // namespace SFT::UI

/// `UI::Color` is an `Srgb` with vector conversions, so Foundation's colour concepts (`convert_to`, the blend helpers) treat it as the sRGB
/// colour it is. The specialisation has to live outside `SFT::UI`.
template <>
struct SFT::Foundation::Color::Detail::ColorTraits<SFT::UI::Color> {
    static constexpr bool is_color = true;
    [[nodiscard]] static constexpr std::array<SFT::f64, 4> components(const SFT::UI::Color &c) noexcept { return {c.r, c.g, c.b, c.a}; }
    [[nodiscard]] static constexpr SFT::UI::Color from_components(const std::array<SFT::f64, 4> &v) noexcept {
        return SFT::UI::Color{v[0], v[1], v[2], v[3]};
    }
};
