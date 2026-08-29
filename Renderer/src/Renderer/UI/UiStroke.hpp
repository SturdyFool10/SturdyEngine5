#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <vector>
#pragma endregion

#include <Renderer/UI/CustomElement.hpp>
#include <Renderer/UI/Style.hpp>

using std::vector;

namespace SFT::UI {

    /// Line-cap style for a stroked polyline. Only `Round` is implemented today — every stroke is
    /// drawn as round-capped capsules regardless of this value (see UiStrokePipeline's doc comment);
    /// the other values are reserved for a future stage.
    enum class StrokeCapKind : u32 { Round = 0, Butt = 1, Square = 2 };

    /// Join style between consecutive polyline segments. Only `Round` is implemented today: two
    /// capsule segments sharing an endpoint already overlap into a seamless round join with no extra
    /// geometry, so no dedicated join instance is needed. `Miter`/`Bevel` are reserved for a future
    /// stage.
    enum class StrokeJoinKind : u32 { Round = 0, Miter = 1, Bevel = 2 };

    struct StrokeStyle {
        Color color{0.0, 0.0, 0.0, 1.0};
        f32 width = 1.0f;

        /// AA feather width in pixels, added on top of `width` as extra (empty) vertex-geometry
        /// padding for the antialiasing gradient to fall off into. `0` means "derive AA purely from
        /// fwidth()", matching UiQuadInstance's own no-padding convention — the crisp default used for
        /// axis/gridlines. A caller-set value gives data lines a wider, more stylized soft edge.
        f32 feather_px = 0.0f;

        /// Dash pattern lengths in pixels, measured along the polyline's arc length. `dash_length ==
        /// 0` means solid (no dashing).
        f32 dash_length = 0.0f;
        f32 dash_gap = 0.0f;

        StrokeCapKind cap = StrokeCapKind::Round;
        StrokeJoinKind join = StrokeJoinKind::Round;

        /// Snaps the segment's centerline to the pixel grid for a crisp 1px hairline, instead of a
        /// fwidth()-blurred half-pixel-offset line. Only meaningful for (near-)axis-aligned segments —
        /// intended for axis/gridlines, never for data series (snapping data would misrepresent it).
        bool snap_to_pixel_grid = false;

        /// `0` disables it; a positive value draws `glow_layers` extra soft, low-alpha, ever-wider
        /// capsule instances underneath the normal crisp stroke, fading outward — the same idea behind
        /// this UI's own dashing/feathering, applied several times at once. This is a self-contained
        /// screen-space approximation of bloom (layered soft halos), not the main scene's actual HDR
        /// bright-pass bloom (RendererBloom.cpp): the UI surface composites after the main scene's own
        /// tonemap, so it isn't fed through that pipeline at all, and building a real offscreen-HDR-
        /// plus-bright-pass path for one UI element would mean UiRenderer reaching into Renderer::Renderer
        /// across a layer boundary that doesn't otherwise exist (UiRenderer::prepare() only has an
        /// RHI::RhiDevice/CommandEncoder, not a Renderer::Renderer) — this gets a genuinely glowing look
        /// without that risk. Higher values read as "brighter"/wider glow.
        f32 glow_intensity = 0.0f;
        u32 glow_layers = 3;
    };

    // Field order must byte-match Shaders/ui_stroke.slang's UiStrokeInstance exactly (see the comment
    // there). One instance draws one polyline segment as a capsule; UI::stroke_polyline() (Context.hpp)
    // expands a whole polyline into N-1 of these.
    struct UiStrokeInstance {
        glm::vec2 p0{0.0f};
        glm::vec2 p1{0.0f};
        glm::vec4 color{1.0f};
        f32 half_width = 0.5f;
        f32 feather_px = 0.0f;
        f32 dash_length = 0.0f;
        f32 dash_gap = 0.0f;
        f32 dash_phase = 0.0f;
        f32 snap_to_pixel_grid = 0.0f;
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
    };

    // One polyline within a Context::stroke_paths() call. `points` are in the stroked element's own
    // local space (relative to its top-left) until Context::finish_frame()'s resolve pass offsets
    // them by the element's resolved bounding box.
    struct StrokePath {
        vector<glm::vec2> points;
        StrokeStyle style;
    };

    // The Clay CUSTOM-render-command payload behind Context::stroke_paths()/stroke_polyline() — see
    // UiCustomCommandKind's own doc comment (CustomElement.hpp) for why `command_kind` must stay the
    // first member. Multiple paths share one Clay leaf element (one bounding box, one scissor/paint
    // order) specifically so a chart widget can draw axis lines, gridlines, and every data series
    // inside the same plot rect in a single call — Clay's ordinary box-model layout has no way to
    // overlay multiple *sibling* elements at the same rect (each sibling gets its own position in the
    // parent's flow), and reaching for `FloatingConfig` (Style.hpp) to force an overlay per line would
    // fight that same box model instead of sidestepping it, plus bring its own scissor-inheritance and
    // paint-order questions into what should be a single, ordinary draw call.
    struct StrokePolylineData {
        UiCustomCommandKind command_kind = UiCustomCommandKind::Stroke;

        vector<StrokePath> paths;
    };

} // namespace SFT::UI
