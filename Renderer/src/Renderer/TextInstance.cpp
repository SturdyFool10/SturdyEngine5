#include <Renderer/src/Renderer/TextInstance.hpp>


namespace SFT::Renderer {

    /// Creates a glyph instance value from the supplied arguments.
    ///
    /// @param position `position` value used by the operation.
    /// @param placement `placement` value used by the operation.
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param atlas_pixel_range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    GlyphInstance make_glyph_instance(glm::vec2 position, const GlyphPlacement &placement,
                                                            const GlyphSlot &slot, f32 atlas_pixel_range) noexcept {
        const glm::vec2 instance_scale = slot.reference_ppem > 0.0f
                                             ? placement.size / glm::vec2{slot.reference_ppem}
                                             : glm::vec2{1.0f};


        const glm::vec2 bearing_offset = glm::vec2{slot.bearing_x, -slot.bearing_top} * instance_scale;
        const f32 cosine = std::cos(placement.rotation);
        const f32 sine = std::sin(placement.rotation);
        const glm::vec2 rotated_bearing{
            bearing_offset.x * cosine - bearing_offset.y * sine,
            bearing_offset.x * sine + bearing_offset.y * cosine,
        };
        const glm::vec2 raster_size = slot.raster_size_px * instance_scale;
        return GlyphInstance{
            .position = position + rotated_bearing,
            .size = raster_size,
            .uv_min = slot.uv_min,
            .uv_max = slot.uv_max,
            .color = placement.color,
            .rotation = placement.rotation,
            .format_kind = format_kind_value(slot.format),
            .distance_pixel_range = atlas_pixel_range,
            .stem_darkening_px = placement.stem_darkening ? resolved_stem_darkening_px(placement.pixel_size) : 0.0f,
        };
    }

} // namespace SFT::Renderer

