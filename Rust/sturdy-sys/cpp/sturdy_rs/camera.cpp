#include "sturdy_rs/camera.hpp"

#include <cstring>

#include <glm/gtc/type_ptr.hpp>

// Generated from src/lib.rs (CameraDesc) and src/camera.rs.
#include "sturdy_rs/bridge.h"
#include "sturdy_rs/camera_bridge.h"

namespace sturdy_rs::camera {
    namespace {

        namespace E = SFT::Engine;

        glm::vec3 v3(std::array<float, 3> a) noexcept { return {a[0], a[1], a[2]}; }
        glm::vec2 v2(std::array<float, 2> a) noexcept { return {a[0], a[1]}; }
        std::array<float, 3> a3(glm::vec3 v) noexcept { return {v.x, v.y, v.z}; }
        glm::quat q4(std::array<float, 4> a) noexcept { return glm::quat{a[3], a[0], a[1], a[2]}; }  // xyzw -> wxyz

        std::array<float, 16> mat(const glm::mat4 &m) noexcept {
            std::array<float, 16> out{};
            std::memcpy(out.data(), glm::value_ptr(m), sizeof(out));
            return out;
        }

    } // namespace

    void camera_apply_desc(CameraRig &camera, const CameraDesc &d) {
        switch (d.projection) {
        case CameraProjection::Orthographic:
            camera.set_orthographic(d.orthographic_size, d.near_clip, d.far_clip);
            break;
        case CameraProjection::Custom: {
            glm::mat4 projection;
            std::memcpy(glm::value_ptr(projection), d.custom_projection.data(), sizeof(d.custom_projection));
            camera.set_custom_projection(projection);
            camera.set_clip_planes(d.near_clip, d.far_clip);
            break;
        }
        case CameraProjection::Perspective:
            camera.set_perspective(d.vertical_fov_degrees, d.near_clip, d.far_clip);
            break;
        }
        camera.set_aspect_ratio(d.aspect_ratio);
        camera.set_sensor_size_mm(v2(d.sensor_size_mm));
        if (d.has_focal_length && d.projection == CameraProjection::Perspective) {
            camera.set_focal_length_mm(d.focal_length_mm);
        }
        camera.set_position(v3(d.position));
        if (d.has_orientation) {
            camera.set_orientation(q4(d.orientation));
        } else {
            camera.set_euler_degrees(v3(d.euler_degrees));
        }
        camera.set_lens_shift(v2(d.lens_shift));
        camera.set_jitter_ndc(v2(d.jitter_ndc));
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
        camera.set_clear_color({d.clear_color[0], d.clear_color[1], d.clear_color[2], d.clear_color[3]});
        camera.set_render_scale(d.render_scale);
        camera.set_normalized_viewport(
            {d.normalized_viewport[0], d.normalized_viewport[1], d.normalized_viewport[2], d.normalized_viewport[3]});
        if (d.reset_history) {
            camera.reset_history();
        }
    }

    CameraDesc camera_to_desc(const CameraRig &c) {
        CameraDesc d{};
        switch (c.projection_mode()) {
        case E::CameraProjectionMode::Orthographic: d.projection = CameraProjection::Orthographic; break;
        case E::CameraProjectionMode::Custom: d.projection = CameraProjection::Custom; break;
        default: d.projection = CameraProjection::Perspective; break;
        }
        d.position = a3(c.position());
        d.euler_degrees = a3(c.euler_degrees());
        d.has_orientation = true;
        const glm::quat q = c.orientation();
        d.orientation = {q.x, q.y, q.z, q.w};
        d.vertical_fov_degrees = c.vertical_fov_degrees();
        d.orthographic_size = c.orthographic_vertical_size();
        d.custom_projection = mat(c.projection_matrix());
        d.aspect_ratio = c.aspect_ratio();
        d.near_clip = c.near_clip();
        d.far_clip = c.far_clip();
        d.lens_shift = {c.lens_shift().x, c.lens_shift().y};
        d.jitter_ndc = {c.jitter_ndc().x, c.jitter_ndc().y};
        d.reverse_z = c.reverse_z();
        d.has_focal_length = true;
        d.focal_length_mm = c.focal_length_mm();
        d.sensor_size_mm = {c.sensor_size_mm().x, c.sensor_size_mm().y};
        d.aperture_f_stop = c.aperture_f_stop();
        d.shutter_seconds = c.shutter_seconds();
        d.iso = c.iso();
        d.focus_distance = c.focus_distance();
        d.aperture_blades = c.aperture_blades();
        d.exposure_compensation_ev = c.exposure_compensation_ev();
        d.culling_mask = c.culling_mask();
        d.priority = c.priority();
        d.active = c.active();
        const glm::vec4 clear = c.clear_color();
        d.clear_color = {clear.x, clear.y, clear.z, clear.w};
        d.render_scale = c.render_scale();
        const glm::vec4 vp = c.normalized_viewport();
        d.normalized_viewport = {vp.x, vp.y, vp.z, vp.w};
        d.reset_history = false;
        return d;
    }

    std::unique_ptr<CameraRig> camera_rig_new(const CameraDesc &desc) {
        auto rig = std::make_unique<CameraRig>();
        camera_apply_desc(*rig, desc);
        return rig;
    }
    CameraDesc camera_rig_desc(const CameraRig &rig) { return camera_to_desc(rig); }
    void camera_rig_apply(CameraRig &rig, const CameraDesc &desc) { camera_apply_desc(rig, desc); }

