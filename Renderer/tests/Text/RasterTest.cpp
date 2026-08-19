#include <Renderer/Text/Text.hpp>

#include <algorithm>
#include <array>
#include <cassert>

namespace {
using namespace SFT;
using namespace SFT::Text;


/// Creates a square contour value from the supplied arguments.
///
/// @param min `min` value used by the operation.
/// @param max `max` value used by the operation.
/// @param reverse `reverse` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] Contour make_square_contour(glm::vec2 min, glm::vec2 max, bool reverse) {
    std::array<glm::vec2, 4> corners{min, glm::vec2{max.x, min.y}, max, glm::vec2{min.x, max.y}};
    if (reverse) {
        std::ranges::reverse(corners);
    }
    Contour contour;
    contour.push_back(OutlineSegment{.kind = OutlineSegmentKind::MoveTo, .to = corners[0]});
    for (usize i = 1; i < corners.size(); ++i) {
        contour.push_back(OutlineSegment{.kind = OutlineSegmentKind::LineTo, .to = corners[i]});
    }
    contour.push_back(OutlineSegment{.kind = OutlineSegmentKind::LineTo, .to = corners[0]});
    return contour;
}

/// Creates a ring outline value from the supplied arguments.
///
/// @param reverse `reverse` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] GlyphOutline make_ring_outline(bool reverse) {
    GlyphOutline outline;
    outline.contours.push_back(make_square_contour({100.0f, 100.0f}, {900.0f, 900.0f}, reverse));
    outline.contours.push_back(make_square_contour({350.0f, 350.0f}, {650.0f, 650.0f}, reverse));
    return outline;
}

/// Performs the sample operation using the supplied arguments.
///
/// @param glyph `glyph` value used by the operation.
/// @param row `row` value used by the operation.
/// @param col `col` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @pre `glyph.channel_count == 1`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] u8 sample(const RasterizedGlyph &glyph, u32 row, u32 col) {
    assert(glyph.channel_count == 1);
    return glyph.pixels[static_cast<usize>(row) * glyph.width + col];
}


/// Performs the ring rasterizes correctly regardless of input winding operation using the supplied arguments.
///
/// @pre `rasterized`; debug builds assert if this precondition is violated.
/// @pre `rasterized->width == 100 && rasterized->height == 100 && rasterized->channel_count == 1`; debug builds assert if this precondition is violated.
/// @pre `sample(*rasterized, 49, 20) > 200`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void ring_rasterizes_correctly_regardless_of_input_winding() {
    const RasterParams params{
        .width = 100, .height = 100, .scale = 0.1f, .pixel_range = 4.0f, .padding_px = 10.0f, .translation = std::nullopt};

    for (const bool reverse : {false, true}) {
        const GlyphOutline outline = make_ring_outline(reverse);
        const auto rasterized = rasterize_glyph(outline, RasterFormat::SDF, params);
        assert(rasterized);
        assert(rasterized->width == 100 && rasterized->height == 100 && rasterized->channel_count == 1);


        assert(sample(*rasterized, 49, 20) > 200);

        assert(sample(*rasterized, 49, 50) < 55);

        assert(sample(*rasterized, 94, 5) < 55);
    }
}

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    ring_rasterizes_correctly_regardless_of_input_winding();
}
