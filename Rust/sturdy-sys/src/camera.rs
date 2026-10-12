//! Raw `cxx` bindings for a live `SFT::Engine::Camera`: the full camera model (physical lens,
//! exposure, jitter, viewport, history) plus its math — movement/orbit/framing helpers,
//! matrices, projection/unprojection, picking rays and frustum queries.
//!
//! State moves in and out as the root bridge's [`CameraDesc`](crate::ffi::CameraDesc), which
//! carries every field; the object itself only exists on the C++ side so its operations (which
//! keep the focal-length/FOV and orientation/euler representations in sync) are the engine's own.
//! The per-frame path (`RustGameLogic::request_render_frame` in `shim.cpp`) uses the same
//! `camera_apply_desc`/`camera_to_desc` pair, plus a persistent camera per surface so temporal
//! history (previous view-projection, needed by motion blur / temporal effects) survives.

#[cxx::bridge(namespace = "sturdy_rs::camera")]
pub mod ffi {
    /// A camera-space ray.
    #[derive(Debug, Clone, Copy, Default)]
    struct Ray {
        origin: [f32; 3],
        direction: [f32; 3],
    }

    /// Result of `project`/`unproject`/`screen_ray`; `ok == false` when the point is behind the
    /// camera / the matrix is singular.
    #[derive(Debug, Clone, Copy, Default)]
    struct Vec3Result {
        ok: bool,
        value: [f32; 3],
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct RayResult {
        ok: bool,
        ray: Ray,
    }

    /// Mirrors `SFT::Engine::CameraContainment`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Containment {
        Outside,
        Intersecting,
        Inside,
    }

    unsafe extern "C++" {
        // The root bridge's generated header: `CameraDesc`'s full definition lives there.
        include!("sturdy_rs/bridge.h");
        include!("sturdy_rs/camera.hpp");

        #[namespace = "sturdy_rs"]
        type CameraDesc = crate::ffi::CameraDesc;

        /// `SFT::Engine::Camera`.
        type CameraRig;

        fn camera_rig_new(desc: &CameraDesc) -> UniquePtr<CameraRig>;
        fn camera_rig_desc(rig: &CameraRig) -> CameraDesc;
        fn camera_rig_apply(rig: Pin<&mut CameraRig>, desc: &CameraDesc);

        fn camera_look_at(rig: Pin<&mut CameraRig>, target: [f32; 3], world_up: [f32; 3]);
        fn camera_set_forward(rig: Pin<&mut CameraRig>, direction: [f32; 3], world_up: [f32; 3]);
        fn camera_translate_world(rig: Pin<&mut CameraRig>, delta: [f32; 3]);
        fn camera_translate_local(rig: Pin<&mut CameraRig>, delta: [f32; 3]);
        /// `rotation` is an xyzw quaternion.
        fn camera_rotate_local(rig: Pin<&mut CameraRig>, rotation: [f32; 4]);
        fn camera_rotate_world(rig: Pin<&mut CameraRig>, rotation: [f32; 4]);
        fn camera_yaw_pitch_roll(rig: Pin<&mut CameraRig>, yaw: f32, pitch: f32, roll: f32);
        fn camera_orbit(rig: Pin<&mut CameraRig>, pivot: [f32; 3], yaw: f32, pitch: f32);
        fn camera_dolly(rig: Pin<&mut CameraRig>, distance: f32);
        fn camera_pan(rig: Pin<&mut CameraRig>, local_distance: [f32; 2]);
        fn camera_zoom(rig: Pin<&mut CameraRig>, magnification: f32);
        fn camera_frame_sphere(rig: Pin<&mut CameraRig>, center: [f32; 3], radius: f32, padding: f32);
        fn camera_frame_bounds(rig: Pin<&mut CameraRig>, min: [f32; 3], max: [f32; 3], padding: f32);
        fn camera_set_viewport_size(rig: Pin<&mut CameraRig>, width: u32, height: u32);
        fn camera_set_jitter_pixels(rig: Pin<&mut CameraRig>, pixel_offset: [f32; 2], viewport_size: [f32; 2]);
        fn camera_clear_jitter(rig: Pin<&mut CameraRig>);

        fn camera_distance_to(rig: &CameraRig, point: [f32; 3]) -> f32;
        fn camera_forward(rig: &CameraRig) -> [f32; 3];
        fn camera_right(rig: &CameraRig) -> [f32; 3];
        fn camera_up(rig: &CameraRig) -> [f32; 3];
        fn camera_horizontal_fov_degrees(rig: &CameraRig) -> f32;
        fn camera_exposure_multiplier(rig: &CameraRig) -> f32;
        fn camera_ev100(rig: &CameraRig) -> f32;

        /// Column-major 4x4 matrices.
        fn camera_world_matrix(rig: &CameraRig) -> [f32; 16];
        fn camera_view_matrix(rig: &CameraRig) -> [f32; 16];
        fn camera_projection_matrix(rig: &CameraRig) -> [f32; 16];
        fn camera_view_projection_matrix(rig: &CameraRig) -> [f32; 16];
        fn camera_inverse_view_projection_matrix(rig: &CameraRig) -> [f32; 16];
        fn camera_previous_view_projection_matrix(rig: &CameraRig) -> [f32; 16];

        /// Screen space is top-left origin with depth in [0, 1].
        fn camera_project(rig: &CameraRig, world: [f32; 3], viewport_size: [f32; 2]) -> Vec3Result;
        fn camera_unproject(rig: &CameraRig, screen: [f32; 3], viewport_size: [f32; 2]) -> Vec3Result;
        fn camera_screen_ray(rig: &CameraRig, pixel: [f32; 2], viewport_size: [f32; 2]) -> RayResult;

        fn camera_sees_point(rig: &CameraRig, point: [f32; 3]) -> bool;
        fn camera_sees_sphere(rig: &CameraRig, center: [f32; 3], radius: f32) -> bool;
        fn camera_sees_bounds(rig: &CameraRig, min: [f32; 3], max: [f32; 3]) -> Containment;

        fn camera_commit_frame(rig: Pin<&mut CameraRig>);
        fn camera_reset_history(rig: Pin<&mut CameraRig>);
        fn camera_has_history(rig: &CameraRig) -> bool;
    }
}
