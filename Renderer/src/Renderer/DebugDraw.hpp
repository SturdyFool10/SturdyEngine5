#pragma once

#include <Foundation/Foundation.hpp>

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Async/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Scene.hpp>

/// Debug drawing: lines, boxes, spheres, arrows, frusta and friends, drawn over the finished frame with a depth test against
/// the scene (hidden stretches are dimmed, not lost). For physics, audio, AI and gameplay debugging, and anything else that needs
/// to *see* a number.
///
/// Two halves that know nothing of each other:
///
/// * `DebugDrawList` records shapes as thick line segments. Pure CPU, no GPU, no renderer: use it anywhere, test it anywhere.
/// * `DebugDrawRenderer` turns a snapshot of a list into an `OverlayPass` (one draw call for the whole frame).
///
/// `Engine::DebugDraw` (Engine/DebugDraw.hpp) wraps both with a lifetime per shape, so gameplay code just calls
/// `engine.debug_draw().sphere(...)`.
namespace SFT::Renderer {

    /// Packs an rgba colour (0..1 per channel) into the line colour word (r in the low byte).
    [[nodiscard]] u32 pack_debug_color(const glm::vec4 &rgba) noexcept;

    /// One segment as uploaded to the GPU (`DebugLineInstance` in Shaders/debug_lines.slang).
    struct DebugLine {
        glm::vec3 a{0.0f};
        /// Width in screen pixels.
        f32 width_px = 1.5f;
        glm::vec3 b{0.0f};
        /// `pack_debug_color`.
        u32 color = 0xffffffffu;
    };
    static_assert(sizeof(DebugLine) == 32);

    /// Records debug shapes as line segments.
    class DebugDrawList {
      public:
        /// Width, in pixels, of every shape recorded after this call.
        void set_width(f32 width_px) noexcept { width_px_ = width_px; }
        [[nodiscard]] f32 width() const noexcept { return width_px_; }

        void line(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color);
        /// Connected segments; `closed` joins the last point back to the first.
        void polyline(std::span<const glm::vec3> points, const glm::vec4 &color, bool closed = false);
        /// A small 3-axis cross, `size` across.
        void cross(const glm::vec3 &point, f32 size, const glm::vec4 &color);
        /// An axis-aligned box.
        void box(const glm::vec3 &min, const glm::vec3 &max, const glm::vec4 &color);
        /// A box `half_extents` around the origin, placed by `transform` (rotation, scale and translation).
        void oriented_box(const glm::mat4 &transform, const glm::vec3 &half_extents, const glm::vec4 &color);
        /// Three orthogonal great circles.
        void sphere(const glm::vec3 &center, f32 radius, const glm::vec4 &color, u32 segments = 24);
        /// A circle in the plane whose normal is `normal`.
        void circle(const glm::vec3 &center, const glm::vec3 &normal, f32 radius, const glm::vec4 &color, u32 segments = 32);
        /// An arc of the circle spanned by `axis_x`/`axis_y` (unit, perpendicular), from `start_radians` to `end_radians`.
        void arc(const glm::vec3 &center, const glm::vec3 &axis_x, const glm::vec3 &axis_y, f32 radius, f32 start_radians,
                 f32 end_radians, const glm::vec4 &color, u32 segments = 16);
        /// Two end caps joined by four lines: a capsule around the segment a-b.
        void capsule(const glm::vec3 &a, const glm::vec3 &b, f32 radius, const glm::vec4 &color, u32 segments = 16);
        /// A cone from `apex` along `direction` (any length), `length` long, opening `half_angle_radians`: a spot light's volume.
        void cone(const glm::vec3 &apex, const glm::vec3 &direction, f32 length, f32 half_angle_radians, const glm::vec4 &color,
                  u32 segments = 24);
        /// A line with an arrow head at `to`; `head_size` is the head length (0 = a fifth of the arrow).
        void arrow(const glm::vec3 &from, const glm::vec3 &to, const glm::vec4 &color, f32 head_size = 0.0f);
        /// The red/green/blue X/Y/Z axes of `transform`, each `size` long.
        void axes(const glm::mat4 &transform, f32 size);
        /// A camera frustum from its inverse view-projection (depth 0 at the near plane, 1 at the far plane).
        void frustum(const glm::mat4 &inverse_view_projection, const glm::vec4 &color);
        /// A grid of `half_cells` x `half_cells` cells either side of `center`, spanned by `axis_u`/`axis_v` (unit).
        void grid(const glm::vec3 &center, const glm::vec3 &axis_u, const glm::vec3 &axis_v, u32 half_cells, f32 cell_size,
                  const glm::vec4 &color);

