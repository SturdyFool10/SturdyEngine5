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

        /// Performs the signed distance operation for `CameraPlane` using the supplied arguments.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Reports whether contains holds for this `CameraFrustum`.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(glm::vec3 point) const noexcept;
        /// Performs the intersects sphere operation for `CameraFrustum` using the supplied arguments.
        ///
        /// @param center `center` value used by the operation.
        /// @param radius `radius` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool intersects_sphere(glm::vec3 center, f32 radius) const noexcept;
        /// Performs the classify operation for `CameraFrustum` using the supplied arguments.
        ///
        /// @param bounds `bounds` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CameraContainment classify(const CameraAabb &bounds) const noexcept;
    };


    class Camera {
      public:
        /// Constructs a `Camera` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Camera() noexcept = default;

        /// Performs the perspective operation for `Camera` using the supplied arguments.
        ///
        /// @param vertical_fov_degrees `vertical_fov_degrees` value used by the operation.
        /// @param aspect_ratio `aspect_ratio` value used by the operation.
        /// @param near_clip `near_clip` value used by the operation.
        /// @param far_clip `far_clip` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static Camera perspective(f32 vertical_fov_degrees = 60.0f,
                                                f32 aspect_ratio = 16.0f / 9.0f,
                                                f32 near_clip = 0.05f,
                                                f32 far_clip = 1000.0f) noexcept;
        /// Performs the orthographic operation for `Camera` using the supplied arguments.
        ///
        /// @param vertical_size Requested or available size for the operation.
        /// @param aspect_ratio `aspect_ratio` value used by the operation.
        /// @param near_clip `near_clip` value used by the operation.
        /// @param far_clip `far_clip` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static Camera orthographic(f32 vertical_size = 10.0f,
                                                 f32 aspect_ratio = 16.0f / 9.0f,
                                                 f32 near_clip = 0.05f,
                                                 f32 far_clip = 1000.0f) noexcept;


        /// Returns the current or globally available position value.
        ///
        /// @return Returns the current position value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 position() const noexcept;
        /// Sets the position for this `Camera`.
        ///
        /// @param position `position` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_position(glm::vec3 position) noexcept;
        /// Returns the current or globally available orientation value.
        ///
        /// @return Returns the current orientation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::quat orientation() const noexcept;
        /// Sets the orientation for this `Camera`.
        ///
        /// @param orientation `orientation` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_orientation(glm::quat orientation) noexcept;
        /// Returns the current or globally available euler degrees value.
        ///
        /// @return Returns the current euler degrees value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 euler_degrees() const noexcept;
        /// Sets the euler degrees for this `Camera`.
        ///
        /// @param pitch_yaw_roll_degrees `pitch_yaw_roll_degrees` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_euler_degrees(glm::vec3 pitch_yaw_roll_degrees) noexcept;
        /// Returns the current or globally available forward value.
        ///
        /// @return Returns the current forward value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 forward() const noexcept;
        /// Returns the current or globally available right value.
        ///
        /// @return Returns the current right value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 right() const noexcept;
        /// Returns the current or globally available up value.
        ///
        /// @return Returns the current up value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec3 up() const noexcept;

        /// Performs the look at operation for `Camera` using the supplied arguments.
        ///
        /// @param target `target` value used by the operation.
        /// @param world_up World used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void look_at(glm::vec3 target, glm::vec3 world_up = {0.0f, 1.0f, 0.0f}) noexcept;
        /// Sets the forward for this `Camera`.
        ///
        /// @param direction `direction` value used by the operation.
        /// @param world_up World used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_forward(glm::vec3 direction, glm::vec3 world_up = {0.0f, 1.0f, 0.0f}) noexcept;
        /// Performs the distance to operation for `Camera` using the supplied arguments.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 distance_to(glm::vec3 point) const noexcept;
        /// Translates world using the supplied arguments and current state.
        ///
        /// @param delta `delta` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void translate_world(glm::vec3 delta) noexcept;
        /// Translates local using the supplied arguments and current state.
        ///
        /// @param delta `delta` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void translate_local(glm::vec3 delta) noexcept;
        /// Moves forward using the supplied arguments and current state.
        ///
        /// @param distance `distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_forward(f32 distance) noexcept;
        /// Moves right using the supplied arguments and current state.
        ///
        /// @param distance `distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_right(f32 distance) noexcept;
        /// Moves up using the supplied arguments and current state.
        ///
        /// @param distance `distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void move_up(f32 distance) noexcept;
        /// Performs the rotate local operation for `Camera` using the supplied arguments.
        ///
        /// @param rotation `rotation` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void rotate_local(glm::quat rotation) noexcept;
        /// Performs the rotate world operation for `Camera` using the supplied arguments.
        ///
        /// @param rotation `rotation` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void rotate_world(glm::quat rotation) noexcept;
        /// Performs the yaw pitch roll operation for `Camera` using the supplied arguments.
        ///
        /// @param yaw_degrees `yaw_degrees` value used by the operation.
        /// @param pitch_degrees `pitch_degrees` value used by the operation.
        /// @param roll_degrees `roll_degrees` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void yaw_pitch_roll(f32 yaw_degrees, f32 pitch_degrees, f32 roll_degrees = 0.0f) noexcept;
        /// Performs the orbit operation for `Camera` using the supplied arguments.
        ///
        /// @param pivot `pivot` value used by the operation.
        /// @param yaw_degrees `yaw_degrees` value used by the operation.
        /// @param pitch_degrees `pitch_degrees` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void orbit(glm::vec3 pivot, f32 yaw_degrees, f32 pitch_degrees) noexcept;
        /// Performs the dolly operation for `Camera` using the supplied arguments.
        ///
        /// @param distance `distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void dolly(f32 distance) noexcept;
        /// Performs the pan operation for `Camera` using the supplied arguments.
        ///
        /// @param local_distance `local_distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void pan(glm::vec2 local_distance) noexcept;
        /// Performs the zoom operation for `Camera` using the supplied arguments.
        ///
        /// @param magnification `magnification` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void zoom(f32 magnification) noexcept;
        /// Performs the frame sphere operation for `Camera` using the supplied arguments.
        ///
        /// @param center `center` value used by the operation.
        /// @param radius `radius` value used by the operation.
        /// @param padding `padding` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void frame_sphere(glm::vec3 center, f32 radius, f32 padding = 1.1f) noexcept;
        /// Performs the frame bounds operation for `Camera` using the supplied arguments.
        ///
        /// @param bounds `bounds` value used by the operation.
        /// @param padding `padding` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void frame_bounds(const CameraAabb &bounds, f32 padding = 1.1f) noexcept;


        /// Returns the current or globally available projection mode value.
        ///
        /// @return Returns the current projection mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CameraProjectionMode projection_mode() const noexcept;
        /// Sets the perspective for this `Camera`.
        ///
        /// @param vertical_fov_degrees `vertical_fov_degrees` value used by the operation.
        /// @param near_clip `near_clip` value used by the operation.
        /// @param far_clip `far_clip` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_perspective(f32 vertical_fov_degrees, f32 near_clip, f32 far_clip) noexcept;
        /// Sets the orthographic for this `Camera`.
        ///
        /// @param vertical_size Requested or available size for the operation.
        /// @param near_clip `near_clip` value used by the operation.
        /// @param far_clip `far_clip` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_orthographic(f32 vertical_size, f32 near_clip, f32 far_clip) noexcept;
        /// Sets the custom projection for this `Camera`.
        ///
        /// @param projection `projection` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_custom_projection(glm::mat4 projection) noexcept;
        /// Clears custom projection.
        ///
        /// @note This function does not throw exceptions.
        void clear_custom_projection() noexcept;

        /// Returns the current or globally available aspect ratio value.
        ///
        /// @return Returns the current aspect ratio value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 aspect_ratio() const noexcept;
        /// Sets the aspect ratio for this `Camera`.
        ///
        /// @param aspect_ratio `aspect_ratio` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_aspect_ratio(f32 aspect_ratio) noexcept;
        /// Sets the viewport size for this `Camera`.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @note This function does not throw exceptions.
        void set_viewport_size(u32 width, u32 height) noexcept;
        /// Returns the current or globally available vertical fov degrees value.
        ///
        /// @return Returns the current vertical fov degrees value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 vertical_fov_degrees() const noexcept;
        /// Sets the vertical fov degrees for this `Camera`.
        ///
        /// @param degrees `degrees` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_vertical_fov_degrees(f32 degrees) noexcept;
        /// Returns the current or globally available horizontal fov degrees value.
        ///
        /// @return Returns the current horizontal fov degrees value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 horizontal_fov_degrees() const noexcept;
        /// Sets the horizontal fov degrees for this `Camera`.
        ///
        /// @param degrees `degrees` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_horizontal_fov_degrees(f32 degrees) noexcept;
        /// Returns the orthographic vertical size for this `Camera`.
        ///
        /// @return Returns the current orthographic vertical size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 orthographic_vertical_size() const noexcept;
        /// Sets the orthographic vertical size for this `Camera`.
        ///
        /// @param size Requested or available size for the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_orthographic_vertical_size(f32 size) noexcept;
        /// Returns the current or globally available near clip value.
        ///
        /// @return Returns the current near clip value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 near_clip() const noexcept;
        /// Returns the current or globally available far clip value.
        ///
        /// @return Returns the current far clip value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 far_clip() const noexcept;
        /// Sets the clip planes for this `Camera`.
        ///
        /// @param near_clip `near_clip` value used by the operation.
        /// @param far_clip `far_clip` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_clip_planes(f32 near_clip, f32 far_clip) noexcept;
        /// Returns the current or globally available lens shift value.
        ///
        /// @return Returns the current lens shift value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 lens_shift() const noexcept;
        /// Sets the lens shift for this `Camera`.
        ///
        /// @param shift `shift` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_lens_shift(glm::vec2 shift) noexcept;
        /// Returns the current or globally available jitter ndc value.
        ///
        /// @return Returns the current jitter ndc value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 jitter_ndc() const noexcept;
        /// Sets the jitter ndc for this `Camera`.
        ///
        /// @param jitter `jitter` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_jitter_ndc(glm::vec2 jitter) noexcept;
        /// Sets the jitter pixels for this `Camera`.
        ///
        /// @param pixel_offset Offset from the beginning of the relevant range or buffer.
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_jitter_pixels(glm::vec2 pixel_offset, glm::vec2 viewport_size) noexcept;
        /// Clears jitter.
        ///
        /// @note This function does not throw exceptions.
        void clear_jitter() noexcept;
        /// Returns the current or globally available reverse z value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool reverse_z() const noexcept;
        /// Sets the reverse z for this `Camera`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void set_reverse_z(bool enabled) noexcept;


        /// Returns the current or globally available focal length mm value.
        ///
        /// @return Returns the current focal length mm value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 focal_length_mm() const noexcept;
        /// Sets the focal length mm for this `Camera`.
        ///
        /// @param millimeters `millimeters` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_focal_length_mm(f32 millimeters) noexcept;
        /// Returns the current or globally available sensor size mm value.
        ///
        /// @return Returns the current sensor size mm value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 sensor_size_mm() const noexcept;
        /// Sets the sensor size mm for this `Camera`.
        ///
        /// @param millimeters `millimeters` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_sensor_size_mm(glm::vec2 millimeters) noexcept;
        /// Returns the current or globally available aperture f stop value.
        ///
        /// @return Returns the current aperture f stop value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 aperture_f_stop() const noexcept;
        /// Sets the aperture f stop for this `Camera`.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_aperture_f_stop(f32 value) noexcept;
        /// Returns the current or globally available shutter seconds value.
        ///
        /// @return Returns the current shutter seconds value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 shutter_seconds() const noexcept;
        /// Sets the shutter seconds for this `Camera`.
        ///
        /// @param seconds `seconds` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_shutter_seconds(f32 seconds) noexcept;
        /// Returns the current or globally available iso value.
        ///
        /// @return Returns the current iso value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 iso() const noexcept;
        /// Sets the iso for this `Camera`.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_iso(f32 value) noexcept;
        /// Returns the current or globally available focus distance value.
        ///
        /// @return Returns the current focus distance value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 focus_distance() const noexcept;
        /// Sets the focus distance for this `Camera`.
        ///
        /// @param distance `distance` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_focus_distance(f32 distance) noexcept;
        /// Returns the current or globally available aperture blades value.
        ///
        /// @return Returns the current aperture blades value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 aperture_blades() const noexcept;
        /// Sets the aperture blades for this `Camera`.
        ///
        /// @param blades `blades` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_aperture_blades(u32 blades) noexcept;
        /// Returns the current or globally available exposure compensation ev value.
        ///
        /// @return Returns the current exposure compensation ev value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 exposure_compensation_ev() const noexcept;
        /// Sets the exposure compensation ev for this `Camera`.
        ///
        /// @param ev `ev` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_exposure_compensation_ev(f32 ev) noexcept;
        /// Returns the current or globally available exposure multiplier value.
        ///
        /// @return Returns the current exposure multiplier value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 exposure_multiplier() const noexcept;
        /// Returns the current or globally available ev100 value.
        ///
        /// @return Returns the current ev100 value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 ev100() const noexcept;


        /// Returns the current or globally available culling mask value.
        ///
        /// @return Returns the current culling mask value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 culling_mask() const noexcept;
        /// Sets the culling mask for this `Camera`.
        ///
        /// @param mask `mask` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_culling_mask(u32 mask) noexcept;
        /// Returns the current or globally available priority value.
        ///
        /// @return Returns the current priority value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 priority() const noexcept;
        /// Sets the priority for this `Camera`.
        ///
        /// @param priority `priority` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_priority(i32 priority) noexcept;
        /// Returns the current or globally available active value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool active() const noexcept;
        /// Sets the active for this `Camera`.
        ///
        /// @param active `active` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_active(bool active) noexcept;
        /// Clears color.
        ///
        /// @return Returns the current clear color value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec4 clear_color() const noexcept;
        /// Sets the clear color for this `Camera`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_clear_color(glm::vec4 color) noexcept;
        /// Renders scale using the current rendering state.
        ///
        /// @return Returns the current render scale value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 render_scale() const noexcept;
        /// Sets the render scale for this `Camera`.
        ///
        /// @param scale `scale` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_render_scale(f32 scale) noexcept;
        /// Returns the current or globally available normalized viewport value.
        ///
        /// @return Returns the current normalized viewport value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec4 normalized_viewport() const noexcept;
        /// Sets the normalized viewport for this `Camera`.
        ///
        /// @param rectangle `rectangle` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_normalized_viewport(glm::vec4 rectangle) noexcept;

        // Derived transforms and coordinate conversion. Screen coordinates use a top-left origin and
        // depth in [0,1].
        /// Returns the current or globally available world matrix value.
        ///
        /// @return Returns the current world matrix value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 world_matrix() const noexcept;
        /// Returns the current or globally available view matrix value.
        ///
        /// @return Returns the current view matrix value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 view_matrix() const noexcept;
        /// Returns the current or globally available projection matrix value.
        ///
        /// @return glm::perspectiveRH_ZO/orthoRH_ZO's native +Y-up NDC, unmodified.
        /// @note Every backend reconciles its own native clip-space convention against this one
        ///       value (D3D12 needs nothing; Vulkan flips its viewport — see RHI::Viewport's own doc
        ///       comment), so this matrix is never backend-flipped, and no caller should flip it
        ///       either.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 projection_matrix() const noexcept;
        /// Returns the current or globally available view projection matrix value.
        ///
        /// @return Returns the current view projection matrix value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 view_projection_matrix() const noexcept;
        /// Returns the current or globally available inverse view projection matrix value.
        ///
        /// @return Returns the current inverse view projection matrix value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 inverse_view_projection_matrix() const noexcept;
        /// Performs the project operation for `Camera` using the supplied arguments.
        ///
        /// @param world World used or affected by the operation.
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<glm::vec3> project(glm::vec3 world,
                                                       glm::vec2 viewport_size) const noexcept;
        /// Performs the unproject operation for `Camera` using the supplied arguments.
        ///
        /// @param screen `screen` value used by the operation.
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<glm::vec3> unproject(glm::vec3 screen,
                                                         glm::vec2 viewport_size) const noexcept;
        /// Performs the screen ray operation for `Camera` using the supplied arguments.
        ///
        /// @param pixel `pixel` value used by the operation.
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<CameraRay> screen_ray(glm::vec2 pixel,
                                                          glm::vec2 viewport_size) const noexcept;
        /// Returns the current or globally available frustum value.
        ///
        /// @return Returns the current frustum value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CameraFrustum frustum() const noexcept;
        /// Performs the sees operation for `Camera` using the supplied arguments.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool sees(glm::vec3 point) const noexcept;
        /// Performs the sees sphere operation for `Camera` using the supplied arguments.
        ///
        /// @param center `center` value used by the operation.
        /// @param radius `radius` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool sees_sphere(glm::vec3 center, f32 radius) const noexcept;
        /// Performs the sees operation for `Camera` using the supplied arguments.
        ///
        /// @param bounds `bounds` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] CameraContainment sees(const CameraAabb &bounds) const noexcept;


        /// Performs the commit frame operation for `Camera` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void commit_frame() noexcept;
        /// Resets history to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset_history() noexcept;
        /// Reports whether this `Camera` has history.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_history() const noexcept;
        /// Returns the current or globally available previous view projection matrix value.
        ///
        /// @return Returns the current previous view projection matrix value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::mat4 previous_view_projection_matrix() const noexcept;

        /// Renders view using the current rendering state.
        ///
        /// @return Returns the current renderer view value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::Renderer::CameraView renderer_view() const noexcept;

      private:
        /// Performs the synchronize focal length from fov operation for `Camera` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void synchronize_focal_length_from_fov() noexcept;
        /// Performs the synchronize fov from focal length operation for `Camera` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
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
