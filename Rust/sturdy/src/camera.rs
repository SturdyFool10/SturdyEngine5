//! The engine's full camera model.
//!
//! [`Camera`] is a plain value describing every camera property the engine has — pose, projection
//! (perspective / orthographic / custom matrix), physical lens and exposure (focal length, sensor,
//! f-stop, shutter, ISO, focus distance, aperture blades, exposure compensation), sub-pixel jitter,
//! lens shift, reverse-Z, culling mask, priority, clear color, render scale and normalized viewport.
//! Return one from [`crate::GameLogic::frame`] inside a [`crate::FrameDescription`]; the engine keeps
//! one live camera per window and carries its temporal history (the previous view-projection that
//! motion blur and other temporal effects need) across frames for you. Set
//! [`Camera::reset_history`] (via [`Camera::cut`]) on a teleport or scene change to drop it.
//!
//! [`CameraRig`] is the same camera as a live engine object, for the math around it: moving,
//! orbiting, panning and framing helpers, view/projection matrices, world<->screen conversion,
//! picking rays and frustum queries. [`Camera::rig`] builds one; [`CameraRig::camera`] reads the
//! state back out as a [`Camera`].

use cxx::UniquePtr;
use glam::{Mat4, Quat, Vec2, Vec3, Vec4};
use sturdy_sys::camera::ffi as rig_ffi;
use sturdy_sys::ffi;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Projection {
    Perspective { vertical_fov_degrees: f32 },
    Orthographic { vertical_size: f32 },
    /// An arbitrary projection matrix (column-major, as glam's [`Mat4`]).
    Custom(Mat4),
}

/// A camera described by value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Camera {
    pub projection: Projection,
    pub position: Vec3,
    /// Pitch, yaw, roll in degrees. Ignored when [`Camera::orientation`] is set.
    pub euler_degrees: Vec3,
    /// Exact orientation; takes precedence over `euler_degrees`.
    pub orientation: Option<Quat>,
    /// `None` uses the framebuffer's aspect ratio.
    pub aspect_ratio: Option<f32>,
    pub near_clip: f32,
    pub far_clip: f32,
    /// Off-axis lens shift, in fractions of the frame.
    pub lens_shift: Vec2,
    /// Sub-pixel projection jitter in NDC (for temporal anti-aliasing).
    pub jitter_ndc: Vec2,
    pub reverse_z: bool,
    /// Physical focal length; when set it overrides the perspective FOV (the two stay in sync).
    pub focal_length_mm: Option<f32>,
    pub sensor_size_mm: Vec2,
    pub aperture_f_stop: f32,
    pub shutter_seconds: f32,
    pub iso: f32,
    pub focus_distance: f32,
    pub aperture_blades: u32,
    pub exposure_compensation_ev: f32,
    /// Bitmask matched against renderables' visibility layers.
    pub culling_mask: u32,
    /// Higher-priority cameras render later / on top when several are active.
    pub priority: i32,
    pub active: bool,
    pub clear_color: Vec4,
    pub render_scale: f32,
    /// `x, y, width, height` in 0..1 of the window.
    pub normalized_viewport: Vec4,
    /// Drop temporal history before this frame (a camera cut).
    pub reset_history: bool,
}

impl Camera {
    pub const fn perspective() -> Self {
        Self::with(Projection::Perspective { vertical_fov_degrees: 60.0 })
    }

    pub const fn orthographic() -> Self {
        Self::with(Projection::Orthographic { vertical_size: 10.0 })
    }

    const fn with(projection: Projection) -> Self {
        Self {
            projection,
            position: Vec3::ZERO,
            euler_degrees: Vec3::ZERO,
            orientation: None,
            aspect_ratio: None,
            near_clip: 0.05,
            far_clip: 1000.0,
            lens_shift: Vec2::ZERO,
            jitter_ndc: Vec2::ZERO,
            reverse_z: false,
            focal_length_mm: None,
            sensor_size_mm: Vec2::new(36.0, 24.0),
            aperture_f_stop: 2.8,
            shutter_seconds: 1.0 / 60.0,
            iso: 100.0,
            focus_distance: 10.0,
            aperture_blades: 7,
            exposure_compensation_ev: 0.0,
            culling_mask: u32::MAX,
            priority: 0,
            active: true,
            clear_color: Vec4::new(0.01, 0.015, 0.025, 1.0),
            render_scale: 1.0,
            normalized_viewport: Vec4::new(0.0, 0.0, 1.0, 1.0),
            reset_history: false,
        }
    }

