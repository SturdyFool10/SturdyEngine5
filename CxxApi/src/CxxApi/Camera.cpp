#include <CxxApi/Camera.hpp>

#include <cstring>

#include <glm/gtc/type_ptr.hpp>

#include <CxxApi/Conversions.hpp>

namespace SFT::CxxApi {

    namespace E = SFT::Engine;

    void camera_apply_desc(E::Camera &camera, const CameraDesc &d) noexcept {
        switch (d.projection) {
            case CameraProjection::Orthographic:
                camera.set_orthographic(d.orthographic_size, d.near_clip, d.far_clip);
                break;
            case CameraProjection::Custom:
                camera.set_custom_projection(to_mat4(d.custom_projection));
                camera.set_clip_planes(d.near_clip, d.far_clip);
                break;
            case CameraProjection::Perspective:
            default:
                camera.set_perspective(d.vertical_fov_degrees, d.near_clip, d.far_clip);
                break;
        }
        camera.set_aspect_ratio(d.aspect_ratio);
        camera.set_sensor_size_mm(to_vec2(d.sensor_size_mm));
        if (d.has_focal_length && d.projection == CameraProjection::Perspective) {
            camera.set_focal_length_mm(d.focal_length_mm);
        }
        camera.set_position(to_vec3(d.position));
        if (d.has_orientation) {
            camera.set_orientation(to_quat(d.orientation));
        } else {
            camera.set_euler_degrees(to_vec3(d.euler_degrees));
        }
        camera.set_lens_shift(to_vec2(d.lens_shift));
        camera.set_jitter_ndc(to_vec2(d.jitter_ndc));
        camera.set_reverse_z(d.reverse_z);
        camera.set_aperture_f_stop(d.aperture_f_stop);
        camera.set_shutter_seconds(d.shutter_seconds);
        camera.set_iso(d.iso);
        camera.set_focus_distance(d.focus_distance);
        camera.set_aperture_blades(d.aperture_blades);
        camera.set_exposure_compensation_ev(d.exposure_compensation_ev);
        camera.set_culling_mask(d.culling_mask);
        camera.set_priority(d.priority);
        camera.set_active(d.active);
        camera.set_clear_color(to_vec4(d.clear_color));
        camera.set_render_scale(d.render_scale);
        camera.set_normalized_viewport(to_vec4(d.normalized_viewport));
        if (d.reset_history) {
            camera.reset_history();
        }
    }

    CameraDesc camera_to_desc(const E::Camera &c) noexcept {
        CameraDesc d{};
        switch (c.projection_mode()) {
            case E::CameraProjectionMode::Orthographic: d.projection = CameraProjection::Orthographic; break;
            case E::CameraProjectionMode::Custom: d.projection = CameraProjection::Custom; break;
            default: d.projection = CameraProjection::Perspective; break;
        }
        store(d.position, c.position());
        store(d.euler_degrees, c.euler_degrees());
        d.has_orientation = true;
        store(d.orientation, c.orientation());
        d.vertical_fov_degrees = c.vertical_fov_degrees();
        d.orthographic_size = c.orthographic_vertical_size();
        store(d.custom_projection, c.projection_matrix());
        d.aspect_ratio = c.aspect_ratio();
        d.near_clip = c.near_clip();
        d.far_clip = c.far_clip();
        store(d.lens_shift, c.lens_shift());
        store(d.jitter_ndc, c.jitter_ndc());
        d.reverse_z = c.reverse_z();
        d.has_focal_length = true;
        d.focal_length_mm = c.focal_length_mm();
        store(d.sensor_size_mm, c.sensor_size_mm());
        d.aperture_f_stop = c.aperture_f_stop();
        d.shutter_seconds = c.shutter_seconds();
        d.iso = c.iso();
        d.focus_distance = c.focus_distance();
        d.aperture_blades = c.aperture_blades();
        d.exposure_compensation_ev = c.exposure_compensation_ev();
        d.culling_mask = c.culling_mask();
        d.priority = c.priority();
        d.active = c.active();
        store(d.clear_color, c.clear_color());
        d.render_scale = c.render_scale();
        store(d.normalized_viewport, c.normalized_viewport());
        d.reset_history = false;
        return d;
    }

    CameraDesc camera_desc_defaults() noexcept {
        CameraDesc d = camera_to_desc(E::Camera{});
        // A fresh camera is described by its euler angles, not a forced orientation.
        d.has_orientation = false;
        d.has_focal_length = false;
        return d;
    }

    std::unique_ptr<E::Camera> camera_new(const CameraDesc &desc) {
        auto camera = std::make_unique<E::Camera>();
        camera_apply_desc(*camera, desc);
        return camera;
    }

