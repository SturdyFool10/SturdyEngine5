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

    /// One layout element. Padding and gap are in logical UI units.
    struct ForeignUiElement {
        ForeignUiSizing width_kind = ForeignUiSizing::Fit;
        f32 width_value = 0.0f;
        ForeignUiSizing height_kind = ForeignUiSizing::Fit;
        f32 height_value = 0.0f;
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
    };

    struct ForeignUiTextStyle {
        u32 font_id = 0;
        u32 font_size = 16;
        f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    /// Whether UI units follow the OS content scale (the default) or are raw pixels. Values match the C ABI.
    enum class ForeignUiScaleMode : u32 { ContentScale = 0, RawPixels = 1 };

    /// Loads a font file and registers it under `font_id`. Safe at any time: it is replayed into the UI context whenever that is rebuilt.
    [[nodiscard]] ForeignUiResult<void> register_foreign_ui_font(Engine &engine, std::string_view path, u32 font_id);
    [[nodiscard]] ForeignUiResult<void> set_foreign_ui_scale_mode(Engine &engine, ForeignUiScaleMode mode);

    /// Opens a UI frame on this thread, sized to the surface's framebuffer in pixels.
    [[nodiscard]] ForeignUiResult<void> begin_foreign_ui_frame(Engine &engine, u32 framebuffer_width, u32 framebuffer_height, f64 delta_seconds);
    /// Finishes the frame. With a non-null `frame` the layout is attached to it as an overlay; with null it is finished and discarded so
    /// the next frame starts clean. Fails (and closes the frame) when an element was left open.
    [[nodiscard]] ForeignUiResult<void> end_foreign_ui_frame(Engine &engine, RenderFrameParameters *frame);

    /// Abandons the open frame (and any elements left open) without finishing it. For an error path that cannot reach the engine; a normal
    /// frame ends with `end_foreign_ui_frame`. Does nothing when no frame is open.
    void discard_foreign_ui_frame() noexcept;

    [[nodiscard]] ForeignUiResult<void> begin_foreign_ui_element(const ForeignUiElement &element);
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