    #[must_use]
    pub fn at(mut self, position: impl Into<Vec3>) -> Self {
        self.position = position.into();
        self
    }

    #[must_use]
    pub fn euler_degrees(mut self, pitch_yaw_roll: impl Into<Vec3>) -> Self {
        self.euler_degrees = pitch_yaw_roll.into();
        self
    }

    /// Sets an exact orientation (overrides [`Camera::euler_degrees`]).
    #[must_use]
    pub fn orientation(mut self, orientation: Quat) -> Self {
        self.orientation = Some(orientation);
        self
    }

    #[must_use]
    pub const fn fov_degrees(mut self, degrees: f32) -> Self {
        self.projection = Projection::Perspective { vertical_fov_degrees: degrees };
        self.focal_length_mm = None;
        self
    }

    #[must_use]
    pub const fn clip(mut self, near: f32, far: f32) -> Self {
        self.near_clip = near;
        self.far_clip = far;
        self
    }

    #[must_use]
    pub const fn orthographic_size(mut self, vertical_size: f32) -> Self {
        self.projection = Projection::Orthographic { vertical_size };
        self
    }

    #[must_use]
    pub const fn custom_projection(mut self, projection: Mat4) -> Self {
        self.projection = Projection::Custom(projection);
        self
    }

    /// Physical lens: focal length in millimetres (overrides the FOV).
    #[must_use]
    pub const fn focal_length_mm(mut self, millimeters: f32) -> Self {
        self.focal_length_mm = Some(millimeters);
        self
    }

    #[must_use]
    pub const fn sensor_size_mm(mut self, size: Vec2) -> Self {
        self.sensor_size_mm = size;
        self
    }

    /// Exposure triangle: f-stop, shutter time in seconds, ISO.
    #[must_use]
    pub const fn exposure(mut self, f_stop: f32, shutter_seconds: f32, iso: f32) -> Self {
        self.aperture_f_stop = f_stop;
        self.shutter_seconds = shutter_seconds;
        self.iso = iso;
        self
    }

    #[must_use]
    pub const fn exposure_compensation_ev(mut self, ev: f32) -> Self {
        self.exposure_compensation_ev = ev;
        self
    }

    /// Depth-of-field controls: focus distance in metres and aperture blade count.
    #[must_use]
    pub const fn focus(mut self, distance: f32, aperture_blades: u32) -> Self {
        self.focus_distance = distance;
        self.aperture_blades = aperture_blades;
        self
    }

    #[must_use]
    pub const fn jitter_ndc(mut self, jitter: Vec2) -> Self {
        self.jitter_ndc = jitter;
        self
    }

    #[must_use]
    pub const fn lens_shift(mut self, shift: Vec2) -> Self {
        self.lens_shift = shift;
        self
    }

    #[must_use]
    pub const fn reverse_z(mut self, enabled: bool) -> Self {
        self.reverse_z = enabled;
        self
    }

    #[must_use]
    pub const fn culling_mask(mut self, mask: u32) -> Self {
        self.culling_mask = mask;
        self
    }

    #[must_use]
    pub const fn priority(mut self, priority: i32) -> Self {
        self.priority = priority;
        self
    }

    #[must_use]
    pub const fn clear_color(mut self, color: Vec4) -> Self {
        self.clear_color = color;
        self
    }

    #[must_use]
    pub const fn render_scale(mut self, scale: f32) -> Self {
        self.render_scale = scale;
        self
    }

    /// Renders into a sub-rectangle of the window (`x, y, width, height`, each 0..1).
    #[must_use]
    pub const fn viewport(mut self, normalized: Vec4) -> Self {
        self.normalized_viewport = normalized;
        self
    }

    /// Marks this frame as a camera cut: temporal history is dropped.
    #[must_use]
    pub const fn cut(mut self) -> Self {
        self.reset_history = true;
        self
    }

