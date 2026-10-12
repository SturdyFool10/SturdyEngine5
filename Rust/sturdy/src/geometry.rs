//! Standalone geometry helpers from the renderer: Catmull-Rom splines (camera paths, rails,
//! text-on-path) and view-frustum culling. Pure math, no [`crate::Engine`] needed.

use cxx::UniquePtr;
use glam::{Mat4, Vec2, Vec3};
use sturdy_sys::geometry::ffi;

macro_rules! spline {
    ($name:ident, $vec:ident, $n:literal, $create:ident, $valid:ident, $segs:ident, $pos:ident, $tan:ident, $len:ident, $arc:ident) => {
        /// A centripetal Catmull-Rom spline through its waypoints. Parameter `t` runs from 0 to
        /// [`segment_count`](Self::segment_count) (one unit per segment); `t_at_arc_length` converts
        /// a distance along the curve into `t` for constant-speed traversal.
        pub struct $name(UniquePtr<ffi::$name>);

        impl $name {
            /// `samples_per_segment` sets the resolution of the arc-length table (32 is the
            /// engine's default). Fewer than two waypoints gives a spline that is not
            /// [`valid`](Self::valid).
            pub fn new(waypoints: &[$vec], samples_per_segment: u32) -> Self {
                let flat: Vec<f32> = waypoints.iter().flat_map(|p| p.to_array()).collect();
                Self(ffi::$create(&flat, samples_per_segment))
            }

            pub fn valid(&self) -> bool {
                ffi::$valid(&self.0)
            }

            pub fn segment_count(&self) -> u32 {
                ffi::$segs(&self.0)
            }

            /// Point at `t` (clamped to `0..=segment_count`).
            pub fn position(&self, t: f32) -> $vec {
                $vec::from_array(ffi::$pos(&self.0, t))
            }

            /// Unnormalized tangent at `t`.
            pub fn tangent(&self, t: f32) -> $vec {
                $vec::from_array(ffi::$tan(&self.0, t))
            }

            pub fn total_length(&self) -> f32 {
                ffi::$len(&self.0)
            }

            /// The parameter `t` at which the curve has traveled `arc_length` from its start.
            pub fn t_at_arc_length(&self, arc_length: f32) -> f32 {
                ffi::$arc(&self.0, arc_length)
            }
        }
    };
}

spline!(Spline3, Vec3, 3, spline3_create, spline3_valid, spline3_segment_count, spline3_position, spline3_tangent, spline3_total_length, spline3_t_at_arc_length);
spline!(Spline2, Vec2, 2, spline2_create, spline2_valid, spline2_segment_count, spline2_position, spline2_tangent, spline2_total_length, spline2_t_at_arc_length);

/// Six view-frustum planes extracted from a view-projection matrix.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Frustum {
    planes: [f32; 24],
}

impl Frustum {
    pub fn from_view_projection(view_projection: Mat4) -> Self {
        Self { planes: ffi::frustum_from_view_projection(&view_projection.to_cols_array()) }
    }

    /// Whether a sphere is at least partly inside the frustum.
    pub fn intersects_sphere(&self, center: Vec3, radius: f32) -> bool {
        ffi::frustum_intersects_sphere(&self.planes, center.to_array(), radius)
    }
}
