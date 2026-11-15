#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include <Engine/Engine.hpp>
#include <Engine/ScreenUi.hpp>
#include <Foundation/Foundation.hpp>
#include <Renderer/UI/UI.hpp>

namespace SFT::Engine {

    /// The immediate-mode UI for code that cannot use C++ RAII scopes (Rust, C#, Java, scripts), shared by every binding so they behave
    /// identically. The engine's own UI is built from scopes (`Context::element` returns an `ElementScope` whose destructor closes the
    /// element); here a caller pairs explicit begin/end calls instead, and an unbalanced tree is reported rather than corrupting the layout.
    ///
    /// A UI frame is `begin_foreign_ui_frame`, any number of elements and text, then `end_foreign_ui_frame`, which finishes the layout and
    /// attaches it to the frame as an overlay. The frame is bounded by begin/end on one thread (a window's render thread, usually) and only
    /// one can be open per thread. The UI renderer and its input state are created on first use and kept for the engine until
    /// `release_foreign_ui`, which must run before the engine is destroyed.

    enum class ForeignUiErrorKind {
        InvalidArgument,
        /// Something this needs is not there (no graphics device, a font file that cannot be read).
        NotAvailable,
        /// A frame is already open on this thread, or none is open when one is needed.
        Busy,
    };

    struct ForeignUiError {
        ForeignUiErrorKind kind = ForeignUiErrorKind::InvalidArgument;
        std::string message;
    };

    template <class Value>
    using ForeignUiResult = std::expected<Value, ForeignUiError>;

    /// How one axis of an element is sized. Values match the C ABI.
    enum class ForeignUiSizing : u32 { Fit = 0, Grow = 1, Fixed = 2, Percent = 3 };

    /// `axis` with the size limits applied when it is a `Fit` or `Grow` axis (see `ForeignUiElement::width_min`); other kinds come back as
    /// they are. A maximum of 0 or less is no maximum.
    [[nodiscard]] inline UI::SizingAxis with_size_limits(UI::SizingAxis axis, f32 min_size, f32 max_size) noexcept {
        if (axis.kind == UI::SizingKind::Fit || axis.kind == UI::SizingKind::Grow) {
            axis.min = min_size > 0.0f ? min_size : 0.0f;
            axis.max = max_size > 0.0f ? max_size : 3.4028235e38f;
        }
        return axis;
    }

