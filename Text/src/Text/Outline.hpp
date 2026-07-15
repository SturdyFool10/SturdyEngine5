#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <hb.h>
#include <utility>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"

using std::vector;

namespace SFT::Text {

    enum class OutlineSegmentKind {
        MoveTo,
        LineTo,
        QuadTo,
        CubicTo,
    };

    // One drawing command in a glyph contour, mirroring HarfBuzz's hb-draw callback shapes exactly
    // (move_to/line_to/quadratic_to/cubic_to). `control1`/`control2` are only meaningful for
    // QuadTo (control1) and CubicTo (control1, control2). Coordinates are in font design units —
    // see `Font::units_per_em()`.
    struct OutlineSegment {
        OutlineSegmentKind kind = OutlineSegmentKind::MoveTo;
        glm::vec2 control1{0.0f};
        glm::vec2 control2{0.0f};
        glm::vec2 to{0.0f};
    };

    // A single closed contour (subpath) of a glyph outline — always starts with a MoveTo.
    using Contour = vector<OutlineSegment>;

    // A glyph's full vector outline: every contour that makes up the glyph (an "o" has two — the
    // outer ring and the inner counter). This is the "ready for vector graphics" representation —
    // curves are preserved exactly as HarfBuzz decoded them from glyf/CFF/CFF2, not flattened, so
    // any consumer (a vector renderer, an SDF/MSDF rasterizer — see :Raster) can process them at
    // whatever precision it needs.
    struct GlyphOutline {
        vector<Contour> contours;
    };

    namespace Detail {

        struct DrawContext {
            GlyphOutline outline;
        };

        void draw_move_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *);

        void draw_line_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *);

        void draw_quadratic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
                                      float control_x, float control_y, float to_x, float to_y, void *);

        void draw_cubic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
                                  float control1_x, float control1_y, float control2_x, float control2_y,
                                  float to_x, float to_y, void *);

        void draw_close_path(hb_draw_funcs_t *, void *, hb_draw_state_t *, void *);

        // One process-lifetime, immutable callback table shared by every glyph_outline() call —
        // hb-draw's recommended usage (hb_draw_funcs_make_immutable) is to build the vtable once
        // and reuse it, since the actual per-glyph state lives in `draw_data`, not the funcs table.
        [[nodiscard]] hb_draw_funcs_t *shared_draw_funcs() noexcept;

    } // namespace Detail

    // Extracts glyph `glyph_id`'s vector outline directly from `font`'s glyf/CFF/CFF2 tables via
    // HarfBuzz's hb-draw API — no FreeType or other outline-decoding dependency. This is the
    // public "vector graphics ready" entry point: contours are exact curves (not flattened),
    // suitable for a vector renderer as-is, and also feed the SDF/MSDF rasterizer (:Raster).
    [[nodiscard]] TextExpected<GlyphOutline> glyph_outline(const Font &font, u32 glyph_id);

} // namespace SFT::Text
