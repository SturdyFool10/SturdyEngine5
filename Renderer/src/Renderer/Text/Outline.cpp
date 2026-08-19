#include <Renderer/Text/Outline.hpp>

namespace SFT::Text::Detail {

/// Draws move to using the current rendering state.
///
/// @param draw_data Data consumed or referenced by the operation.
/// @param to_x `to_x` value used by the operation.
/// @param to_y `to_y` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void draw_move_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *) {
            auto &context = *static_cast<DrawContext *>(draw_data);
            context.outline.contours.emplace_back();
            context.outline.contours.back().push_back(OutlineSegment{
                .kind = OutlineSegmentKind::MoveTo,
                .to = glm::vec2{to_x, to_y},
            });
        }

/// Draws line to using the current rendering state.
///
/// @param draw_data Data consumed or referenced by the operation.
/// @param to_x `to_x` value used by the operation.
/// @param to_y `to_y` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

/// Draws quadratic to using the current rendering state.
///
/// @param draw_data Data consumed or referenced by the operation.
/// @param control_x `control_x` value used by the operation.
/// @param control_y `control_y` value used by the operation.
/// @param to_x `to_x` value used by the operation.
/// @param to_y `to_y` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

/// Draws cubic to using the current rendering state.
///
/// @param draw_data Data consumed or referenced by the operation.
/// @param control1_x `control1_x` value used by the operation.
/// @param control1_y `control1_y` value used by the operation.
/// @param control2_x `control2_x` value used by the operation.
/// @param control2_y `control2_y` value used by the operation.
/// @param to_x `to_x` value used by the operation.
/// @param to_y `to_y` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

/// Draws close path using the current rendering state.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void draw_close_path(hb_draw_funcs_t *, void *, hb_draw_state_t *, void *) {


        }

/// Returns the current or globally available shared draw funcs value.
///
/// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
/// @note This function does not throw exceptions.
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

/// Performs the glyph outline operation for `Text` using the supplied arguments.
///
/// @param font `font` value used by the operation.
/// @param glyph_id Identifier of the target object or resource.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`.
TextExpected<GlyphOutline> glyph_outline(const Font &font, u32 glyph_id) {
        if (!font) {
            return text_error(TextErrorCode::InvalidArgument, "Cannot extract an outline from an invalid font.");
        }

        Detail::DrawContext context;
        hb_font_draw_glyph(font.handle(), static_cast<hb_codepoint_t>(glyph_id), Detail::shared_draw_funcs(), &context);
        return std::move(context.outline);
    }

} // namespace SFT::Text
