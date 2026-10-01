#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <Engine/Camera.hpp>

#include <CxxApi/Types/Camera.hpp>

namespace SFT::CxxApi {

    /// Applies every field of `desc` to `camera`.
    void camera_apply_desc(Engine::Camera &camera, const CameraDesc &desc) noexcept;
    /// Reads every field of `camera` back into a description (orientation as a quaternion).
    [[nodiscard]] CameraDesc camera_to_desc(const Engine::Camera &camera) noexcept;
    /// A default-constructed `Engine::Camera` as a description.
    [[nodiscard]] CameraDesc camera_desc_defaults() noexcept;

    [[nodiscard]] std::unique_ptr<Engine::Camera> camera_new(const CameraDesc &desc);

    void camera_look_at(Engine::Camera &camera, std::array<float, 3> target, std::array<float, 3> world_up) noexcept;
    void camera_set_forward(Engine::Camera &camera, std::array<float, 3> direction, std::array<float, 3> world_up) noexcept;
    void camera_translate_world(Engine::Camera &camera, std::array<float, 3> delta) noexcept;
    void camera_translate_local(Engine::Camera &camera, std::array<float, 3> delta) noexcept;
    /// `rotation` is an xyzw quaternion.
    void camera_rotate_local(Engine::Camera &camera, std::array<float, 4> rotation) noexcept;
    void camera_rotate_world(Engine::Camera &camera, std::array<float, 4> rotation) noexcept;
    void camera_yaw_pitch_roll(Engine::Camera &camera, float yaw, float pitch, float roll) noexcept;
    void camera_orbit(Engine::Camera &camera, std::array<float, 3> pivot, float yaw, float pitch) noexcept;
    void camera_dolly(Engine::Camera &camera, float distance) noexcept;
    void camera_pan(Engine::Camera &camera, std::array<float, 2> local_distance) noexcept;
    void camera_zoom(Engine::Camera &camera, float magnification) noexcept;
    void camera_frame_sphere(Engine::Camera &camera, std::array<float, 3> center, float radius, float padding) noexcept;
    void camera_frame_bounds(Engine::Camera &camera, std::array<float, 3> min, std::array<float, 3> max, float padding) noexcept;
    void camera_set_viewport_size(Engine::Camera &camera, std::uint32_t width, std::uint32_t height) noexcept;
    void camera_set_jitter_pixels(Engine::Camera &camera, std::array<float, 2> pixel_offset, std::array<float, 2> viewport_size) noexcept;
    void camera_clear_jitter(Engine::Camera &camera) noexcept;

    [[nodiscard]] float camera_distance_to(const Engine::Camera &camera, std::array<float, 3> point) noexcept;
    [[nodiscard]] std::array<float, 3> camera_forward(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 3> camera_right(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 3> camera_up(const Engine::Camera &camera) noexcept;
    [[nodiscard]] float camera_horizontal_fov_degrees(const Engine::Camera &camera) noexcept;
    [[nodiscard]] float camera_exposure_multiplier(const Engine::Camera &camera) noexcept;
    [[nodiscard]] float camera_ev100(const Engine::Camera &camera) noexcept;

    /// Column-major 4x4 matrices.
    [[nodiscard]] std::array<float, 16> camera_world_matrix(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 16> camera_view_matrix(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 16> camera_projection_matrix(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 16> camera_view_projection_matrix(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 16> camera_inverse_view_projection_matrix(const Engine::Camera &camera) noexcept;
    [[nodiscard]] std::array<float, 16> camera_previous_view_projection_matrix(const Engine::Camera &camera) noexcept;

    /// Screen space is top-left origin with depth in [0, 1].
    [[nodiscard]] Vec3Result camera_project(const Engine::Camera &camera, std::array<float, 3> world, std::array<float, 2> viewport_size) noexcept;
    [[nodiscard]] Vec3Result camera_unproject(const Engine::Camera &camera, std::array<float, 3> screen, std::array<float, 2> viewport_size) noexcept;
    [[nodiscard]] RayResult camera_screen_ray(const Engine::Camera &camera, std::array<float, 2> pixel, std::array<float, 2> viewport_size) noexcept;

    [[nodiscard]] bool camera_sees_point(const Engine::Camera &camera, std::array<float, 3> point) noexcept;
    [[nodiscard]] bool camera_sees_sphere(const Engine::Camera &camera, std::array<float, 3> center, float radius) noexcept;
    [[nodiscard]] Containment camera_sees_bounds(const Engine::Camera &camera, std::array<float, 3> min, std::array<float, 3> max) noexcept;

    void camera_commit_frame(Engine::Camera &camera) noexcept;
    void camera_reset_history(Engine::Camera &camera) noexcept;
    [[nodiscard]] bool camera_has_history(const Engine::Camera &camera) noexcept;

} // namespace SFT::CxxApi
