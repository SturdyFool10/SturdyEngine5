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

        inline void draw_move_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            context.outline.contours.emplace_back();
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::MoveTo,
                .to = glm::vec2{to_x, to_y},
            });
        }

        inline void draw_line_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            if (context.outline.contours.empty()) {
                return;
            }
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::LineTo,
                .to = glm::vec2{to_x, to_y},
            });
        }

        inline void draw_quadratic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
                                      float control_x, float control_y, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            if (context.outline.contours.empty()) {
                return;
            }
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::QuadTo,
                .control1 = glm::vec2{control_x, control_y},
                .to = glm::vec2{to_x, to_y},
            });
        }

        inline void draw_cubic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
                                  float control1_x, float control1_y, float control2_x, float control2_y,
                                  float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            if (context.outline.contours.empty()) {
                return;
            }
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::CubicTo,
                .control1 = glm::vec2{control1_x, control1_y},
                .control2 = glm::vec2{control2_x, control2_y},
                .to = glm::vec2{to_x, to_y},
            });
        }

        inline void draw_close_path(hb_draw_funcs_t *, void *, hb_draw_state_t *, void *) {
            // Contours decoded from glyf/CFF outlines are always implicitly closed (the last point
            // meets the first); no explicit closing segment is needed in this representation.
        }

        // One process-lifetime, immutable callback table shared by every glyph_outline() call —
        // hb-draw's recommended usage (hb_draw_funcs_make_immutable) is to build the vtable once
        // and reuse it, since the actual per-glyph state lives in `draw_data`, not the funcs table.
        [[nodiscard]] inline hb_draw_funcs_t *shared_draw_funcs() noexcept {
            static hb_draw_funcs_t *funcs = [] {
                hb_draw_funcs_t *created = hb_draw_funcs_create();
                hb_draw_funcs_set_move_to_func(created, draw_move_to, nullptr, nullptr);
                hb_draw_funcs_set_line_to_func(created, draw_line_to, nullptr, nullptr);
                hb_draw_funcs_set_quadratic_to_func(created, draw_quadratic_to, nullptr, nullptr);
                hb_draw_funcs_set_cubic_to_func(created, draw_cubic_to, nullptr, nullptr);
                hb_draw_funcs_set_close_path_func(created, draw_close_path, nullptr, nullptr);
                hb_draw_funcs_make_immutable(created);
                return created;
            }();
            return funcs;
        }

    } // namespace Detail

    // Extracts glyph `glyph_id`'s vector outline directly from `font`'s glyf/CFF/CFF2 tables via
    // HarfBuzz's hb-draw API — no FreeType or other outline-decoding dependency. This is the
    // public "vector graphics ready" entry point: contours are exact curves (not flattened),
    // suitable for a vector renderer as-is, and also feed the SDF/MSDF rasterizer (:Raster).
    [[nodiscard]] inline TextExpected<GlyphOutline> glyph_outline(const Font &font, u32 glyph_id) {
        if (!font) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot extract an outline from an invalid font.");
        }

        Detail::DrawContext context;
        hb_font_draw_glyph(font.handle(), static_cast<hb_codepoint_t>(glyph_id), Detail::shared_draw_funcs(), &context);
        return std::move(context.outline);
    }

} // namespace SFT::Text
