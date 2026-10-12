#pragma once

#include <Foundation/Foundation.hpp>

#include <memory>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Async/Async.hpp>
#include <Renderer/DebugDraw.hpp>

namespace SFT::Engine {

    /// Draw lines, boxes, spheres, arrows and friends into the world from anywhere in game code, and see them over the frame:
    ///
    /// ```cpp
    /// engine.debug_draw().sphere(hit_point, 0.1f, {1, 0.2f, 0.2f, 1});          // this frame only
    /// engine.debug_draw().arrow(origin, origin + velocity, {0.2f, 1, 0.2f, 1}, 0, 2.0f);  // stays for two seconds
    /// ```
    ///
    /// Every call takes a `seconds` lifetime: 0 (the default) shows the shape until the next update, so a call made every
    /// update draws a steady image; a positive value keeps it for that many (unscaled) seconds, whatever the game does next.
    /// Hidden stretches (behind geometry) are drawn dimmed, not skipped (`set_settings`). Safe to call from any thread.
    /// Shapes are projected with the camera's own matrices: a vertex-warp fisheye lens (`CameraEmulation`) bends the world but
    /// not these lines.
    class DebugDraw {
      public:
        /// Master switch; with it off, calls are ignored and nothing is drawn.
        void set_enabled(bool enabled);
        [[nodiscard]] bool enabled() const;
        void set_settings(const Renderer::DebugDrawSettings &settings);
        [[nodiscard]] Renderer::DebugDrawSettings settings() const;
        /// Line width, in pixels, of the shapes drawn after this call.
        void set_width(f32 width_px);

        void line(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color, f32 seconds = 0.0f);
        void polyline(std::span<const glm::vec3> points, const glm::vec4 &color, bool closed = false, f32 seconds = 0.0f);
        void cross(const glm::vec3 &point, f32 size, const glm::vec4 &color, f32 seconds = 0.0f);
        void box(const glm::vec3 &min, const glm::vec3 &max, const glm::vec4 &color, f32 seconds = 0.0f);
        void oriented_box(const glm::mat4 &transform, const glm::vec3 &half_extents, const glm::vec4 &color, f32 seconds = 0.0f);
        void sphere(const glm::vec3 &center, f32 radius, const glm::vec4 &color, f32 seconds = 0.0f);
        void circle(const glm::vec3 &center, const glm::vec3 &normal, f32 radius, const glm::vec4 &color, f32 seconds = 0.0f);
        void capsule(const glm::vec3 &a, const glm::vec3 &b, f32 radius, const glm::vec4 &color, f32 seconds = 0.0f);
        void cone(const glm::vec3 &apex, const glm::vec3 &direction, f32 length, f32 half_angle_radians, const glm::vec4 &color,
                  f32 seconds = 0.0f);
        void arrow(const glm::vec3 &from, const glm::vec3 &to, const glm::vec4 &color, f32 head_size = 0.0f, f32 seconds = 0.0f);
        void axes(const glm::mat4 &transform, f32 size, f32 seconds = 0.0f);
        void frustum(const glm::mat4 &inverse_view_projection, const glm::vec4 &color, f32 seconds = 0.0f);
        void grid(const glm::vec3 &center, const glm::vec3 &axis_u, const glm::vec3 &axis_v, u32 half_cells, f32 cell_size,
                  const glm::vec4 &color, f32 seconds = 0.0f);
        /// Submits a list recorded elsewhere (e.g. by a physics or audio debug visualiser).
        void submit(const Renderer::DebugDrawList &list, f32 seconds = 0.0f);

        void clear();
        /// How many segments are currently alive.
        [[nodiscard]] usize line_count() const;

        /// Advances time by `delta_seconds`: removes this-frame-only shapes and the ones whose lifetime ran out. The engine
        /// calls this at the start of every update.
        void advance(f64 delta_seconds);
        /// An immutable copy of what is alive now (null when disabled or empty); cheap when nothing changed since the last call.
        [[nodiscard]] std::shared_ptr<const std::vector<Renderer::DebugLine>> snapshot();

      private:
        struct Entry {
            Renderer::DebugLine line;
            /// Time after which the segment is removed; negative = until the next `advance`.
            f64 expires = -1.0;
        };
        struct State {
            std::vector<Entry> entries;
            f64 time = 0.0;
            f32 width_px = 1.5f;
            bool enabled = true;
            Renderer::DebugDrawSettings settings{};
            std::shared_ptr<const std::vector<Renderer::DebugLine>> cached;
        };

        /// A fresh list that uses the current line width, or an empty one when disabled (`record` then does nothing).
        template <class Fn>
        void record(f32 seconds, Fn &&fill);

        mutable Async::Mutex<State> state_;
    };

} // namespace SFT::Engine
