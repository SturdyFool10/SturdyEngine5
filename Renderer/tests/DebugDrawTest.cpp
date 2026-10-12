// CPU-only checks of `DebugDrawList` (Renderer/DebugDraw.cpp): every shape records the segments it should, in the right place.
// The GPU side (pipeline, overlay) is exercised by running the demo.

#include <Renderer/DebugDraw.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
    bool near(const glm::vec3 &a, const glm::vec3 &b, float eps = 1e-4f) { return glm::length(a - b) <= eps; }

    struct Bounds {
        glm::vec3 min{1e30f}, max{-1e30f};
    };
    Bounds bounds_of(const SFT::Renderer::DebugDrawList &list) {
        Bounds b;
        for (const auto &line : list.lines()) {
            b.min = glm::min(b.min, glm::min(line.a, line.b));
            b.max = glm::max(b.max, glm::max(line.a, line.b));
        }
        return b;
    }
    const glm::vec4 white{1.0f};
} // namespace

int main() {
    using namespace SFT::Renderer;

    // Colour packing: r in the low byte, channels clamped, non-finite treated as zero.
    check(pack_debug_color({1.0f, 0.0f, 0.0f, 1.0f}) == 0xff0000ffu, "red packs into the low byte, alpha into the high byte");
    check(pack_debug_color({0.0f, 1.0f, 0.0f, 0.0f}) == 0x0000ff00u, "green is the second byte");
    check(pack_debug_color({2.0f, -1.0f, NAN, 0.5f}) == (0x80000000u | 0xffu), "out-of-range and NaN channels are clamped");

    // line / width.
    {
        DebugDrawList list;
        list.set_width(4.0f);
        list.line({0, 0, 0}, {1, 2, 3}, white);
        check(list.size() == 1 && list.lines()[0].width_px == 4.0f && list.lines()[0].b == glm::vec3(1, 2, 3), "a line records its endpoints and width");
    }

    // polyline: open and closed.
    {
        DebugDrawList list;
        const glm::vec3 points[] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        list.polyline(points, white);
        check(list.size() == 3, "an open polyline of 4 points has 3 segments");
        list.clear();
        list.polyline(points, white, true);
        check(list.size() == 4, "a closed polyline has 4");
    }

    // Boxes: 12 edges, spanning exactly the box; an oriented box is the same shape moved by its transform.
    {
        DebugDrawList list;
        list.box({-1, 0, 2}, {3, 5, 4}, white);
        const Bounds b = bounds_of(list);
        check(list.size() == 12, "a box has 12 edges");
        check(near(b.min, {-1, 0, 2}) && near(b.max, {3, 5, 4}), "an axis-aligned box spans its min/max");

        DebugDrawList moved;
        const glm::mat4 transform = glm::translate(glm::mat4{1.0f}, glm::vec3{10, 0, 0}) * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{0, 0, 1});
        moved.oriented_box(transform, {2, 1, 1}, white);
        const Bounds m = bounds_of(moved);
        // Rotated 90 degrees about Z, the 2x1 footprint becomes 1x2 around x = 10.
        check(near(m.min, {9, -2, -1}) && near(m.max, {11, 2, 1}), "an oriented box follows its transform");
        // Every edge has the length of one of the box's full extents.
        bool lengths_ok = true;
        for (const auto &line : moved.lines()) {
            const float length = glm::length(line.b - line.a);
            lengths_ok = lengths_ok && (near(length, 4.0f) || near(length, 2.0f));
        }
        check(lengths_ok, "oriented box edges keep the box's dimensions");
    }

    // Sphere / circle: points lie on the sphere; segment counts follow `segments`.
    {
        DebugDrawList list;
        list.sphere({1, 2, 3}, 2.0f, white, 16);
        check(list.size() == 3 * 16, "a sphere is three circles");
        bool on_sphere = true;
        for (const auto &line : list.lines()) {
            on_sphere = on_sphere && near(glm::length(line.a - glm::vec3{1, 2, 3}), 2.0f) && near(glm::length(line.b - glm::vec3{1, 2, 3}), 2.0f);
        }
        check(on_sphere, "every sphere vertex is one radius from the centre");

        DebugDrawList ring;
        ring.circle({0, 0, 0}, {0, 1, 0}, 1.0f, white, 20);
        bool flat = true;
        for (const auto &line : ring.lines()) flat = flat && near(line.a.y, 0.0f) && near(line.b.y, 0.0f);
        check(ring.size() == 20 && flat, "a circle lies in the plane of its normal");
    }

    // Capsule: reaches `radius` beyond both ends along the axis and `radius` to the sides.
    {
        DebugDrawList list;
        list.capsule({0, 0, 0}, {0, 4, 0}, 1.0f, white, 8);
        const Bounds b = bounds_of(list);
        check(near(b.min.y, -1.0f) && near(b.max.y, 5.0f), "a capsule's hemispheres extend a radius past the end points");
        check(near(b.min.x, -1.0f) && near(b.max.x, 1.0f) && near(b.min.z, -1.0f) && near(b.max.z, 1.0f), "a capsule is one radius wide");
    }

    // Cone: apex and base circle of radius length * tan(half angle).
    {
        DebugDrawList list;
        list.cone({0, 0, 0}, {0, -2, 0}, 3.0f, glm::radians(45.0f), white, 24);
        const Bounds b = bounds_of(list);
        check(near(b.min.y, -3.0f) && near(b.max.y, 0.0f), "a cone runs from its apex to `length` along the direction");
        check(near(b.max.x, 3.0f, 1e-3f) && near(b.min.x, -3.0f, 1e-3f), "a 45 degree cone's base radius equals its length");
    }

    // Arrow: shaft + 4 head lines, ending at the tip.
    {
        DebugDrawList list;
        list.arrow({0, 0, 0}, {0, 0, 5}, white);
        check(list.size() == 5, "an arrow is a shaft and four head lines");
        bool head_at_tip = true;
        for (usize i = 1; i < list.size(); ++i) head_at_tip = head_at_tip && near(list.lines()[i].a, {0, 0, 5});
        check(head_at_tip, "arrow head lines start at the tip");
    }

    // Axes: three lines from the transform's origin along its (scaled) basis, in RGB order.
    {
        DebugDrawList list;
        list.axes(glm::translate(glm::mat4{1.0f}, glm::vec3{1, 1, 1}), 2.0f);
        check(list.size() == 3 && near(list.lines()[0].b, {3, 1, 1}) && near(list.lines()[1].b, {1, 3, 1}) && near(list.lines()[2].b, {1, 1, 3}),
              "axes are the transform's X, Y and Z, `size` long");
        check(list.lines()[0].color != list.lines()[1].color && list.lines()[1].color != list.lines()[2].color, "each axis has its own colour");
    }

    // Frustum: 12 edges whose near corners sit on the near plane and far corners on the far plane.
    {
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 1.0f, 10.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3{0, 0, 0}, glm::vec3{0, 0, -1}, glm::vec3{0, 1, 0});
        DebugDrawList list;
        list.frustum(glm::inverse(projection * view), white);
        const Bounds b = bounds_of(list);
        check(list.size() == 12, "a frustum has 12 edges");
        check(near(b.max.z, -1.0f, 1e-3f) && near(b.min.z, -10.0f, 1e-3f), "frustum spans the near to the far plane along the view axis");
    }

    // Grid: (2n + 1) lines each way.
    {
        DebugDrawList list;
        list.grid({0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 3, 2.0f, white);
        const Bounds b = bounds_of(list);
        check(list.size() == 2 * 7, "a grid of half_cells = 3 has 7 lines each way");
        check(near(b.min, {-6, 0, -6}) && near(b.max, {6, 0, 6}), "a grid spans half_cells * cell_size either side");
    }

    // take() empties the list.
    {
        DebugDrawList list;
        list.cross({0, 0, 0}, 1.0f, white);
        const auto taken = list.take();
        check(taken.size() == 3 && list.empty(), "take() moves the segments out");
    }

    if (failures == 0) std::printf("DebugDrawTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
