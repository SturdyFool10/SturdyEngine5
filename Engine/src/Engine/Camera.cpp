#include "Camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/quaternion_common.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/ext/quaternion_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace SFT::Engine {

    namespace {

        constexpr f32 minimum_near = 0.0001f;
        constexpr f32 minimum_fov_degrees = 1.0f;
        constexpr f32 maximum_fov_degrees = 179.0f;

        [[nodiscard]] glm::quat safe_normalize(glm::quat value) noexcept {
            const f32 length_squared = glm::dot(value, value);
            if (!std::isfinite(length_squared) || length_squared <= std::numeric_limits<f32>::epsilon()) {
                return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            }
            return glm::normalize(value);
        }

        [[nodiscard]] glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) noexcept {
            const f32 length_squared = glm::dot(value, value);
            if (!std::isfinite(length_squared) || length_squared <= std::numeric_limits<f32>::epsilon()) {
                return fallback;
            }
            return glm::normalize(value);
        }

        [[nodiscard]] CameraPlane normalized_plane(glm::vec4 equation) noexcept {
            const glm::vec3 normal{equation};
            const f32 length = glm::length(normal);
            if (!std::isfinite(length) || length <= std::numeric_limits<f32>::epsilon()) {
                return {};
            }
            return CameraPlane{.normal = normal / length, .distance = equation.w / length};
        }

        [[nodiscard]] glm::vec4 matrix_row(const glm::mat4 &matrix, usize row) noexcept {
            return {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
        }

    } // namespace

    f32 CameraPlane::signed_distance(glm::vec3 point) const noexcept {
        return glm::dot(normal, point) + distance;
    }

    bool CameraFrustum::contains(glm::vec3 point) const noexcept {
        return std::ranges::all_of(planes, [point](const CameraPlane &plane) {
            return plane.signed_distance(point) >= 0.0f;
        });
    }

    bool CameraFrustum::intersects_sphere(glm::vec3 center, f32 radius) const noexcept {
        const f32 safe_radius = std::max(radius, 0.0f);
        return std::ranges::all_of(planes, [center, safe_radius](const CameraPlane &plane) {
            return plane.signed_distance(center) >= -safe_radius;
        });
    }

    CameraContainment CameraFrustum::classify(const CameraAabb &bounds) const noexcept {
        bool intersects = false;
        for (const CameraPlane &plane : planes) {
            const glm::vec3 positive{
                plane.normal.x >= 0.0f ? bounds.maximum.x : bounds.minimum.x,
                plane.normal.y >= 0.0f ? bounds.maximum.y : bounds.minimum.y,
                plane.normal.z >= 0.0f ? bounds.maximum.z : bounds.minimum.z,
            };
            if (plane.signed_distance(positive) < 0.0f) {
                return CameraContainment::Outside;
            }
            const glm::vec3 negative{
                plane.normal.x >= 0.0f ? bounds.minimum.x : bounds.maximum.x,
                plane.normal.y >= 0.0f ? bounds.minimum.y : bounds.maximum.y,
                plane.normal.z >= 0.0f ? bounds.minimum.z : bounds.maximum.z,
            };
            intersects |= plane.signed_distance(negative) < 0.0f;
        }
        return intersects ? CameraContainment::Intersecting : CameraContainment::Inside;
    }

    Camera Camera::perspective(f32 vertical_fov_degrees, f32 aspect_ratio,
                               f32 near_clip, f32 far_clip) noexcept {
        Camera camera;
        camera.set_aspect_ratio(aspect_ratio);
        camera.set_perspective(vertical_fov_degrees, near_clip, far_clip);
        return camera;
    }

    Camera Camera::orthographic(f32 vertical_size, f32 aspect_ratio,
                                f32 near_clip, f32 far_clip) noexcept {
        Camera camera;
        camera.set_aspect_ratio(aspect_ratio);
        camera.set_orthographic(vertical_size, near_clip, far_clip);
        return camera;
    }

    glm::vec3 Camera::position() const noexcept { return position_; }
    void Camera::set_position(glm::vec3 position) noexcept { position_ = position; }
    glm::quat Camera::orientation() const noexcept { return orientation_; }
    void Camera::set_orientation(glm::quat orientation) noexcept { orientation_ = safe_normalize(orientation); }

    glm::vec3 Camera::euler_degrees() const noexcept {
        return glm::degrees(glm::eulerAngles(orientation_));
    }

    void Camera::set_euler_degrees(glm::vec3 pitch_yaw_roll_degrees) noexcept {
        orientation_ = safe_normalize(glm::quat{glm::radians(pitch_yaw_roll_degrees)});
    }

    glm::vec3 Camera::forward() const noexcept { return orientation_ * glm::vec3{0.0f, 0.0f, -1.0f}; }
    glm::vec3 Camera::right() const noexcept { return orientation_ * glm::vec3{1.0f, 0.0f, 0.0f}; }
    glm::vec3 Camera::up() const noexcept { return orientation_ * glm::vec3{0.0f, 1.0f, 0.0f}; }

    void Camera::look_at(glm::vec3 target, glm::vec3 world_up) noexcept {
        const glm::vec3 direction = safe_normalize(target - position_, forward());
        set_forward(direction, world_up);
    }

    void Camera::set_forward(glm::vec3 direction, glm::vec3 world_up) noexcept {
        direction = safe_normalize(direction, forward());
        glm::vec3 up_axis = safe_normalize(world_up, {0.0f, 1.0f, 0.0f});
        if (std::abs(glm::dot(direction, up_axis)) > 0.999f) {
            up_axis = std::abs(direction.y) < 0.999f
                          ? glm::vec3{0.0f, 1.0f, 0.0f}
                          : glm::vec3{0.0f, 0.0f, 1.0f};
        }
        orientation_ = safe_normalize(glm::quatLookAtRH(direction, up_axis));
    }

    f32 Camera::distance_to(glm::vec3 point) const noexcept { return glm::distance(position_, point); }

    void Camera::translate_world(glm::vec3 delta) noexcept { position_ += delta; }
    void Camera::translate_local(glm::vec3 delta) noexcept { position_ += orientation_ * delta; }
    void Camera::move_forward(f32 distance) noexcept { position_ += forward() * distance; }
    void Camera::move_right(f32 distance) noexcept { position_ += right() * distance; }
    void Camera::move_up(f32 distance) noexcept { position_ += up() * distance; }

    void Camera::rotate_local(glm::quat rotation) noexcept {
        orientation_ = safe_normalize(orientation_ * safe_normalize(rotation));
    }

    void Camera::rotate_world(glm::quat rotation) noexcept {
        orientation_ = safe_normalize(safe_normalize(rotation) * orientation_);
    }

    void Camera::yaw_pitch_roll(f32 yaw_degrees, f32 pitch_degrees, f32 roll_degrees) noexcept {
        rotate_world(glm::angleAxis(glm::radians(yaw_degrees), glm::vec3{0.0f, 1.0f, 0.0f}));
        rotate_local(glm::angleAxis(glm::radians(pitch_degrees), glm::vec3{1.0f, 0.0f, 0.0f}));
        rotate_local(glm::angleAxis(glm::radians(roll_degrees), glm::vec3{0.0f, 0.0f, -1.0f}));
    }

    void Camera::orbit(glm::vec3 pivot, f32 yaw_degrees, f32 pitch_degrees) noexcept {
        glm::vec3 offset = position_ - pivot;
        if (glm::dot(offset, offset) <= std::numeric_limits<f32>::epsilon()) {
            offset = -forward();
        }
        const glm::quat yaw = glm::angleAxis(glm::radians(yaw_degrees), glm::vec3{0.0f, 1.0f, 0.0f});
        const glm::vec3 yawed = yaw * offset;
        const glm::vec3 pitch_axis = safe_normalize(glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, yawed), right());
        const glm::quat pitch = glm::angleAxis(glm::radians(pitch_degrees), pitch_axis);
        position_ = pivot + pitch * yawed;
        look_at(pivot);
    }

    void Camera::dolly(f32 distance) noexcept { move_forward(distance); }
    void Camera::pan(glm::vec2 local_distance) noexcept {
        position_ += right() * local_distance.x + up() * local_distance.y;
    }

    void Camera::zoom(f32 magnification) noexcept {
        const f32 factor = std::max(magnification, 0.0001f);
        if (projection_mode_ == CameraProjectionMode::Orthographic) {
            set_orthographic_vertical_size(orthographic_vertical_size_ / factor);
        } else if (projection_mode_ == CameraProjectionMode::Perspective) {
            set_vertical_fov_degrees(vertical_fov_degrees() / factor);
        }
    }

    void Camera::frame_sphere(glm::vec3 center, f32 radius, f32 padding) noexcept {
        const f32 framed_radius = std::max(radius, 0.0001f) * std::max(padding, 1.0f);
        if (projection_mode_ == CameraProjectionMode::Orthographic) {
            set_orthographic_vertical_size(framed_radius * 2.0f);
        }
        const f32 distance = projection_mode_ == CameraProjectionMode::Perspective
                                 ? framed_radius / std::sin(vertical_fov_radians_ * 0.5f)
                                 : std::max(distance_to(center), framed_radius + near_clip_);
        position_ = center - forward() * distance;
        focus_distance_ = distance;
        look_at(center);
    }

    void Camera::frame_bounds(const CameraAabb &bounds, f32 padding) noexcept {
        const glm::vec3 center = (bounds.minimum + bounds.maximum) * 0.5f;
        const f32 radius = glm::length((bounds.maximum - bounds.minimum) * 0.5f);
        frame_sphere(center, radius, padding);
    }

    CameraProjectionMode Camera::projection_mode() const noexcept { return projection_mode_; }

    void Camera::set_perspective(f32 vertical_fov_degrees, f32 near_clip, f32 far_clip) noexcept {
        projection_mode_ = CameraProjectionMode::Perspective;
        set_clip_planes(near_clip, far_clip);
        set_vertical_fov_degrees(vertical_fov_degrees);
    }

    void Camera::set_orthographic(f32 vertical_size, f32 near_clip, f32 far_clip) noexcept {
        projection_mode_ = CameraProjectionMode::Orthographic;
        set_clip_planes(near_clip, far_clip);
        set_orthographic_vertical_size(vertical_size);
    }

    void Camera::set_custom_projection(glm::mat4 projection) noexcept {
        custom_projection_ = projection;
        projection_mode_ = CameraProjectionMode::Custom;
    }

    void Camera::clear_custom_projection() noexcept { projection_mode_ = CameraProjectionMode::Perspective; }
    f32 Camera::aspect_ratio() const noexcept { return aspect_ratio_; }
    void Camera::set_aspect_ratio(f32 aspect_ratio) noexcept { aspect_ratio_ = std::max(aspect_ratio, 0.0001f); }

    void Camera::set_viewport_size(u32 width, u32 height) noexcept {
        if (width != 0 && height != 0) {
            set_aspect_ratio(static_cast<f32>(width) / static_cast<f32>(height));
        }
    }

    f32 Camera::vertical_fov_degrees() const noexcept { return glm::degrees(vertical_fov_radians_); }

    void Camera::set_vertical_fov_degrees(f32 degrees) noexcept {
        vertical_fov_radians_ = glm::radians(std::clamp(degrees, minimum_fov_degrees, maximum_fov_degrees));
        synchronize_focal_length_from_fov();
    }

    f32 Camera::horizontal_fov_degrees() const noexcept {
        return glm::degrees(2.0f * std::atan(std::tan(vertical_fov_radians_ * 0.5f) * aspect_ratio_));
    }

    void Camera::set_horizontal_fov_degrees(f32 degrees) noexcept {
        const f32 horizontal = glm::radians(std::clamp(degrees, minimum_fov_degrees, maximum_fov_degrees));
        vertical_fov_radians_ = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect_ratio_);
        synchronize_focal_length_from_fov();
    }

    f32 Camera::orthographic_vertical_size() const noexcept { return orthographic_vertical_size_; }
    void Camera::set_orthographic_vertical_size(f32 size) noexcept {
        orthographic_vertical_size_ = std::max(size, 0.0001f);
    }
    f32 Camera::near_clip() const noexcept { return near_clip_; }
    f32 Camera::far_clip() const noexcept { return far_clip_; }

    void Camera::set_clip_planes(f32 near_clip, f32 far_clip) noexcept {
        near_clip_ = std::max(near_clip, minimum_near);
        far_clip_ = std::max(far_clip, near_clip_ + minimum_near);
    }

    glm::vec2 Camera::lens_shift() const noexcept { return lens_shift_; }
    void Camera::set_lens_shift(glm::vec2 shift) noexcept { lens_shift_ = shift; }
    glm::vec2 Camera::jitter_ndc() const noexcept { return jitter_ndc_; }
    void Camera::set_jitter_ndc(glm::vec2 jitter) noexcept { jitter_ndc_ = jitter; }

    void Camera::set_jitter_pixels(glm::vec2 pixel_offset, glm::vec2 viewport_size) noexcept {
        if (viewport_size.x > 0.0f && viewport_size.y > 0.0f) {
            jitter_ndc_ = 2.0f * pixel_offset / viewport_size;
        }
    }

    void Camera::clear_jitter() noexcept { jitter_ndc_ = {0.0f, 0.0f}; }
    bool Camera::reverse_z() const noexcept { return reverse_z_; }
    void Camera::set_reverse_z(bool enabled) noexcept { reverse_z_ = enabled; }
    bool Camera::flip_projection_y() const noexcept { return flip_projection_y_; }
    void Camera::set_flip_projection_y(bool enabled) noexcept { flip_projection_y_ = enabled; }

    f32 Camera::focal_length_mm() const noexcept { return focal_length_mm_; }
    void Camera::set_focal_length_mm(f32 millimeters) noexcept {
        focal_length_mm_ = std::max(millimeters, 0.1f);
        synchronize_fov_from_focal_length();
    }
    glm::vec2 Camera::sensor_size_mm() const noexcept { return sensor_size_mm_; }
    void Camera::set_sensor_size_mm(glm::vec2 millimeters) noexcept {
        sensor_size_mm_ = glm::max(millimeters, glm::vec2{0.1f});
        synchronize_fov_from_focal_length();
    }
    f32 Camera::aperture_f_stop() const noexcept { return aperture_f_stop_; }
    void Camera::set_aperture_f_stop(f32 value) noexcept { aperture_f_stop_ = std::max(value, 0.1f); }
    f32 Camera::shutter_seconds() const noexcept { return shutter_seconds_; }
    void Camera::set_shutter_seconds(f32 seconds) noexcept { shutter_seconds_ = std::max(seconds, 0.000001f); }
    f32 Camera::iso() const noexcept { return iso_; }
    void Camera::set_iso(f32 value) noexcept { iso_ = std::max(value, 1.0f); }
    f32 Camera::focus_distance() const noexcept { return focus_distance_; }
    void Camera::set_focus_distance(f32 distance) noexcept { focus_distance_ = std::max(distance, 0.0f); }
    u32 Camera::aperture_blades() const noexcept { return aperture_blades_; }
    void Camera::set_aperture_blades(u32 blades) noexcept { aperture_blades_ = std::clamp(blades, 3u, 32u); }
    f32 Camera::exposure_compensation_ev() const noexcept { return exposure_compensation_ev_; }
    void Camera::set_exposure_compensation_ev(f32 ev) noexcept { exposure_compensation_ev_ = ev; }
    f32 Camera::exposure_multiplier() const noexcept { return std::exp2(exposure_compensation_ev_); }
    f32 Camera::ev100() const noexcept {
        return std::log2((aperture_f_stop_ * aperture_f_stop_) / shutter_seconds_ * (100.0f / iso_));
    }

    u32 Camera::culling_mask() const noexcept { return culling_mask_; }
    void Camera::set_culling_mask(u32 mask) noexcept { culling_mask_ = mask; }
    i32 Camera::priority() const noexcept { return priority_; }
    void Camera::set_priority(i32 priority) noexcept { priority_ = priority; }
    bool Camera::active() const noexcept { return active_; }
    void Camera::set_active(bool active) noexcept { active_ = active; }
    glm::vec4 Camera::clear_color() const noexcept { return clear_color_; }
    void Camera::set_clear_color(glm::vec4 color) noexcept { clear_color_ = color; }
    f32 Camera::render_scale() const noexcept { return render_scale_; }
    void Camera::set_render_scale(f32 scale) noexcept { render_scale_ = std::clamp(scale, 0.1f, 4.0f); }
    glm::vec4 Camera::normalized_viewport() const noexcept { return normalized_viewport_; }
    void Camera::set_normalized_viewport(glm::vec4 rectangle) noexcept {
        normalized_viewport_ = glm::clamp(rectangle, glm::vec4{0.0f}, glm::vec4{1.0f});
        normalized_viewport_.z = std::min(normalized_viewport_.z, 1.0f - normalized_viewport_.x);
        normalized_viewport_.w = std::min(normalized_viewport_.w, 1.0f - normalized_viewport_.y);
    }

    glm::mat4 Camera::world_matrix() const noexcept {
        return glm::translate(glm::mat4{1.0f}, position_) * glm::mat4_cast(orientation_);
    }

    glm::mat4 Camera::view_matrix() const noexcept {
        return glm::mat4_cast(glm::conjugate(orientation_)) *
               glm::translate(glm::mat4{1.0f}, -position_);
    }

    glm::mat4 Camera::projection_matrix() const noexcept {
        if (projection_mode_ == CameraProjectionMode::Custom) {
            return custom_projection_;
        }

        const f32 projection_near = reverse_z_ ? far_clip_ : near_clip_;
        const f32 projection_far = reverse_z_ ? near_clip_ : far_clip_;
        glm::mat4 projection{1.0f};
        if (projection_mode_ == CameraProjectionMode::Orthographic) {
            const f32 half_height = orthographic_vertical_size_ * 0.5f;
            const f32 half_width = half_height * aspect_ratio_;
            projection = glm::orthoRH_ZO(-half_width, half_width, -half_height, half_height,
                                         projection_near, projection_far);
        } else {
            projection = glm::perspectiveRH_ZO(vertical_fov_radians_, aspect_ratio_,
                                               projection_near, projection_far);
        }
        if (flip_projection_y_) {
            projection[1][1] *= -1.0f;
        }
        projection[2][0] += lens_shift_.x + jitter_ndc_.x;
        projection[2][1] += lens_shift_.y + jitter_ndc_.y;
        return projection;
    }

    glm::mat4 Camera::view_projection_matrix() const noexcept { return projection_matrix() * view_matrix(); }
    glm::mat4 Camera::inverse_view_projection_matrix() const noexcept { return glm::inverse(view_projection_matrix()); }

    std::optional<glm::vec3> Camera::project(glm::vec3 world, glm::vec2 viewport_size) const noexcept {
        if (viewport_size.x <= 0.0f || viewport_size.y <= 0.0f) {
            return std::nullopt;
        }
        const glm::vec4 clip = view_projection_matrix() * glm::vec4{world, 1.0f};
        if (!std::isfinite(clip.w) || std::abs(clip.w) <= std::numeric_limits<f32>::epsilon()) {
            return std::nullopt;
        }
        const glm::vec3 ndc = glm::vec3{clip} / clip.w;
        return glm::vec3{
            (ndc.x * 0.5f + 0.5f) * viewport_size.x,
            (ndc.y * 0.5f + 0.5f) * viewport_size.y,
            ndc.z,
        };
    }

    std::optional<glm::vec3> Camera::unproject(glm::vec3 screen, glm::vec2 viewport_size) const noexcept {
        if (viewport_size.x <= 0.0f || viewport_size.y <= 0.0f) {
            return std::nullopt;
        }
        const glm::vec4 clip{
            screen.x / viewport_size.x * 2.0f - 1.0f,
            screen.y / viewport_size.y * 2.0f - 1.0f,
            screen.z,
            1.0f,
        };
        const glm::vec4 world = inverse_view_projection_matrix() * clip;
        if (!std::isfinite(world.w) || std::abs(world.w) <= std::numeric_limits<f32>::epsilon()) {
            return std::nullopt;
        }
        return glm::vec3{world} / world.w;
    }

    std::optional<CameraRay> Camera::screen_ray(glm::vec2 pixel, glm::vec2 viewport_size) const noexcept {
        const f32 near_depth = reverse_z_ ? 1.0f : 0.0f;
        const f32 far_depth = reverse_z_ ? 0.0f : 1.0f;
        const auto near_point = unproject(glm::vec3{pixel, near_depth}, viewport_size);
        const auto far_point = unproject(glm::vec3{pixel, far_depth}, viewport_size);
        if (!near_point || !far_point) {
            return std::nullopt;
        }
        const glm::vec3 direction = safe_normalize(*far_point - *near_point, forward());
        return CameraRay{
            .origin = projection_mode_ == CameraProjectionMode::Orthographic ? *near_point : position_,
            .direction = direction,
        };
    }

    CameraFrustum Camera::frustum() const noexcept {
        const glm::mat4 matrix = view_projection_matrix();
        const glm::vec4 row0 = matrix_row(matrix, 0);
        const glm::vec4 row1 = matrix_row(matrix, 1);
        const glm::vec4 row2 = matrix_row(matrix, 2);
        const glm::vec4 row3 = matrix_row(matrix, 3);
        CameraFrustum result{};
        result.planes[CameraFrustum::Left] = normalized_plane(row3 + row0);
        result.planes[CameraFrustum::Right] = normalized_plane(row3 - row0);
        result.planes[CameraFrustum::Bottom] = normalized_plane(row3 + row1);
        result.planes[CameraFrustum::Top] = normalized_plane(row3 - row1);
        result.planes[CameraFrustum::Near] = normalized_plane(reverse_z_ ? row3 - row2 : row2);
        result.planes[CameraFrustum::Far] = normalized_plane(reverse_z_ ? row2 : row3 - row2);
        return result;
    }

    bool Camera::sees(glm::vec3 point) const noexcept { return frustum().contains(point); }
    bool Camera::sees_sphere(glm::vec3 center, f32 radius) const noexcept {
        return frustum().intersects_sphere(center, radius);
    }
    CameraContainment Camera::sees(const CameraAabb &bounds) const noexcept { return frustum().classify(bounds); }

    void Camera::commit_frame() noexcept {
        previous_view_projection_ = view_projection_matrix();
        has_history_ = true;
    }
    void Camera::reset_history() noexcept {
        previous_view_projection_ = view_projection_matrix();
        has_history_ = false;
    }
    bool Camera::has_history() const noexcept { return has_history_; }
    glm::mat4 Camera::previous_view_projection_matrix() const noexcept {
        return has_history_ ? previous_view_projection_ : view_projection_matrix();
    }

    SFT::Renderer::CameraView Camera::renderer_view() const noexcept {
        return SFT::Renderer::CameraView{
            .view = view_matrix(),
            .projection = projection_matrix(),
            .world_position = position_,
            .near_plane = near_clip_,
            .far_plane = far_clip_,
            .vertical_fov_radians = vertical_fov_radians_,
        };
    }

    void Camera::synchronize_focal_length_from_fov() noexcept {
        focal_length_mm_ = sensor_size_mm_.y / (2.0f * std::tan(vertical_fov_radians_ * 0.5f));
    }

    void Camera::synchronize_fov_from_focal_length() noexcept {
        vertical_fov_radians_ = 2.0f * std::atan(sensor_size_mm_.y / (2.0f * focal_length_mm_));
        vertical_fov_radians_ = std::clamp(
            vertical_fov_radians_,
            glm::radians(minimum_fov_degrees),
            glm::radians(maximum_fov_degrees));
    }

} // namespace SFT::Engine
