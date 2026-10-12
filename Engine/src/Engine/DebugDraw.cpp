#include <Engine/DebugDraw.hpp>

#include <algorithm>

namespace SFT::Engine {

    template <class Fn>
    void DebugDraw::record(f32 seconds, Fn &&fill) {
        f32 width = 1.5f;
        {
            auto state = state_.lock();
            if (!state->enabled) {
                return;
            }
            width = state->width_px;
        }
        // Shapes are generated outside the lock: a sphere is a hundred segments of trigonometry.
        Renderer::DebugDrawList list;
        list.set_width(width);
        fill(list);
        submit(list, seconds);
    }

    void DebugDraw::set_enabled(bool enabled) {
        auto state = state_.lock();
        state->enabled = enabled;
        if (!enabled) {
            state->entries.clear();
            state->cached.reset();
        }
    }

    bool DebugDraw::enabled() const { return state_.lock()->enabled; }

    void DebugDraw::set_settings(const Renderer::DebugDrawSettings &settings) { state_.lock()->settings = settings; }

    Renderer::DebugDrawSettings DebugDraw::settings() const { return state_.lock()->settings; }

    void DebugDraw::set_width(f32 width_px) { state_.lock()->width_px = std::clamp(width_px, 0.5f, 64.0f); }

    void DebugDraw::line(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.line(a, b, color); });
    }

    void DebugDraw::polyline(std::span<const glm::vec3> points, const glm::vec4 &color, bool closed, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.polyline(points, color, closed); });
    }

    void DebugDraw::cross(const glm::vec3 &point, f32 size, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.cross(point, size, color); });
    }

    void DebugDraw::box(const glm::vec3 &min, const glm::vec3 &max, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.box(min, max, color); });
    }

    void DebugDraw::oriented_box(const glm::mat4 &transform, const glm::vec3 &half_extents, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.oriented_box(transform, half_extents, color); });
    }

    void DebugDraw::sphere(const glm::vec3 &center, f32 radius, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.sphere(center, radius, color); });
    }

    void DebugDraw::circle(const glm::vec3 &center, const glm::vec3 &normal, f32 radius, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.circle(center, normal, radius, color); });
    }

    void DebugDraw::capsule(const glm::vec3 &a, const glm::vec3 &b, f32 radius, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.capsule(a, b, radius, color); });
    }

    void DebugDraw::cone(const glm::vec3 &apex, const glm::vec3 &direction, f32 length, f32 half_angle_radians, const glm::vec4 &color,
                         f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.cone(apex, direction, length, half_angle_radians, color); });
    }

    void DebugDraw::arrow(const glm::vec3 &from, const glm::vec3 &to, const glm::vec4 &color, f32 head_size, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.arrow(from, to, color, head_size); });
    }

    void DebugDraw::axes(const glm::mat4 &transform, f32 size, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.axes(transform, size); });
    }

    void DebugDraw::frustum(const glm::mat4 &inverse_view_projection, const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.frustum(inverse_view_projection, color); });
    }

    void DebugDraw::grid(const glm::vec3 &center, const glm::vec3 &axis_u, const glm::vec3 &axis_v, u32 half_cells, f32 cell_size,
                         const glm::vec4 &color, f32 seconds) {
        record(seconds, [&](Renderer::DebugDrawList &list) { list.grid(center, axis_u, axis_v, half_cells, cell_size, color); });
    }

    void DebugDraw::submit(const Renderer::DebugDrawList &list, f32 seconds) {
        if (list.empty()) {
            return;
        }
        auto state = state_.lock();
        if (!state->enabled) {
            return;
        }
        // Bound the backlog: a runaway caller must not grow the list without limit between renders.
        const usize cap = static_cast<usize>(state->settings.max_lines) * 2;
        if (state->entries.size() >= cap) {
            return;
        }
        const f64 expires = seconds > 0.0f ? state->time + static_cast<f64>(seconds) : -1.0;
        state->entries.reserve(state->entries.size() + list.size());
        for (const Renderer::DebugLine &line : list.lines()) {
            state->entries.push_back(Entry{.line = line, .expires = expires});
        }
        state->cached.reset();
    }

    void DebugDraw::clear() {
        auto state = state_.lock();
        state->entries.clear();
        state->cached.reset();
    }

    usize DebugDraw::line_count() const { return state_.lock()->entries.size(); }

    void DebugDraw::advance(f64 delta_seconds) {
        auto state = state_.lock();
        state->time += std::max(delta_seconds, 0.0);
        const f64 now = state->time;
        const usize before = state->entries.size();
        std::erase_if(state->entries, [now](const Entry &entry) { return entry.expires < 0.0 || entry.expires <= now; });
        if (state->entries.size() != before) {
            state->cached.reset();
        }
    }

    std::shared_ptr<const std::vector<Renderer::DebugLine>> DebugDraw::snapshot() {
        auto state = state_.lock();
        if (!state->enabled || state->entries.empty()) {
            return nullptr;
        }
        if (!state->cached) {
            auto lines = std::make_shared<std::vector<Renderer::DebugLine>>();
            lines->reserve(state->entries.size());
            for (const Entry &entry : state->entries) {
                lines->push_back(entry.line);
            }
            state->cached = std::move(lines);
        }
        return state->cached;
    }

} // namespace SFT::Engine
