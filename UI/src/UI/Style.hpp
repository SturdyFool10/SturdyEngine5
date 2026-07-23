#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#pragma endregion

// Plain, composable style/layout structs — deliberately no CSS-like cascade or theme engine (Clay
// itself has none either). "Stylable" here means: app code composes its own theme as ordinary
// values/functions returning these structs, the same way the rest of this codebase avoids
// speculative abstraction (see e.g. Renderer::MaterialParameter, RenderGraphColorAttachmentDesc).
//
// Every field here mirrors a Clay counterpart 1:1 (see Context.cpp's to_clay_declaration()/
// to_clay_text_config()) — nothing Clay supports is hidden behind this wrapper, which is what
// keeps the framework unrestrictive rather than a curated subset.
namespace SFT::UI {

    // 0-255 per channel, matching Clay_Color's own convention.
    struct Color {
        f32 r = 0.0f;
        f32 g = 0.0f;
        f32 b = 0.0f;
        f32 a = 255.0f;

        [[nodiscard]] friend constexpr bool operator==(const Color &, const Color &) noexcept = default;
    };

    struct CornerRadius {
        f32 top_left = 0.0f;
        f32 top_right = 0.0f;
        f32 bottom_left = 0.0f;
        f32 bottom_right = 0.0f;

        [[nodiscard]] static constexpr CornerRadius all(f32 radius) noexcept {
            return CornerRadius{radius, radius, radius, radius};
        }
    };

    struct Padding {
        u16 left = 0;
        u16 right = 0;
        u16 top = 0;
        u16 bottom = 0;

        [[nodiscard]] static constexpr Padding all(u16 value) noexcept { return Padding{value, value, value, value}; }
        [[nodiscard]] static constexpr Padding symmetric(u16 horizontal, u16 vertical) noexcept {
            return Padding{horizontal, horizontal, vertical, vertical};
        }
    };

    enum class SizingKind : u8 { Fit, Grow, Fixed, Percent };

    // One axis of an element's sizing. `min`/`max` bound Fit/Grow; `value` holds the Fixed size or
    // the Percent fraction (0-1), depending on `kind`.
    struct SizingAxis {
        SizingKind kind = SizingKind::Fit;
        f32 min = 0.0f;
        f32 max = 3.4028235e38f; // FLT_MAX — Clay's own "no cap" convention for FIT/GROW.
        f32 value = 0.0f;

        [[nodiscard]] static constexpr SizingAxis fit(f32 min_size = 0.0f, f32 max_size = 3.4028235e38f) noexcept {
            return SizingAxis{.kind = SizingKind::Fit, .min = min_size, .max = max_size};
        }
        [[nodiscard]] static constexpr SizingAxis grow(f32 min_size = 0.0f, f32 max_size = 3.4028235e38f) noexcept {
            return SizingAxis{.kind = SizingKind::Grow, .min = min_size, .max = max_size};
        }
        [[nodiscard]] static constexpr SizingAxis fixed(f32 size) noexcept {
            return SizingAxis{.kind = SizingKind::Fixed, .min = size, .max = size, .value = size};
        }
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
        // Draws borders between children instead of around the element — see Clay_BorderWidth's
        // own doc comment (vertical lines for LeftToRight layout, horizontal for TopToBottom).
        u16 between_children = 0;

        [[nodiscard]] static constexpr BorderWidth all(u16 value) noexcept {
            return BorderWidth{.left = value, .right = value, .top = value, .bottom = value};
        }
    };

    struct BorderStyle {
        Color color{};
        BorderWidth width{};
    };

    // Clipping/scroll behavior. `child_offset` is subtracted from child positions each frame — the
    // caller (or a future scroll-input system, see plans/clay-ui-renderer.md Phase 2) owns updating it.
    struct ClipConfig {
        bool horizontal = false;
        bool vertical = false;
        glm::vec2 child_offset{0.0f};
    };

    // Full layout + visual config for one element — mirrors Clay_LayoutConfig plus the top-level
    // visual fields of Clay_ElementDeclaration.
    struct ElementDecl {
        Sizing sizing{};
        Padding padding{};
        u16 child_gap = 0;
        ChildAlignment child_alignment{};
        LayoutDirection direction = LayoutDirection::LeftToRight;

        Color background_color{};
        CornerRadius corner_radius{};
        BorderStyle border{};
        ClipConfig clip{};

        // Debug/tooling label only — never shown, never hashed into layout.
        UString debug_label;

        // Stable interaction identity: set this on anything that should answer
        // UI::Context::hovered(id)/clicked(id) queries (see Context.hpp). Must stay the same string
        // across frames for the same logical widget — hit-testing hashes it the same way Clay's own
        // CLAY_ID() macro would. Leave empty for purely decorative elements; they simply won't be
        // hit-testable by id (Clay's own anonymous-element hover still works internally, but this
        // wrapper doesn't expose that path — see Context::hovered(id)'s doc comment).
        UString id;
    };

    enum class TextWrapMode : u8 { Words, Newlines, None };
    enum class TextAlign : u8 { Left, Center, Right };

    // Caller-assigned opaque font identity, registered via UI::Context::register_font() and
    // referenced from TextStyle::font_id — mirrors Clay_TextElementConfig::fontId.
    using FontId = u16;

    struct TextStyle {
        Color color{255.0f, 255.0f, 255.0f, 255.0f};
        FontId font_id = 0;
        u16 font_size = 16;
        u16 letter_spacing = 0;
        u16 line_height = 0; // 0 = derive from the font's own metrics.
        TextWrapMode wrap_mode = TextWrapMode::Words;
        TextAlign alignment = TextAlign::Left;
    };

} // namespace SFT::UI