    /// One layout element. Padding and gap are in logical UI units.
    struct ForeignUiElement {
        ForeignUiSizing width_kind = ForeignUiSizing::Fit;
        f32 width_value = 0.0f;
        ForeignUiSizing height_kind = ForeignUiSizing::Fit;
        f32 height_value = 0.0f;
        /// Smallest and largest size of a `Fit` or `Grow` axis (CSS `min-width` / `max-width` and the height pair). A minimum of 0 and a
        /// maximum of 0 or less mean "no limit"; both are ignored for `Fixed` and `Percent`.
        f32 width_min = 0.0f;
        f32 width_max = 0.0f;
        f32 height_min = 0.0f;
        f32 height_max = 0.0f;
        /// Left, right, top, bottom.
        f32 padding[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        f32 child_gap = 0.0f;
        bool top_to_bottom = false;
        /// Straight-alpha non-linear sRGB.
        f32 background[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        /// Top-left, top-right, bottom-right, bottom-left.
        f32 corner_radius[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        /// Names the element for `hovered` / `clicked`; empty for none.
        std::string id;
        /// Profile of each corner in the order of `corner_radius`: 1 round (the default), 2 squircle, 0 bevel, -1 scoop, +inf square,
        /// -inf notch, or any CSS `superellipse(k)`.
        f32 corner_shape[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        /// A `ForeignUiEffectId` from `ForeignUiEffects.hpp`, or 0 for none. The element and everything inside it is drawn through it.
        u64 effect = 0;
        /// Per-edge border colours and styles (left, right, top, bottom), when `has_border_edges`: straight-alpha sRGB colour and a
        /// style (0 solid, 1 dashed, 2 dotted, 3 double) for each. The widths still come from where the element's border is declared.
        /// Edge-aware layout: pad the element so children stay inside its rounded/bevelled/scooped/notched corners.
        bool inset_for_corners = false;
        /// Clipping: `clip_horizontal` / `clip_vertical` clip (and make scrollable) the overflow to the element's rectangle; `clip_follow_corners`
        /// cuts it to the element's own rounded/shaped corners; `clip_path` is a `ForeignUiEffectId` of a clip effect (circle, polygon, ...) to cut it to.
        bool clip_horizontal = false;
        bool clip_vertical = false;
        bool clip_follow_corners = false;
        u64 clip_path = 0;
        bool has_border_edges = false;
        /// Left, right, top, bottom widths in pixels of the border the edges describe.
        f32 border_width[4] = {};
        f32 border_edge_color[4][4] = {};
        u32 border_edge_style[4] = {};
    };

    struct ForeignUiTextStyle {
        u32 font_id = 0;
        u32 font_size = 16;
        f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        /// Bits: 1 underline, 2 overline, 4 line-through.
        u32 decoration_lines = 0;
        /// 0 solid, 1 dashed, 2 dotted, 3 double.
        u32 decoration_style = 0;
        /// Pixels; 0 = from the font size.
        f32 decoration_thickness = 0.0f;
        bool has_decoration_color = false;
        f32 decoration_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool shadow_enabled = false;
        f32 shadow_offset[2] = {1.0f, 1.0f};
        f32 shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.5f};
    };

    /// The engine text style for a foreign one. Fails for an unknown decoration style.
    [[nodiscard]] ForeignUiResult<UI::TextStyle> make_foreign_ui_text_style(const ForeignUiTextStyle &style);

    /// Whether UI units follow the OS content scale (the default) or are raw pixels. Values match the C ABI.
    enum class ForeignUiScaleMode : u32 { ContentScale = 0, RawPixels = 1 };

    /// Loads a font file and registers it under `font_id`. Safe at any time: it is replayed into the UI context whenever that is rebuilt.
    /// What decides where the window UI's elements react to the pointer: 0 CSS-like (transforms and clip-paths followed, drawn elements cover
    /// those below; the default), 1 fixed to the layout. Takes effect at the next frame. Any thread.
    void set_foreign_ui_hit_test_mode(u32 mode) noexcept;
    [[nodiscard]] ForeignUiResult<void> register_foreign_ui_font(Engine &engine, std::string_view path, u32 font_id);
    [[nodiscard]] ForeignUiResult<void> set_foreign_ui_scale_mode(Engine &engine, ForeignUiScaleMode mode);

    /// Opens a UI frame on this thread, sized to the surface's framebuffer in pixels.
    [[nodiscard]] ForeignUiResult<void> begin_foreign_ui_frame(Engine &engine, u32 framebuffer_width, u32 framebuffer_height, f64 delta_seconds);
    /// Finishes the frame. With a non-null `frame` the layout is attached to it as an overlay; with null it is finished and discarded so
    /// the next frame starts clean. Fails (and closes the frame) when an element was left open.
    [[nodiscard]] ForeignUiResult<void> end_foreign_ui_frame(Engine &engine, RenderFrameParameters *frame);

    /// Makes `context` this thread's open UI frame while it lives, so a UI tree's builder can use the same element, text and query calls
    /// (`begin_foreign_ui_element`, ...) as a window frame. The destructor closes whatever the builder left open. Must be created and
    /// destroyed on the thread running the builder, and not while another frame is open on it.
    class ForeignUiContextBinding {
      public:
        ForeignUiContextBinding(UI::Context &context, glm::vec2 viewport);
        ~ForeignUiContextBinding();
        ForeignUiContextBinding(const ForeignUiContextBinding &) = delete;
        ForeignUiContextBinding &operator=(const ForeignUiContextBinding &) = delete;

        /// True when another frame was already open (the binding did nothing).
        [[nodiscard]] bool rejected() const noexcept { return rejected_; }
        /// True when every element the builder opened is closed again.
        [[nodiscard]] bool balanced() const noexcept;

      private:
        bool rejected_ = false;
    };

    /// Abandons the open frame (and any elements left open) without finishing it. For an error path that cannot reach the engine; a normal
    /// frame ends with `end_foreign_ui_frame`. Does nothing when no frame is open.
    void discard_foreign_ui_frame() noexcept;

    /// The engine declaration for `element` (sizing, padding, colours, corner radii and shapes, effect, id). Fails for an unrecognised
    /// sizing mode or an unknown effect id. No frame needs to be open.
    [[nodiscard]] ForeignUiResult<UI::ElementDecl> make_foreign_ui_decl(const ForeignUiElement &element);
    [[nodiscard]] ForeignUiResult<void> begin_foreign_ui_element(const ForeignUiElement &element);
    /// Opens an element from a ready-made declaration (a dock panel's content region, say); close it with `end_foreign_ui_element`.
    [[nodiscard]] ForeignUiResult<void> begin_foreign_ui_declared_element(const UI::ElementDecl &declaration);
    [[nodiscard]] ForeignUiResult<void> end_foreign_ui_element();
    [[nodiscard]] ForeignUiResult<void> foreign_ui_text(std::string_view text, const ForeignUiTextStyle &style);
    [[nodiscard]] ForeignUiResult<bool> foreign_ui_hovered(std::string_view id);
    [[nodiscard]] ForeignUiResult<bool> foreign_ui_clicked(std::string_view id);
    [[nodiscard]] ForeignUiResult<glm::vec2> foreign_ui_pointer_position();
    [[nodiscard]] ForeignUiResult<bool> foreign_ui_pointer_down();

    /// The `ScreenUi` the foreign surfaces share for this engine (created on first use, released by `release_foreign_ui`). For a binding that
    /// has to read or feed its window input or draw into its context outside the begin/end calls above (the Rust shim does).
    [[nodiscard]] ScreenUi &foreign_screen_ui(Engine &engine);

    /// The open frame's context, for widgets (the graph widget) that draw into it directly; null when no frame is open on this thread.
    [[nodiscard]] UI::Context *foreign_ui_context() noexcept;
    /// Translates a sizing mode and value into the layout axis. Fails for a value outside `ForeignUiSizing`.
    [[nodiscard]] ForeignUiResult<UI::SizingAxis> to_ui_axis(ForeignUiSizing kind, f32 value);

    /// Drops the UI renderer and input state created for the engine. Call before the engine is destroyed (it holds a system in the engine's
    /// schedule and GPU objects on its device).
    void release_foreign_ui() noexcept;

} // namespace SFT::Engine
