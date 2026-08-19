#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <hb.h>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/Text/Error.hpp>
#include <Renderer/Text/Font.hpp>

using std::vector;

namespace SFT::Text {

    enum class OutlineSegmentKind {
        MoveTo,
        LineTo,
        QuadTo,
        CubicTo,
    };


    struct OutlineSegment {
        OutlineSegmentKind kind = OutlineSegmentKind::MoveTo;
        glm::vec2 control1{0.0f};
        glm::vec2 control2{0.0f};
        glm::vec2 to{0.0f};
    };


    using Contour = vector<OutlineSegment>;


    struct GlyphOutline {
        vector<Contour> contours;
    };

    namespace Detail {

        struct DrawContext {
            GlyphOutline outline;
        };

        /// Draws move to using the current rendering state.
        ///
        /// @param draw_data Data consumed or referenced by the operation.
        /// @param to_x `to_x` value used by the operation.
        /// @param to_y `to_y` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_move_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *);

        /// Draws line to using the current rendering state.
        ///
        /// @param draw_data Data consumed or referenced by the operation.
        /// @param to_x `to_x` value used by the operation.
        /// @param to_y `to_y` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_line_to(hb_draw_funcs_t *, void *draw_data, hb_draw_state_t *, float to_x, float to_y, void *);

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
                                      float control_x, float control_y, float to_x, float to_y, void *);

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
                                  float to_x, float to_y, void *);

        /// Draws close path using the current rendering state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_close_path(hb_draw_funcs_t *, void *, hb_draw_state_t *, void *);


        /// Returns the current or globally available shared draw funcs value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] hb_draw_funcs_t *shared_draw_funcs() noexcept;

    } // namespace Detail


    /// Performs the glyph outline operation for `Text` using the supplied arguments.
    ///
    /// @param font `font` value used by the operation.
    /// @param glyph_id Identifier of the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] TextExpected<GlyphOutline> glyph_outline(const Font &font, u32 glyph_id);

} // namespace SFT::Text