    void camera_look_at(CameraRig &r, std::array<float, 3> t, std::array<float, 3> up) noexcept { r.look_at(v3(t), v3(up)); }
    void camera_set_forward(CameraRig &r, std::array<float, 3> d, std::array<float, 3> up) noexcept { r.set_forward(v3(d), v3(up)); }
    void camera_translate_world(CameraRig &r, std::array<float, 3> d) noexcept { r.translate_world(v3(d)); }
    void camera_translate_local(CameraRig &r, std::array<float, 3> d) noexcept { r.translate_local(v3(d)); }
    void camera_rotate_local(CameraRig &r, std::array<float, 4> q) noexcept { r.rotate_local(q4(q)); }
    void camera_rotate_world(CameraRig &r, std::array<float, 4> q) noexcept { r.rotate_world(q4(q)); }
    void camera_yaw_pitch_roll(CameraRig &r, float y, float p, float ro) noexcept { r.yaw_pitch_roll(y, p, ro); }
    void camera_orbit(CameraRig &r, std::array<float, 3> pivot, float y, float p) noexcept { r.orbit(v3(pivot), y, p); }
    void camera_dolly(CameraRig &r, float d) noexcept { r.dolly(d); }
    void camera_pan(CameraRig &r, std::array<float, 2> d) noexcept { r.pan(v2(d)); }
    void camera_zoom(CameraRig &r, float m) noexcept { r.zoom(m); }
    void camera_frame_sphere(CameraRig &r, std::array<float, 3> c, float radius, float padding) noexcept {
        r.frame_sphere(v3(c), radius, padding);
    }
    void camera_frame_bounds(CameraRig &r, std::array<float, 3> mn, std::array<float, 3> mx, float padding) noexcept {
        r.frame_bounds(E::CameraAabb{v3(mn), v3(mx)}, padding);
    }
    void camera_set_viewport_size(CameraRig &r, std::uint32_t w, std::uint32_t h) noexcept { r.set_viewport_size(w, h); }
    void camera_set_jitter_pixels(CameraRig &r, std::array<float, 2> px, std::array<float, 2> vp) noexcept {
        r.set_jitter_pixels(v2(px), v2(vp));
    }
    void camera_clear_jitter(CameraRig &r) noexcept { r.clear_jitter(); }

    float camera_distance_to(const CameraRig &r, std::array<float, 3> p) noexcept { return r.distance_to(v3(p)); }
    std::array<float, 3> camera_forward(const CameraRig &r) noexcept { return a3(r.forward()); }
    std::array<float, 3> camera_right(const CameraRig &r) noexcept { return a3(r.right()); }
    std::array<float, 3> camera_up(const CameraRig &r) noexcept { return a3(r.up()); }
    float camera_horizontal_fov_degrees(const CameraRig &r) noexcept { return r.horizontal_fov_degrees(); }
    float camera_exposure_multiplier(const CameraRig &r) noexcept { return r.exposure_multiplier(); }
    float camera_ev100(const CameraRig &r) noexcept { return r.ev100(); }

    std::array<float, 16> camera_world_matrix(const CameraRig &r) noexcept { return mat(r.world_matrix()); }
    std::array<float, 16> camera_view_matrix(const CameraRig &r) noexcept { return mat(r.view_matrix()); }
    std::array<float, 16> camera_projection_matrix(const CameraRig &r) noexcept { return mat(r.projection_matrix()); }
    std::array<float, 16> camera_view_projection_matrix(const CameraRig &r) noexcept { return mat(r.view_projection_matrix()); }
    std::array<float, 16> camera_inverse_view_projection_matrix(const CameraRig &r) noexcept {
        return mat(r.inverse_view_projection_matrix());
    }
    std::array<float, 16> camera_previous_view_projection_matrix(const CameraRig &r) noexcept {
        return mat(r.previous_view_projection_matrix());
    }

    Vec3Result camera_project(const CameraRig &r, std::array<float, 3> w, std::array<float, 2> vp) noexcept {
        Vec3Result out{};
        if (const auto p = r.project(v3(w), v2(vp))) {
            out.ok = true;
            out.value = a3(*p);
        }
        return out;
    }
    Vec3Result camera_unproject(const CameraRig &r, std::array<float, 3> s, std::array<float, 2> vp) noexcept {
        Vec3Result out{};
        if (const auto p = r.unproject(v3(s), v2(vp))) {
            out.ok = true;
            out.value = a3(*p);
        }
        return out;
    }
    RayResult camera_screen_ray(const CameraRig &r, std::array<float, 2> px, std::array<float, 2> vp) noexcept {
        RayResult out{};
        if (const auto ray = r.screen_ray(v2(px), v2(vp))) {
            out.ok = true;
            out.ray.origin = a3(ray->origin);
            out.ray.direction = a3(ray->direction);
        }
        return out;
    }

    bool camera_sees_point(const CameraRig &r, std::array<float, 3> p) noexcept { return r.sees(v3(p)); }
    bool camera_sees_sphere(const CameraRig &r, std::array<float, 3> c, float radius) noexcept {
        return r.sees_sphere(v3(c), radius);
    }
    Containment camera_sees_bounds(const CameraRig &r, std::array<float, 3> mn, std::array<float, 3> mx) noexcept {
        switch (r.sees(E::CameraAabb{v3(mn), v3(mx)})) {
        case E::CameraContainment::Inside: return Containment::Inside;
        case E::CameraContainment::Intersecting: return Containment::Intersecting;
        default: return Containment::Outside;
        }
    }

    void camera_commit_frame(CameraRig &r) noexcept { r.commit_frame(); }
    void camera_reset_history(CameraRig &r) noexcept { r.reset_history(); }
    bool camera_has_history(const CameraRig &r) noexcept { return r.has_history(); }

} // namespace sturdy_rs::camera
