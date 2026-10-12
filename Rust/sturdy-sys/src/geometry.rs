//! Raw `cxx` bindings for the renderer's standalone geometry helpers: Catmull-Rom splines
//! (`Renderer/Spline.hpp`) and frustum culling (`Renderer/Culling.hpp`). Pure math; no engine.

#[cxx::bridge(namespace = "sturdy_rs::geometry")]
pub mod ffi {
    unsafe extern "C++" {
        include!("sturdy_rs/geometry.hpp");

        /// `SFT::Renderer::Spline3D` (`CatmullRomSpline<glm::vec3>`).
        type Spline3;
        /// `SFT::Renderer::Spline2D`.
        type Spline2;

        /// `waypoints` is flattened xyz triples. Fewer than two waypoints gives an invalid spline.
        fn spline3_create(waypoints: &[f32], samples_per_segment: u32) -> UniquePtr<Spline3>;
        fn spline3_valid(spline: &Spline3) -> bool;
        fn spline3_segment_count(spline: &Spline3) -> u32;
        fn spline3_position(spline: &Spline3, t: f32) -> [f32; 3];
        fn spline3_tangent(spline: &Spline3, t: f32) -> [f32; 3];
        fn spline3_total_length(spline: &Spline3) -> f32;
        fn spline3_t_at_arc_length(spline: &Spline3, arc_length: f32) -> f32;

        /// `waypoints` is flattened xy pairs.
        fn spline2_create(waypoints: &[f32], samples_per_segment: u32) -> UniquePtr<Spline2>;
        fn spline2_valid(spline: &Spline2) -> bool;
        fn spline2_segment_count(spline: &Spline2) -> u32;
        fn spline2_position(spline: &Spline2, t: f32) -> [f32; 2];
        fn spline2_tangent(spline: &Spline2, t: f32) -> [f32; 2];
        fn spline2_total_length(spline: &Spline2) -> f32;
        fn spline2_t_at_arc_length(spline: &Spline2, arc_length: f32) -> f32;

        /// `view_projection` is column-major (glam's layout). Returns the six planes as
        /// `[a, b, c, d]` quadruples, flattened.
        fn frustum_from_view_projection(view_projection: &[f32; 16]) -> [f32; 24];
        fn frustum_intersects_sphere(planes: &[f32; 24], center: [f32; 3], radius: f32) -> bool;
    }
}