    void camera_look_at(E::Camera &c, std::array<float, 3> t, std::array<float, 3> up) noexcept { c.look_at(to_vec3(t), to_vec3(up)); }
    void camera_set_forward(E::Camera &c, std::array<float, 3> d, std::array<float, 3> up) noexcept { c.set_forward(to_vec3(d), to_vec3(up)); }
    void camera_translate_world(E::Camera &c, std::array<float, 3> d) noexcept { c.translate_world(to_vec3(d)); }
    void camera_translate_local(E::Camera &c, std::array<float, 3> d) noexcept { c.translate_local(to_vec3(d)); }
    void camera_rotate_local(E::Camera &c, std::array<float, 4> q) noexcept { c.rotate_local(to_quat(q)); }
    void camera_rotate_world(E::Camera &c, std::array<float, 4> q) noexcept { c.rotate_world(to_quat(q)); }
    void camera_yaw_pitch_roll(E::Camera &c, float y, float p, float r) noexcept { c.yaw_pitch_roll(y, p, r); }
    void camera_orbit(E::Camera &c, std::array<float, 3> pivot, float y, float p) noexcept { c.orbit(to_vec3(pivot), y, p); }
    void camera_dolly(E::Camera &c, float d) noexcept { c.dolly(d); }
    void camera_pan(E::Camera &c, std::array<float, 2> d) noexcept { c.pan(to_vec2(d)); }
    void camera_zoom(E::Camera &c, float m) noexcept { c.zoom(m); }
    void camera_frame_sphere(E::Camera &c, std::array<float, 3> center, float radius, float padding) noexcept {
        c.frame_sphere(to_vec3(center), radius, padding);
    }
    void camera_frame_bounds(E::Camera &c, std::array<float, 3> mn, std::array<float, 3> mx, float padding) noexcept {
        c.frame_bounds(E::CameraAabb{to_vec3(mn), to_vec3(mx)}, padding);
    }
    void camera_set_viewport_size(E::Camera &c, std::uint32_t w, std::uint32_t h) noexcept { c.set_viewport_size(w, h); }
    void camera_set_jitter_pixels(E::Camera &c, std::array<float, 2> px, std::array<float, 2> vp) noexcept {
        c.set_jitter_pixels(to_vec2(px), to_vec2(vp));
    }
    void camera_clear_jitter(E::Camera &c) noexcept { c.clear_jitter(); }

    float camera_distance_to(const E::Camera &c, std::array<float, 3> p) noexcept { return c.distance_to(to_vec3(p)); }
    std::array<float, 3> camera_forward(const E::Camera &c) noexcept { return to_array(c.forward()); }
    std::array<float, 3> camera_right(const E::Camera &c) noexcept { return to_array(c.right()); }
    std::array<float, 3> camera_up(const E::Camera &c) noexcept { return to_array(c.up()); }
    float camera_horizontal_fov_degrees(const E::Camera &c) noexcept { return c.horizontal_fov_degrees(); }
    float camera_exposure_multiplier(const E::Camera &c) noexcept { return c.exposure_multiplier(); }
    float camera_ev100(const E::Camera &c) noexcept { return c.ev100(); }

    std::array<float, 16> camera_world_matrix(const E::Camera &c) noexcept { return to_array(c.world_matrix()); }
    std::array<float, 16> camera_view_matrix(const E::Camera &c) noexcept { return to_array(c.view_matrix()); }
    std::array<float, 16> camera_projection_matrix(const E::Camera &c) noexcept { return to_array(c.projection_matrix()); }
    std::array<float, 16> camera_view_projection_matrix(const E::Camera &c) noexcept { return to_array(c.view_projection_matrix()); }
    std::array<float, 16> camera_inverse_view_projection_matrix(const E::Camera &c) noexcept {
        return to_array(c.inverse_view_projection_matrix());
    }
    std::array<float, 16> camera_previous_view_projection_matrix(const E::Camera &c) noexcept {
        return to_array(c.previous_view_projection_matrix());
    }

    Vec3Result camera_project(const E::Camera &c, std::array<float, 3> w, std::array<float, 2> vp) noexcept {
        Vec3Result out{};
        if (const auto p = c.project(to_vec3(w), to_vec2(vp))) {
            out.ok = true;
            store(out.value, *p);
        }
        return out;
    }

    Vec3Result camera_unproject(const E::Camera &c, std::array<float, 3> s, std::array<float, 2> vp) noexcept {
        Vec3Result out{};
        if (const auto p = c.unproject(to_vec3(s), to_vec2(vp))) {
            out.ok = true;
            store(out.value, *p);
        }
        return out;
    }

    RayResult camera_screen_ray(const E::Camera &c, std::array<float, 2> px, std::array<float, 2> vp) noexcept {
        RayResult out{};
        if (const auto ray = c.screen_ray(to_vec2(px), to_vec2(vp))) {
            out.ok = true;
            store(out.ray.origin, ray->origin);
            store(out.ray.direction, ray->direction);
        }
        return out;
    }

    bool camera_sees_point(const E::Camera &c, std::array<float, 3> p) noexcept { return c.sees(to_vec3(p)); }
    bool camera_sees_sphere(const E::Camera &c, std::array<float, 3> center, float radius) noexcept {
        return c.sees_sphere(to_vec3(center), radius);
    }
    Containment camera_sees_bounds(const E::Camera &c, std::array<float, 3> mn, std::array<float, 3> mx) noexcept {
        switch (c.sees(E::CameraAabb{to_vec3(mn), to_vec3(mx)})) {
            case E::CameraContainment::Inside: return Containment::Inside;
            case E::CameraContainment::Intersecting: return Containment::Intersecting;
            default: return Containment::Outside;
        }
    }

    void camera_commit_frame(E::Camera &c) noexcept { c.commit_frame(); }
    void camera_reset_history(E::Camera &c) noexcept { c.reset_history(); }
    bool camera_has_history(const E::Camera &c) noexcept { return c.has_history(); }

} // namespace SFT::CxxApi
