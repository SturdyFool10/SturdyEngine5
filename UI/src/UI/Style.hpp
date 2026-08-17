#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#pragma endregion


namespace SFT::UI {


    using Color = Foundation::Color::Srgb;


    using EasingFn = f32 (*)(f32);


    enum class ColorBlendSpace : u8 { Oklab, Oklch, Linear, Srgb, Hsl };

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


    struct ClipConfig {
        bool horizontal = false;
        bool vertical = false;
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
        BorderStyle border{};
        ClipConfig clip{};
        FloatingConfig floating{};


        i32 z = 0;


        CursorIcon cursor = CursorIcon::Auto;


        UString debug_label;


        UString id;
    };


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

    struct TextStyle {
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;
        u16 font_size = 16;
        u16 letter_spacing = 0;
        u16 line_height = 0;
        TextWrapMode wrap_mode = TextWrapMode::Words;
        TextAlign alignment = TextAlign::Left;
    };

} // namespace SFT::UI