    /// Builds a live [`CameraRig`] from this description (for the math helpers).
    pub fn rig(&self, framebuffer_aspect: f32) -> CameraRig {
        CameraRig { inner: rig_ffi::camera_rig_new(&self.to_ffi(framebuffer_aspect)) }
    }

    pub(crate) fn to_ffi(self, framebuffer_aspect: f32) -> ffi::CameraDesc {
        let (kind, fov, size, custom) = match self.projection {
            Projection::Perspective { vertical_fov_degrees } => {
                (ffi::CameraProjection::Perspective, vertical_fov_degrees, 10.0, Mat4::IDENTITY)
            }
            Projection::Orthographic { vertical_size } => {
                (ffi::CameraProjection::Orthographic, 60.0, vertical_size, Mat4::IDENTITY)
            }
            Projection::Custom(m) => (ffi::CameraProjection::Custom, 60.0, 10.0, m),
        };
        ffi::CameraDesc {
            projection: kind,
            position: self.position.into(),
            euler_degrees: self.euler_degrees.into(),
            has_orientation: self.orientation.is_some(),
            orientation: self.orientation.unwrap_or(Quat::IDENTITY).to_array(),
            vertical_fov_degrees: fov,
            orthographic_size: size,
            custom_projection: custom.to_cols_array(),
            aspect_ratio: self.aspect_ratio.unwrap_or(framebuffer_aspect),
            near_clip: self.near_clip,
            far_clip: self.far_clip,
            lens_shift: self.lens_shift.into(),
            jitter_ndc: self.jitter_ndc.into(),
            reverse_z: self.reverse_z,
            has_focal_length: self.focal_length_mm.is_some(),
            focal_length_mm: self.focal_length_mm.unwrap_or(0.0),
            sensor_size_mm: self.sensor_size_mm.into(),
            aperture_f_stop: self.aperture_f_stop,
            shutter_seconds: self.shutter_seconds,
            iso: self.iso,
            focus_distance: self.focus_distance,
            aperture_blades: self.aperture_blades,
            exposure_compensation_ev: self.exposure_compensation_ev,
            culling_mask: self.culling_mask,
            priority: self.priority,
            active: self.active,
            clear_color: self.clear_color.into(),
            render_scale: self.render_scale,
            normalized_viewport: self.normalized_viewport.into(),
            reset_history: self.reset_history,
        }
    }

    fn from_ffi(d: &ffi::CameraDesc) -> Self {
        let projection = match d.projection {
            ffi::CameraProjection::Orthographic => Projection::Orthographic { vertical_size: d.orthographic_size },
            ffi::CameraProjection::Custom => Projection::Custom(Mat4::from_cols_array(&d.custom_projection)),
            _ => Projection::Perspective { vertical_fov_degrees: d.vertical_fov_degrees },
        };
        Self {
            projection,
            position: Vec3::from(d.position),
            euler_degrees: Vec3::from(d.euler_degrees),
            orientation: d.has_orientation.then(|| Quat::from_array(d.orientation)),
            aspect_ratio: Some(d.aspect_ratio),
            near_clip: d.near_clip,
            far_clip: d.far_clip,
            lens_shift: Vec2::from(d.lens_shift),
            jitter_ndc: Vec2::from(d.jitter_ndc),
            reverse_z: d.reverse_z,
            focal_length_mm: d.has_focal_length.then_some(d.focal_length_mm),
            sensor_size_mm: Vec2::from(d.sensor_size_mm),
            aperture_f_stop: d.aperture_f_stop,
            shutter_seconds: d.shutter_seconds,
            iso: d.iso,
            focus_distance: d.focus_distance,
            aperture_blades: d.aperture_blades,
            exposure_compensation_ev: d.exposure_compensation_ev,
            culling_mask: d.culling_mask,
            priority: d.priority,
            active: d.active,
            clear_color: Vec4::from(d.clear_color),
            render_scale: d.render_scale,
            normalized_viewport: Vec4::from(d.normalized_viewport),
            reset_history: false,
        }
    }
}

impl Default for Camera {
    fn default() -> Self {
        Self::perspective()
    }
}

/// A world-space ray.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Ray {
    pub origin: Vec3,
    pub direction: Vec3,
}

