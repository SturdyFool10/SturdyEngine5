#include "Outline.hpp"

namespace SFT::Text::Detail {

void draw_move_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            context.outline.contours.emplace_back();
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::MoveTo,
                .to = glm::vec2{to_x, to_y},
            });
        }

void draw_line_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            if (context.outline.contours.empty()) {
                return;
            }
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::LineTo,
                .to = glm::vec2{to_x, to_y},
            });
        }

void draw_quadratic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
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

void draw_cubic_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *,
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

void draw_close_path(hb_draw_funcs_t *, void *, hb_draw_state_t *, void *) {
            // Contours decoded from glyf/CFF outlines are always implicitly closed (the last point
            // meets the first); no explicit closing segment is needed in this representation.
        }

hb_draw_funcs_t *shared_draw_funcs() noexcept {
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

} // namespace SFT::Text::Detail

namespace SFT::Text {

TextExpected<GlyphOutline> glyph_outline(const Font &font, u32 glyph_id) {
        if (!font) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot extract an outline from an invalid font.");
        }

        Detail::DrawContext context;
        hb_font_draw_glyph(font.handle(), static_cast<hb_codepoint_t>(glyph_id), Detail::shared_draw_funcs(), &context);
        return std::move(context.outline);
    }

} // namespace SFT::Text
