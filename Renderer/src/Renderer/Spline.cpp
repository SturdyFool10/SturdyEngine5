#include "Spline.hpp"

namespace SFT::Renderer {

vector<GlyphPathPlacement2D> layout_text_on_spline_2d(const Spline2D &spline,
                                                                               span<const Text::PositionedGlyph> glyphs,
                                                                               u32 units_per_em, f32 pixel_size,
                                                                               f32 start_offset) {
        vector<GlyphPathPlacement2D> placements;
        if (!spline.valid() || units_per_em == 0) {
            return placements;
        }
        placements.reserve(glyphs.size());
        const f32 scale = pixel_size / static_cast<f32>(units_per_em);
        f32 arc_length = start_offset;
        for (const Text::PositionedGlyph &glyph : glyphs) {
            const f32 t = spline.t_at_arc_length(arc_length);
            const glm::vec2 position = spline.position(t);
            const glm::vec2 direction = spline.tangent(t);
            const f32 rotation = (direction.x != 0.0f || direction.y != 0.0f) ? std::atan2(direction.y, direction.x) : 0.0f;
            placements.push_back(GlyphPathPlacement2D{.position = position, .rotation = rotation});
            arc_length += glyph.x_advance * scale;
        }
        return placements;
    }

vector<glm::mat4> layout_text_on_spline_3d(const Spline3D &spline,
                                                                    span<const Text::PositionedGlyph> glyphs,
                                                                    u32 units_per_em, f32 pixel_size, glm::vec3 up_hint,
                                                                    f32 start_offset) {
        vector<glm::mat4> transforms;
        if (!spline.valid() || units_per_em == 0) {
            return transforms;
        }
        transforms.reserve(glyphs.size());
        const f32 scale = pixel_size / static_cast<f32>(units_per_em);
        f32 arc_length = start_offset;
        for (const Text::PositionedGlyph &glyph : glyphs) {
            const f32 t = spline.t_at_arc_length(arc_length);
            const glm::vec3 position = spline.position(t);
            glm::vec3 tangent = spline.tangent(t);
            const f32 tangent_length = glm::length(tangent);
            tangent = tangent_length > 1.0e-6f ? tangent / tangent_length : glm::vec3(0.0f, 0.0f, 1.0f);

            glm::vec3 right = glm::cross(tangent, up_hint);
            if (glm::length(right) < 1.0e-5f) {
                const glm::vec3 fallback_up = std::abs(tangent.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                right = glm::cross(tangent, fallback_up);
            }
            right = glm::normalize(right);
            const glm::vec3 up = glm::normalize(glm::cross(right, tangent));

            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right, 0.0f);
            transform[1] = glm::vec4(up, 0.0f);
            transform[2] = glm::vec4(tangent, 0.0f);
            transform[3] = glm::vec4(position, 1.0f);
            transforms.push_back(transform);

            arc_length += glyph.x_advance * scale;
        }
        return transforms;
    }

} // namespace SFT::Renderer