/// How a bounding box relates to a camera's view frustum.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Containment {
    Outside,
    Intersecting,
    Inside,
}

/// A live engine camera; see the module docs. Angles are in degrees, screen coordinates have a
/// top-left origin with depth in `0..=1`.
pub struct CameraRig {
    inner: UniquePtr<rig_ffi::CameraRig>,
}

impl CameraRig {
    /// The rig's full current state as a [`Camera`] (with an exact `orientation`).
    pub fn camera(&self) -> Camera {
        Camera::from_ffi(&rig_ffi::camera_rig_desc(&self.inner))
    }

    /// Replaces the rig's state (history is kept unless `camera.reset_history` is set).
    pub fn set_camera(&mut self, camera: &Camera, framebuffer_aspect: f32) {
        rig_ffi::camera_rig_apply(self.inner.pin_mut(), &camera.to_ffi(framebuffer_aspect));
    }

    // -- Movement / framing --------------------------------------------------------------------

    pub fn look_at(&mut self, target: Vec3, world_up: Vec3) {
        rig_ffi::camera_look_at(self.inner.pin_mut(), target.into(), world_up.into());
    }
    pub fn set_forward(&mut self, direction: Vec3, world_up: Vec3) {
        rig_ffi::camera_set_forward(self.inner.pin_mut(), direction.into(), world_up.into());
    }
    pub fn translate_world(&mut self, delta: Vec3) {
        rig_ffi::camera_translate_world(self.inner.pin_mut(), delta.into());
    }
    pub fn translate_local(&mut self, delta: Vec3) {
        rig_ffi::camera_translate_local(self.inner.pin_mut(), delta.into());
    }
    pub fn rotate_local(&mut self, rotation: Quat) {
        rig_ffi::camera_rotate_local(self.inner.pin_mut(), rotation.to_array());
    }
    pub fn rotate_world(&mut self, rotation: Quat) {
        rig_ffi::camera_rotate_world(self.inner.pin_mut(), rotation.to_array());
    }
    pub fn yaw_pitch_roll(&mut self, yaw_degrees: f32, pitch_degrees: f32, roll_degrees: f32) {
        rig_ffi::camera_yaw_pitch_roll(self.inner.pin_mut(), yaw_degrees, pitch_degrees, roll_degrees);
    }
    /// Orbits around `pivot`, keeping the distance to it.
    pub fn orbit(&mut self, pivot: Vec3, yaw_degrees: f32, pitch_degrees: f32) {
        rig_ffi::camera_orbit(self.inner.pin_mut(), pivot.into(), yaw_degrees, pitch_degrees);
    }
    /// Moves along the view direction.
    pub fn dolly(&mut self, distance: f32) {
        rig_ffi::camera_dolly(self.inner.pin_mut(), distance);
    }
    /// Moves in the camera's right/up plane.
    pub fn pan(&mut self, local_distance: Vec2) {
        rig_ffi::camera_pan(self.inner.pin_mut(), local_distance.into());
    }
    /// Changes magnification (FOV for perspective, size for orthographic).
    pub fn zoom(&mut self, magnification: f32) {
        rig_ffi::camera_zoom(self.inner.pin_mut(), magnification);
    }
    /// Backs off along the view direction until the sphere fills the view (with `padding`, e.g. 1.1).
    pub fn frame_sphere(&mut self, center: Vec3, radius: f32, padding: f32) {
        rig_ffi::camera_frame_sphere(self.inner.pin_mut(), center.into(), radius, padding);
    }
    pub fn frame_bounds(&mut self, min: Vec3, max: Vec3, padding: f32) {
        rig_ffi::camera_frame_bounds(self.inner.pin_mut(), min.into(), max.into(), padding);
    }
    pub fn set_viewport_size(&mut self, width: u32, height: u32) {
        rig_ffi::camera_set_viewport_size(self.inner.pin_mut(), width, height);
    }
    pub fn set_jitter_pixels(&mut self, pixel_offset: Vec2, viewport_size: Vec2) {
        rig_ffi::camera_set_jitter_pixels(self.inner.pin_mut(), pixel_offset.into(), viewport_size.into());
    }
    pub fn clear_jitter(&mut self) {
        rig_ffi::camera_clear_jitter(self.inner.pin_mut());
    }

