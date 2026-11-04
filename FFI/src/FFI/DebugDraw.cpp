/// C ABI for `Engine::DebugDraw`: lines, shapes and arrows drawn over the world from foreign code.
///
/// Every function resolves the engine and forwards one call. Colours arrive as RGBA floats, matrices as 16 column-major floats.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    [[nodiscard]] glm::vec3 to_glm(const SturdyVec3 &v) noexcept { return {v.x, v.y, v.z}; }
    [[nodiscard]] glm::vec4 to_glm(const SturdyVec4 &v) noexcept { return {v.x, v.y, v.z, v.w}; }
    [[nodiscard]] glm::mat4 to_glm(const SturdyMat4 &m) noexcept {
        glm::mat4 out;
        std::memcpy(&out, m.m, sizeof(out));
        return out;
    }

    /// Resolves the engine and runs `body(debug_draw)`.
    template <class Body>
    [[nodiscard]] SturdyResult with_draw(SturdyEngine engine, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            SFT::Engine::Engine *resolved = nullptr;
            const SturdyResult result = resolve_engine(engine, &resolved);
            if (result != STURDY_OK) {
                return result;
            }
            return body(resolved->debug_draw());
        });
    }

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    // A NaN or infinity in a vertex poisons the line pipeline's bounds; refuse it where the caller can see why.
    [[nodiscard]] bool finite(const SturdyVec3 &v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    [[nodiscard]] bool finite(const SturdyMat4 &m) noexcept {
        for (const float value : m.m) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool lifetime_ok(float seconds) noexcept { return std::isfinite(seconds) && seconds >= 0.0f; }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_settings_init(SturdyDebugDrawSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return invalid("settings must not be null");
        }
        const SFT::Renderer::DebugDrawSettings defaults;
        *settings = {sizeof(SturdyDebugDrawSettings), defaults.depth_test ? STURDY_TRUE : STURDY_FALSE, defaults.occluded_alpha, defaults.max_lines};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_set_settings(SturdyEngine engine, const SturdyDebugDrawSettings *settings) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (settings == nullptr) {
            return invalid("settings must not be null");
        }
        if (settings->struct_size != sizeof(SturdyDebugDrawSettings)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyDebugDrawSettings::struct_size does not match this build");
        }
        if (!std::isfinite(settings->occluded_alpha)) {
            return invalid("occluded_alpha must be finite");
        }
        SFT::Renderer::DebugDrawSettings native;
        native.depth_test = settings->depth_test != 0;
        native.occluded_alpha = settings->occluded_alpha;
        native.max_lines = settings->max_lines;
        draw.set_settings(native);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_settings(SturdyEngine engine, SturdyDebugDrawSettings *out_settings) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (out_settings == nullptr) {
            return invalid("output pointer must not be null");
        }
        const SFT::Renderer::DebugDrawSettings native = draw.settings();
        *out_settings = {sizeof(SturdyDebugDrawSettings), native.depth_test ? STURDY_TRUE : STURDY_FALSE, native.occluded_alpha, native.max_lines};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_set_enabled(SturdyEngine engine, SturdyBool enabled) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        draw.set_enabled(enabled != 0);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_enabled(SturdyEngine engine, SturdyBool *out_enabled) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (out_enabled == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_enabled = draw.enabled() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_set_width(SturdyEngine engine, float width_pixels) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!std::isfinite(width_pixels) || width_pixels <= 0.0f) {
            return invalid("width must be finite and positive");
        }
        draw.set_width(width_pixels);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_clear(SturdyEngine engine) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        draw.clear();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_line_count(SturdyEngine engine, uint64_t *out_count) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = draw.line_count();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_line(SturdyEngine engine, SturdyVec3 a, SturdyVec3 b, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(a) || !finite(b) || !lifetime_ok(seconds)) {
            return invalid("points must be finite and seconds must be finite and not negative");
        }
        draw.line(to_glm(a), to_glm(b), to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_polyline(SturdyEngine engine, const SturdyVec3 *points, uint32_t point_count, SturdyVec4 color, SturdyBool closed,
                                                        float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if ((points == nullptr && point_count != 0) || !lifetime_ok(seconds)) {
            return invalid("points must not be null and seconds must be finite and not negative");
        }
        std::vector<glm::vec3> converted;
        converted.reserve(point_count);
        for (uint32_t i = 0; i < point_count; ++i) {
            if (!finite(points[i])) {
                return invalid("a point is not finite");
            }
            converted.push_back(to_glm(points[i]));
        }
        draw.polyline(converted, to_glm(color), closed != 0, seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_cross(SturdyEngine engine, SturdyVec3 point, float size, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(point) || !std::isfinite(size) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.cross(to_glm(point), size, to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_box(SturdyEngine engine, SturdyVec3 min, SturdyVec3 max, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(min) || !finite(max) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.box(to_glm(min), to_glm(max), to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_oriented_box(SturdyEngine engine, SturdyMat4 transform, SturdyVec3 half_extents, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(transform) || !finite(half_extents) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.oriented_box(to_glm(transform), to_glm(half_extents), to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_sphere(SturdyEngine engine, SturdyVec3 center, float radius, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(center) || !std::isfinite(radius) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.sphere(to_glm(center), radius, to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_circle(SturdyEngine engine, SturdyVec3 center, SturdyVec3 normal, float radius, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(center) || !finite(normal) || !std::isfinite(radius) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.circle(to_glm(center), to_glm(normal), radius, to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_capsule(SturdyEngine engine, SturdyVec3 a, SturdyVec3 b, float radius, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(a) || !finite(b) || !std::isfinite(radius) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.capsule(to_glm(a), to_glm(b), radius, to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_cone(SturdyEngine engine, SturdyVec3 apex, SturdyVec3 direction, float length, float half_angle_radians,
                                                    SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(apex) || !finite(direction) || !std::isfinite(length) || !std::isfinite(half_angle_radians) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.cone(to_glm(apex), to_glm(direction), length, half_angle_radians, to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_arrow(SturdyEngine engine, SturdyVec3 from, SturdyVec3 to, SturdyVec4 color, float head_size, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(from) || !finite(to) || !std::isfinite(head_size) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.arrow(to_glm(from), to_glm(to), to_glm(color), head_size, seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_axes(SturdyEngine engine, SturdyMat4 transform, float size, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(transform) || !std::isfinite(size) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.axes(to_glm(transform), size, seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_frustum(SturdyEngine engine, SturdyMat4 inverse_view_projection, SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(inverse_view_projection) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        draw.frustum(to_glm(inverse_view_projection), to_glm(color), seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_debug_draw_grid(SturdyEngine engine, SturdyVec3 center, SturdyVec3 axis_u, SturdyVec3 axis_v, uint32_t half_cells, float cell_size,
                                                    SturdyVec4 color, float seconds) {
    return with_draw(engine, [&](SFT::Engine::DebugDraw &draw) -> SturdyResult {
        if (!finite(center) || !finite(axis_u) || !finite(axis_v) || !std::isfinite(cell_size) || !lifetime_ok(seconds)) {
            return invalid("arguments must be finite and seconds not negative");
        }
        // The grid emits (2 * half_cells + 1) lines per axis; bound it so one bad call cannot fill the segment budget.
        constexpr uint32_t max_half_cells = 1024;
        if (half_cells > max_half_cells) {
            return invalid("half_cells is too large (limit 1024)");
        }
        draw.grid(to_glm(center), to_glm(axis_u), to_glm(axis_v), half_cells, cell_size, to_glm(color), seconds);
        return STURDY_OK;
    });
}

} // extern "C"
