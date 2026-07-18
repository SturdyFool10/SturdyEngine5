#pragma once

#include <Foundation/Foundation.hpp>
#include <Renderer/Scene.hpp>

#include <array>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace SFT::Engine {

    enum class CameraProjectionMode : u8 {
        Perspective,
        Orthographic,
        Custom,
    };

    struct CameraRay {
        glm::vec3 origin{};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};
    };

    struct CameraAabb {
        glm::vec3 minimum{};
        glm::vec3 maximum{};
    };

    struct CameraPlane {
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        f32 distance = 0.0f;

        [[nodiscard]] f32 signed_distance(glm::vec3 point) const noexcept;
    };

    enum class CameraContainment : u8 {
        Outside,
        Intersecting,
        Inside,
    };

    struct CameraFrustum {
        enum PlaneIndex : usize {
            Left,
            Right,
            Bottom,
            Top,
            Near,
            Far,
            Count,
        };

        std::array<CameraPlane, Count> planes{};

        [[nodiscard]] bool contains(glm::vec3 point) const noexcept;
        [[nodiscard]] bool intersects_sphere(glm::vec3 center, f32 radius) const noexcept;
        [[nodiscard]] CameraContainment classify(const CameraAabb &bounds) const noexcept;
    };

    // A complete game-facing camera object. It owns transform, lens/projection, physical-camera and
    // temporal state while keeping renderer matrices as a derived implementation detail.
    class Camera {
      public:
        Camera() noexcept = default;

        [[nodiscard]] static Camera perspective(f32 vertical_fov_degrees = 60.0f,
                                                f32 aspect_ratio = 16.0f / 9.0f,
                                                f32 near_clip = 0.05f,
                                                f32 far_clip = 1000.0f) noexcept;
        [[nodiscard]] static Camera orthographic(f32 vertical_size = 10.0f,
                                                 f32 aspect_ratio = 16.0f / 9.0f,
                                                 f32 near_clip = 0.05f,
                                                 f32 far_clip = 1000.0f) noexcept;

        // Transform and basis. Orientation is local-camera -> world; the conventional camera looks
        // down local -Z with +Y up and +X right.
        [[nodiscard]] glm::vec3 position() const noexcept;
        void set_position(glm::vec3 position) noexcept;
        [[nodiscard]] glm::quat orientation() const noexcept;
        void set_orientation(glm::quat orientation) noexcept;
        [[nodiscard]] glm::vec3 euler_degrees() const noexcept;
        void set_euler_degrees(glm::vec3 pitch_yaw_roll_degrees) noexcept;
        [[nodiscard]] glm::vec3 forward() const noexcept;
        [[nodiscard]] glm::vec3 right() const noexcept;
        [[nodiscard]] glm::vec3 up() const noexcept;

        void look_at(glm::vec3 target, glm::vec3 world_up = {0.0f, 1.0f, 0.0f}) noexcept;
        void set_forward(glm::vec3 direction, glm::vec3 world_up = {0.0f, 1.0f, 0.0f}) noexcept;
        [[nodiscard]] f32 distance_to(glm::vec3 point) const noexcept;
        void translate_world(glm::vec3 delta) noexcept;
        void translate_local(glm::vec3 delta) noexcept;
        void move_forward(f32 distance) noexcept;
        void move_right(f32 distance) noexcept;
        void move_up(f32 distance) noexcept;
        void rotate_local(glm::quat rotation) noexcept;
        void rotate_world(glm::quat rotation) noexcept;
        void yaw_pitch_roll(f32 yaw_degrees, f32 pitch_degrees, f32 roll_degrees = 0.0f) noexcept;
        void orbit(glm::vec3 pivot, f32 yaw_degrees, f32 pitch_degrees) noexcept;
        void dolly(f32 distance) noexcept;
        void pan(glm::vec2 local_distance) noexcept;
        void zoom(f32 magnification) noexcept;
        void frame_sphere(glm::vec3 center, f32 radius, f32 padding = 1.1f) noexcept;
        void frame_bounds(const CameraAabb &bounds, f32 padding = 1.1f) noexcept;

        // Projection/lens.
        [[nodiscard]] CameraProjectionMode projection_mode() const noexcept;
        void set_perspective(f32 vertical_fov_degrees, f32 near_clip, f32 far_clip) noexcept;
        void set_orthographic(f32 vertical_size, f32 near_clip, f32 far_clip) noexcept;
        void set_custom_projection(glm::mat4 projection) noexcept;
        void clear_custom_projection() noexcept;

        [[nodiscard]] f32 aspect_ratio() const noexcept;
        void set_aspect_ratio(f32 aspect_ratio) noexcept;
        void set_viewport_size(u32 width, u32 height) noexcept;
        [[nodiscard]] f32 vertical_fov_degrees() const noexcept;
        void set_vertical_fov_degrees(f32 degrees) noexcept;
        [[nodiscard]] f32 horizontal_fov_degrees() const noexcept;
        void set_horizontal_fov_degrees(f32 degrees) noexcept;
        [[nodiscard]] f32 orthographic_vertical_size() const noexcept;
        void set_orthographic_vertical_size(f32 size) noexcept;
        [[nodiscard]] f32 near_clip() const noexcept;
        [[nodiscard]] f32 far_clip() const noexcept;
        void set_clip_planes(f32 near_clip, f32 far_clip) noexcept;
        [[nodiscard]] glm::vec2 lens_shift() const noexcept;
        void set_lens_shift(glm::vec2 shift) noexcept;
        [[nodiscard]] glm::vec2 jitter_ndc() const noexcept;
        void set_jitter_ndc(glm::vec2 jitter) noexcept;
        void set_jitter_pixels(glm::vec2 pixel_offset, glm::vec2 viewport_size) noexcept;
        void clear_jitter() noexcept;
        [[nodiscard]] bool reverse_z() const noexcept;
        void set_reverse_z(bool enabled) noexcept;
        [[nodiscard]] bool flip_projection_y() const noexcept;
        void set_flip_projection_y(bool enabled) noexcept;

        // Physical-camera metadata and useful conversions. Focal length and FOV stay synchronized;
        // aperture/shutter/ISO/focus are available to depth-of-field, motion-blur, and exposure systems.
        [[nodiscard]] f32 focal_length_mm() const noexcept;
        void set_focal_length_mm(f32 millimeters) noexcept;
        [[nodiscard]] glm::vec2 sensor_size_mm() const noexcept;
        void set_sensor_size_mm(glm::vec2 millimeters) noexcept;
        [[nodiscard]] f32 aperture_f_stop() const noexcept;
        void set_aperture_f_stop(f32 value) noexcept;
        [[nodiscard]] f32 shutter_seconds() const noexcept;
        void set_shutter_seconds(f32 seconds) noexcept;
        [[nodiscard]] f32 iso() const noexcept;
        void set_iso(f32 value) noexcept;
        [[nodiscard]] f32 focus_distance() const noexcept;
        void set_focus_distance(f32 distance) noexcept;
        [[nodiscard]] u32 aperture_blades() const noexcept;
        void set_aperture_blades(u32 blades) noexcept;
        [[nodiscard]] f32 exposure_compensation_ev() const noexcept;
        void set_exposure_compensation_ev(f32 ev) noexcept;
        [[nodiscard]] f32 exposure_multiplier() const noexcept;
        [[nodiscard]] f32 ev100() const noexcept;

        // View policy commonly consumed by culling/render orchestration.
        [[nodiscard]] u32 culling_mask() const noexcept;
        void set_culling_mask(u32 mask) noexcept;
        [[nodiscard]] i32 priority() const noexcept;
        void set_priority(i32 priority) noexcept;
        [[nodiscard]] bool active() const noexcept;
        void set_active(bool active) noexcept;
        [[nodiscard]] glm::vec4 clear_color() const noexcept;
        void set_clear_color(glm::vec4 color) noexcept;
        [[nodiscard]] f32 render_scale() const noexcept;
        void set_render_scale(f32 scale) noexcept;
        [[nodiscard]] glm::vec4 normalized_viewport() const noexcept;
        void set_normalized_viewport(glm::vec4 rectangle) noexcept;

        // Derived transforms and coordinate conversion. Screen coordinates use a top-left origin and
        // depth in [0,1], matching the engine's Vulkan/WebGPU-style projection contract.
        [[nodiscard]] glm::mat4 world_matrix() const noexcept;
        [[nodiscard]] glm::mat4 view_matrix() const noexcept;
        [[nodiscard]] glm::mat4 projection_matrix() const noexcept;
        [[nodiscard]] glm::mat4 view_projection_matrix() const noexcept;
        [[nodiscard]] glm::mat4 inverse_view_projection_matrix() const noexcept;
        [[nodiscard]] std::optional<glm::vec3> project(glm::vec3 world,
                                                       glm::vec2 viewport_size) const noexcept;
        [[nodiscard]] std::optional<glm::vec3> unproject(glm::vec3 screen,
                                                         glm::vec2 viewport_size) const noexcept;
        [[nodiscard]] std::optional<CameraRay> screen_ray(glm::vec2 pixel,
                                                          glm::vec2 viewport_size) const noexcept;
        [[nodiscard]] CameraFrustum frustum() const noexcept;
        [[nodiscard]] bool sees(glm::vec3 point) const noexcept;
        [[nodiscard]] bool sees_sphere(glm::vec3 center, f32 radius) const noexcept;
        [[nodiscard]] CameraContainment sees(const CameraAabb &bounds) const noexcept;

        // Temporal history for motion vectors/TAA. Call commit_frame() after submitting this camera.
        void commit_frame() noexcept;
        void reset_history() noexcept;
        [[nodiscard]] bool has_history() const noexcept;
        [[nodiscard]] glm::mat4 previous_view_projection_matrix() const noexcept;

        [[nodiscard]] SFT::Renderer::CameraView renderer_view() const noexcept;

      private:
        void synchronize_focal_length_from_fov() noexcept;
        void synchronize_fov_from_focal_length() noexcept;

        glm::vec3 position_{0.0f, 0.0f, 0.0f};
        glm::quat orientation_{1.0f, 0.0f, 0.0f, 0.0f};
        CameraProjectionMode projection_mode_ = CameraProjectionMode::Perspective;
        glm::mat4 custom_projection_{1.0f};
        f32 aspect_ratio_ = 16.0f / 9.0f;
        f32 vertical_fov_radians_ = 1.0471975512f;
        f32 orthographic_vertical_size_ = 10.0f;
        f32 near_clip_ = 0.05f;
        f32 far_clip_ = 1000.0f;
        glm::vec2 lens_shift_{0.0f};
        glm::vec2 jitter_ndc_{0.0f};
        bool reverse_z_ = false;
        bool flip_projection_y_ = true;

        f32 focal_length_mm_ = 20.7846f;
        glm::vec2 sensor_size_mm_{36.0f, 24.0f};
        f32 aperture_f_stop_ = 2.8f;
        f32 shutter_seconds_ = 1.0f / 60.0f;
        f32 iso_ = 100.0f;
        f32 focus_distance_ = 10.0f;
        u32 aperture_blades_ = 7;
        f32 exposure_compensation_ev_ = 0.0f;

        u32 culling_mask_ = ~0u;
        i32 priority_ = 0;
        bool active_ = true;
        glm::vec4 clear_color_{0.01f, 0.015f, 0.025f, 1.0f};
        f32 render_scale_ = 1.0f;
        glm::vec4 normalized_viewport_{0.0f, 0.0f, 1.0f, 1.0f};

        glm::mat4 previous_view_projection_{1.0f};
        bool has_history_ = false;
    };

} // namespace SFT::Engine