        void clear() noexcept { lines_.clear(); }
        void reserve(usize count) { lines_.reserve(count); }
        void append(std::span<const DebugLine> lines) { lines_.insert(lines_.end(), lines.begin(), lines.end()); }
        [[nodiscard]] std::span<const DebugLine> lines() const noexcept { return lines_; }
        [[nodiscard]] usize size() const noexcept { return lines_.size(); }
        [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }
        /// Moves the recorded segments out (the list is left empty).
        [[nodiscard]] std::vector<DebugLine> take() noexcept { return std::move(lines_); }

      private:
        std::vector<DebugLine> lines_;
        f32 width_px_ = 1.5f;
    };

    struct DebugDrawSettings {
        /// Test against the scene depth (when the frame has a scene).
        bool depth_test = true;
        /// Alpha multiplier for stretches behind geometry (0 hides them).
        f32 occluded_alpha = 0.25f;
        /// Segments beyond this are dropped (the cost of the whole feature is linear in this).
        u32 max_lines = 1u << 18;
    };

    /// Owns the line pipelines (one per target format and depth variant) and builds the overlay pass. Keep it alive as long as
    /// any overlay it made can still run. Call `release` while the device is still alive to free the GPU objects; destruction
    /// alone never touches the device (the renderer may already have replaced or torn it down, e.g. after device loss), and a
    /// device change just forgets the old handles.
    class DebugDrawRenderer {
      public:
        DebugDrawRenderer() = default;
        DebugDrawRenderer(const DebugDrawRenderer &) = delete;
        DebugDrawRenderer &operator=(const DebugDrawRenderer &) = delete;

        /// An overlay drawing `lines` as seen through `view_projection`. The pass keeps `lines` alive. Frames without a scene
        /// (overlay-only) draw without a depth test.
        [[nodiscard]] OverlayPass make_overlay(std::shared_ptr<const std::vector<DebugLine>> lines, const glm::mat4 &view_projection,
                                               f32 near_plane, const DebugDrawSettings &settings);

        /// Destroys every pipeline on the device that built them, which must still be alive. Safe to call twice.
        void release() noexcept;

      private:
        struct Pipeline {
            RHI::ShaderModuleHandle vertex_module{};
            RHI::ShaderModuleHandle fragment_module{};
            RHI::BindGroupLayoutHandle bind_group_layout{};
            u32 bind_group_set = 0;
            u32 lines_binding = 0;
            u32 depth_binding = 0;
            bool has_depth_binding = false;
            RHI::PipelineLayoutHandle pipeline_layout{};
            RHI::RenderPipelineHandle pipeline{};
        };
        struct FrameState;

        [[nodiscard]] Core::RendererExpected<const Pipeline *> pipeline_for(RHI::RhiDevice &device, RHI::Format format, bool with_depth);
        void destroy_pipeline(RHI::RhiDevice &device, Pipeline &pipeline) noexcept;

        Async::Mutex<std::unordered_map<u64, std::unique_ptr<Pipeline>>> pipelines_;
        RHI::RhiDevice *device_ = nullptr;
    };

} // namespace SFT::Renderer