    // -- Queries -------------------------------------------------------------------------------

    pub fn distance_to(&self, point: Vec3) -> f32 {
        rig_ffi::camera_distance_to(&self.inner, point.into())
    }
    pub fn forward(&self) -> Vec3 {
        Vec3::from(rig_ffi::camera_forward(&self.inner))
    }
    pub fn right(&self) -> Vec3 {
        Vec3::from(rig_ffi::camera_right(&self.inner))
    }
    pub fn up(&self) -> Vec3 {
        Vec3::from(rig_ffi::camera_up(&self.inner))
    }
    pub fn horizontal_fov_degrees(&self) -> f32 {
        rig_ffi::camera_horizontal_fov_degrees(&self.inner)
    }
    /// Scene-linear exposure multiplier from f-stop, shutter, ISO and compensation.
    pub fn exposure_multiplier(&self) -> f32 {
        rig_ffi::camera_exposure_multiplier(&self.inner)
    }
    /// Exposure value at ISO 100.
    pub fn ev100(&self) -> f32 {
        rig_ffi::camera_ev100(&self.inner)
    }

    pub fn world_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_world_matrix(&self.inner))
    }
    pub fn view_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_view_matrix(&self.inner))
    }
    pub fn projection_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_projection_matrix(&self.inner))
    }
    pub fn view_projection_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_view_projection_matrix(&self.inner))
    }
    pub fn inverse_view_projection_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_inverse_view_projection_matrix(&self.inner))
    }
    /// The view-projection committed by the last [`CameraRig::commit_frame`].
    pub fn previous_view_projection_matrix(&self) -> Mat4 {
        Mat4::from_cols_array(&rig_ffi::camera_previous_view_projection_matrix(&self.inner))
    }

    /// World point to screen pixels (`x, y`) and depth (`z`). `None` only for a degenerate
    /// viewport/matrix: the engine does not reject points behind the camera, so check that `z` is
    /// within `0..=1` for "visible between the near and far planes".
    pub fn project(&self, world: Vec3, viewport_size: Vec2) -> Option<Vec3> {
        let r = rig_ffi::camera_project(&self.inner, world.into(), viewport_size.into());
        r.ok.then(|| Vec3::from(r.value))
    }
    /// Screen pixels + depth back to a world point.
    pub fn unproject(&self, screen: Vec3, viewport_size: Vec2) -> Option<Vec3> {
        let r = rig_ffi::camera_unproject(&self.inner, screen.into(), viewport_size.into());
        r.ok.then(|| Vec3::from(r.value))
    }
    /// The picking ray through a screen pixel.
    pub fn screen_ray(&self, pixel: Vec2, viewport_size: Vec2) -> Option<Ray> {
        let r = rig_ffi::camera_screen_ray(&self.inner, pixel.into(), viewport_size.into());
        r.ok.then(|| Ray { origin: Vec3::from(r.ray.origin), direction: Vec3::from(r.ray.direction) })
    }
    pub fn sees_point(&self, point: Vec3) -> bool {
        rig_ffi::camera_sees_point(&self.inner, point.into())
    }
    pub fn sees_sphere(&self, center: Vec3, radius: f32) -> bool {
        rig_ffi::camera_sees_sphere(&self.inner, center.into(), radius)
    }
    pub fn sees_bounds(&self, min: Vec3, max: Vec3) -> Containment {
        match rig_ffi::camera_sees_bounds(&self.inner, min.into(), max.into()) {
            rig_ffi::Containment::Inside => Containment::Inside,
            rig_ffi::Containment::Intersecting => Containment::Intersecting,
            _ => Containment::Outside,
        }
    }

    // -- Temporal history ----------------------------------------------------------------------

    /// Records the current view-projection as "previous" for the next frame.
    pub fn commit_frame(&mut self) {
        rig_ffi::camera_commit_frame(self.inner.pin_mut());
    }
    pub fn reset_history(&mut self) {
        rig_ffi::camera_reset_history(self.inner.pin_mut());
    }
    pub fn has_history(&self) -> bool {
        rig_ffi::camera_has_history(&self.inner)
    }
}
