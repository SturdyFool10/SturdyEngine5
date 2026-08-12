#include <Text/src/Text/Text.hpp>

#include <algorithm>
#include <array>
#include <cassert>

namespace {
using namespace SFT;
using namespace SFT::Text;

// A square "ring" (outer square with a smaller square hole cut out of its center) — same topology
// every CJK glyph with an enclosed counter (e.g. 口, 日) has, and enough to exercise the
// exterior/hole relationship a real glyph's winding needs to get right. `reverse` flips both
// contours' point order together (preserving their *relative* opposite-ness, exactly what swapping
// a TrueType/glyf-sourced outline for a CFF-sourced one of the same shape actually does — see
// Raster.cpp's own comment on orientContours() for why hb-draw doesn't normalize this itself).
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
    contour.push_back(OutlineSegment{.kind = OutlineSegmentKind::LineTo, .to = corners[0]}); // hb-draw always closes explicitly
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

// Regression test for a real bug: hb-draw-extracted outlines (Outline.cpp) preserve whichever
// winding convention the source font's own table used (TrueType/glyf and CFF/CFF2 fonts disagree
// on which direction means "exterior"), but msdfgen's SDF sign depends on a *consistent* polarity.
// Before Raster.cpp's rasterize_glyph() called shape.orientContours(), a shape built from
// "CFF-style" winding rendered inverted — background solid, glyph ink a transparent hole — which is
// exactly what every CJK glyph in this engine looked like once real fonts (as opposed to .notdef
// tofu boxes) started reaching the rasterizer. This constructs the same ring shape in both winding
// conventions and asserts both rasterize identically and correctly, regardless of which one was fed
// in — the property orientContours() is specifically responsible for.
void ring_rasterizes_correctly_regardless_of_input_winding() {
    const RasterParams params{
        .width = 100, .height = 100, .scale = 0.1f, .pixel_range = 4.0f, .padding_px = 10.0f, .translation = std::nullopt};

    for (const bool reverse : {false, true}) {
        const GlyphOutline outline = make_ring_outline(reverse);
        const auto rasterized = rasterize_glyph(outline, RasterFormat::SDF, params);
        assert(rasterized);
        assert(rasterized->width == 100 && rasterized->height == 100 && rasterized->channel_count == 1);

        // Design (200, 500): inside the ring itself (well clear of both edges) — must read as ink.
        assert(sample(*rasterized, 49, 20) > 200);
        // Design (500, 500): dead center, inside the hole — must read as background, not ink.
        assert(sample(*rasterized, 49, 50) < 55);
        // Design (50, 50): outside the outer square entirely — must read as background.
        assert(sample(*rasterized, 94, 5) < 55);
    }
}

} // namespace

int main() {
    ring_rasterizes_correctly_regardless_of_input_winding();
}
