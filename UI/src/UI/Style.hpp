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

    // Sturdy.UI colors are Foundation::Color::Srgb (0-1 per channel, including alpha) — the same
    // engine-wide color toolkit the rest of Sturdy uses (Foundation/src/Color.hpp), not a bespoke
    // UI-only type. Author in whatever space reads best for the palette at hand — a plain literal,
    // or Foundation::Color::convert_to<Foundation::Color::Srgb>(someOklchValue) — since every
    // Foundation::Color type converts to Srgb uniformly, and Foundation::Color::lerp/mix work across
    // any two color spaces (see UI::blend_toward_srgb in Button.hpp for why a hover/press transition
    // specifically blends in Oklab rather than here in Srgb: interpolating straight in Srgb — or
    // even linear RGB — passes through a muddy, desaturated midpoint Oklab is designed to avoid).
    // Clay itself never interprets color data (it only carries Clay_Color through to render
    // commands unopened), so there's no Clay-convention mismatch in choosing our own representation;
    // ContextImpl.cpp's to_clay_color()/clay_color_to_linear() are the only two places that pack/
    // unpack it, converting to linear right before a value reaches the GPU.
    //
    // Unlike Srgb's own default (alpha=1, opaque — the sensible default for general color math),
    // every field below that uses this type defaults to fully transparent (alpha=0) where "the
    // caller didn't set a color" should mean invisible, not an opaque black rectangle — each such
    // field sets its own {0,0,0,0} initializer explicitly rather than relying on Color{}.
    using Color = Foundation::Color::Srgb;

    // Custom easing hook for the state-transition helpers in Button.hpp/Toggle.hpp/TextEdit.hpp:
    // takes how far a transition has progressed (0..1 — elapsed time divided by that transition's
    // own `transition_seconds`, clamped, see Button.hpp's ColorTransition/Detail::eased_progress)
    // and returns a reshaped progress fraction. nullptr (every style's default) leaves it unchanged.
    // Deliberately not clamped to 0..1 on the way back out — a back/elastic curve overshooting past
    // the target before settling is a legitimate use of this hook, not a bug.
    using EasingFn = f32 (*)(f32);

    // Which Foundation::Color space a state-transition color blend (Button.hpp's blend_toward,
    // Toggle.hpp's ToggleState::update) interpolates through before converting back to UI::Color
    // (Srgb) for storage/drawing. Defaults to Oklab (see blend_toward's own doc comment for why
    // perceptual uniformity is usually what you want); exposed as a knob because not every
    // transition wants that — e.g. Hsl is what you'd reach for to cycle straight through hue for a
    // rainbow effect, which is exactly what Oklab's uniform-lightness/chroma assumption would fight.
    enum class ColorBlendSpace : u8 { Oklab, Oklch, Linear, Srgb, Hsl };

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
        Color color{0.0, 0.0, 0.0, 0.0};
        BorderWidth width{};
    };

    // Clipping/scroll behavior. When either axis is enabled, Context::element() automatically wires
    // this element up as a Clay scroll container: child positions are offset by however far the
    // mouse wheel has scrolled it (fed through Context::begin_layout()'s PointerState::scroll_delta),
    // clamped to content size by Clay itself. Nothing here needs to be set by the caller.
    struct ClipConfig {
        bool horizontal = false;
        bool vertical = false;
    };

    // Which of a floating element's own corners, and which of its attachment target's corners, get
    // pinned together — mirrors Clay_FloatingAttachPointType exactly (see FloatingConfig's own doc
    // comment for the common case: a dropdown's open list wants ElementAttachPoint::LeftTop pinned
    // to ParentAttachPoint::LeftBottom, i.e. "hang the list below the trigger").
    enum class FloatingAttachPoint : u8 { LeftTop, LeftCenter, LeftBottom, CenterTop, CenterCenter, CenterBottom, RightTop, RightCenter, RightBottom };

    enum class FloatingAttachTo : u8 {
        None,          // (default) not floating.
        Parent,        // Attach to whatever element() call this one is nested inside.
        ElementWithId, // Attach to whichever element in the tree declared ElementDecl::id == parent_id (see FloatingConfig::parent_id).
        Root,          // Attach to the layout root — offset alone gives absolute-positioning-like placement.
    };

    // A floating element renders on top of (and doesn't affect the layout of) whatever it's
    // attached to — Clay's native mechanism for anything that needs to visually escape its parent's
    // layout box: a dropdown's open option list, a tooltip, a popover. Mirrors
    // Clay_FloatingElementConfig field-for-field; see clay.h's own doc comments on each field for
    // the full behavior (this wrapper doesn't curate a subset — same "unrestrictive" policy as the
    // rest of Style.hpp).
    struct FloatingConfig {
        FloatingAttachTo attach_to = FloatingAttachTo::None;
        // Only consulted when attach_to == ElementWithId — must equal the *other* element's own
        // ElementDecl::id exactly (Clay hashes both the same way, so this is how they're matched up;
        // there's no compile-time link between them).
        UString parent_id;
        FloatingAttachPoint element_attach_point = FloatingAttachPoint::LeftTop;
        FloatingAttachPoint parent_attach_point = FloatingAttachPoint::LeftBottom;
        glm::vec2 offset{0.0f};
        // Higher draws on top of lower when floating elements themselves overlap each other.
        i16 z_index = 0;
        // false lets hover/click pass through to whatever is visually underneath this floating
        // element instead of being captured by it — rarely what you want for an interactive
        // dropdown/popover, so true (Clay's own default) is the default here too.
        bool capture_pointer = true;
    };

    // Full layout + visual config for one element — mirrors Clay_LayoutConfig plus the top-level
    // visual fields of Clay_ElementDeclaration.
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

        // This element's depth layer — 0 (the default) is the baseline every element starts at;
        // +Z draws closer to the viewer (on top), -Z further back (underneath). *Every* draw item
        // this element produces — its own background/border/image AND any text or nested elements
        // declared inside it — shares this same z unless a nested element sets its own nonzero z,
        // which then applies to it (and anything nested further inside *it*) instead: 0 means
        // "inherit whatever z is already active," not "explicitly reset to the baseline" — the one
        // sharp edge of this model (you cannot force a descendant of a z=5 ancestor back down to a
        // literal 0), traded for not needing a separate "was z actually set" flag. Within one
        // element's own z, sublayers always paint in a fixed background -> children/text -> border
        // order (see Context.hpp's own architecture notes) — z only reorders whole elements (and
        // everything nested inside them) relative to *other* elements, never a single element's own
        // sublayers relative to each other.
        i32 z = 0;

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

    // Where one draw item (a background/border/image quad, one text run, one custom-shader draw)
    // sits in the UI's overall paint order — see ElementDecl::z's own doc comment for the depth
    // system this belongs to. `order` is this item's position in Clay's own render-command stream
    // (which already interleaves an element's background, its children/text, and its border in
    // that exact order — see Context.hpp's architecture notes), used as the tiebreak within the
    // same z so sublayers/siblings that don't set z keep painting in their natural order.
    struct PaintKey {
        i32 z = 0;
        u32 order = 0;

        [[nodiscard]] friend bool operator<(const PaintKey &lhs, const PaintKey &rhs) noexcept {
            return lhs.z != rhs.z ? lhs.z < rhs.z : lhs.order < rhs.order;
        }
    };

    enum class TextWrapMode : u8 { Words, Newlines, None };
    enum class TextAlign : u8 { Left, Center, Right };

    // Caller-assigned opaque font identity, registered via UI::Context::register_font() and
    // referenced from TextStyle::font_id — mirrors Clay_TextElementConfig::fontId.
    using FontId = u16;

    struct TextStyle {
        Color color{1.0, 1.0, 1.0, 1.0};
        FontId font_id = 0;
        u16 font_size = 16;
        u16 letter_spacing = 0;
        u16 line_height = 0; // 0 = derive from the font's own metrics.
        TextWrapMode wrap_mode = TextWrapMode::Words;
        TextAlign alignment = TextAlign::Left;
    };

} // namespace SFT::UI
