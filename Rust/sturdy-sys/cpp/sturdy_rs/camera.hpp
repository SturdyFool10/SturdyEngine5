// Rust <-> SturdyEngine 5 shim for SFT::Engine::Camera. See src/camera.rs.
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <rust/cxx.h>

#include <Engine/Camera.hpp>

namespace sturdy_rs {
    struct CameraDesc;
}

namespace sturdy_rs::camera {

    enum class Containment : std::uint8_t;
    struct Vec3Result;
    struct RayResult;

    using CameraRig = SFT::Engine::Camera;

    /// Applies every field of `desc` to `camera` (shared by the per-frame path in shim.cpp).
    void camera_apply_desc(CameraRig &camera, const CameraDesc &desc);
    CameraDesc camera_to_desc(const CameraRig &camera);

    std::unique_ptr<CameraRig> camera_rig_new(const CameraDesc &desc);
    CameraDesc camera_rig_desc(const CameraRig &rig);
    void camera_rig_apply(CameraRig &rig, const CameraDesc &desc);

    void camera_look_at(CameraRig &rig, std::array<float, 3> target, std::array<float, 3> world_up) noexcept;
    void camera_set_forward(CameraRig &rig, std::array<float, 3> direction, std::array<float, 3> world_up) noexcept;
    void camera_translate_world(CameraRig &rig, std::array<float, 3> delta) noexcept;
    void camera_translate_local(CameraRig &rig, std::array<float, 3> delta) noexcept;
    void camera_rotate_local(CameraRig &rig, std::array<float, 4> rotation) noexcept;
    void camera_rotate_world(CameraRig &rig, std::array<float, 4> rotation) noexcept;
    void camera_yaw_pitch_roll(CameraRig &rig, float yaw, float pitch, float roll) noexcept;
    void camera_orbit(CameraRig &rig, std::array<float, 3> pivot, float yaw, float pitch) noexcept;
    void camera_dolly(CameraRig &rig, float distance) noexcept;
    void camera_pan(CameraRig &rig, std::array<float, 2> local_distance) noexcept;
    void camera_zoom(CameraRig &rig, float magnification) noexcept;
    void camera_frame_sphere(CameraRig &rig, std::array<float, 3> center, float radius, float padding) noexcept;
    void camera_frame_bounds(CameraRig &rig, std::array<float, 3> min, std::array<float, 3> max, float padding) noexcept;
    void camera_set_viewport_size(CameraRig &rig, std::uint32_t width, std::uint32_t height) noexcept;
    void camera_set_jitter_pixels(CameraRig &rig, std::array<float, 2> pixel_offset, std::array<float, 2> viewport_size) noexcept;
    void camera_clear_jitter(CameraRig &rig) noexcept;

    float camera_distance_to(const CameraRig &rig, std::array<float, 3> point) noexcept;
    std::array<float, 3> camera_forward(const CameraRig &rig) noexcept;
    std::array<float, 3> camera_right(const CameraRig &rig) noexcept;
    std::array<float, 3> camera_up(const CameraRig &rig) noexcept;
    float camera_horizontal_fov_degrees(const CameraRig &rig) noexcept;
    float camera_exposure_multiplier(const CameraRig &rig) noexcept;
    float camera_ev100(const CameraRig &rig) noexcept;

    std::array<float, 16> camera_world_matrix(const CameraRig &rig) noexcept;
    std::array<float, 16> camera_view_matrix(const CameraRig &rig) noexcept;
    std::array<float, 16> camera_projection_matrix(const CameraRig &rig) noexcept;
    std::array<float, 16> camera_view_projection_matrix(const CameraRig &rig) noexcept;
    std::array<float, 16> camera_inverse_view_projection_matrix(const CameraRig &rig) noexcept;
    std::array<float, 16> camera_previous_view_projection_matrix(const CameraRig &rig) noexcept;

    Vec3Result camera_project(const CameraRig &rig, std::array<float, 3> world, std::array<float, 2> viewport_size) noexcept;
    Vec3Result camera_unproject(const CameraRig &rig, std::array<float, 3> screen, std::array<float, 2> viewport_size) noexcept;
    RayResult camera_screen_ray(const CameraRig &rig, std::array<float, 2> pixel, std::array<float, 2> viewport_size) noexcept;

    bool camera_sees_point(const CameraRig &rig, std::array<float, 3> point) noexcept;
    bool camera_sees_sphere(const CameraRig &rig, std::array<float, 3> center, float radius) noexcept;
    Containment camera_sees_bounds(const CameraRig &rig, std::array<float, 3> min, std::array<float, 3> max) noexcept;

    void camera_commit_frame(CameraRig &rig) noexcept;
    void camera_reset_history(CameraRig &rig) noexcept;
    bool camera_has_history(const CameraRig &rig) noexcept;

} // namespace sturdy_rs::camera
