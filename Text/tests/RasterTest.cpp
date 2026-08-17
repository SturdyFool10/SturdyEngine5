#include <Text/src/Text/Text.hpp>

#include <algorithm>
#include <array>
#include <cassert>

namespace {
using namespace SFT;
using namespace SFT::Text;







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

[[nodiscard]] GlyphOutline make_ring_outline(bool reverse) {
    GlyphOutline outline;
    outline.contours.push_back(make_square_contour({100.0f, 100.0f}, {900.0f, 900.0f}, reverse));
    outline.contours.push_back(make_square_contour({350.0f, 350.0f}, {650.0f, 650.0f}, reverse));
    return outline;
}

[[nodiscard]] u8 sample(const RasterizedGlyph &glyph, u32 row, u32 col) {
    assert(glyph.channel_count == 1);
    return glyph.pixels[static_cast<usize>(row) * glyph.width + col];
}










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

int main() {
    ring_rasterizes_correctly_regardless_of_input_winding();
}
