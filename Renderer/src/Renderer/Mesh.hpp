#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <Renderer/Handles.hpp>
#include <Renderer/Geometry.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {


    enum class Axis : u8 { X, Y, Z };

    struct UvSphereParams {
        f32 radius = 0.5f;
        u32 rings = 16;
        u32 segments = 32;
    };

    struct IcoSphereParams {
        f32 radius = 0.5f;
        u32 subdivisions = 2;
    };

    struct CylinderParams {
        f32 radius = 0.5f;
        f32 height = 1.0f;
        u32 radial_segments = 32;
        Axis axis = Axis::Y;
        bool capped = true;
    };

    struct ConeParams {
        f32 radius = 0.5f;
        f32 height = 1.0f;
        u32 radial_segments = 32;
        Axis axis = Axis::Y;
        bool capped = true;
    };

    struct CubeParams {
        f32 size = 1.0f;
    };

    struct RectangularPrismParams {
        glm::vec3 extents{1.0f, 1.0f, 1.0f};
    };

    struct TetrahedronParams {
        f32 size = 1.0f;
    };

    struct PlaneParams {
        f32 width = 1.0f;
        f32 depth = 1.0f;
        u32 width_segments = 1;
        u32 depth_segments = 1;
        Axis axis = Axis::Y;
    };

    struct TorusParams {
        f32 major_radius = 0.5f;
        f32 minor_radius = 0.2f;
        u32 major_segments = 32;
        u32 minor_segments = 16;
    };


    class Mesh {
      public:
        /// Constructs a `Mesh` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Mesh() = default;

        /// Creates a `Mesh` resource or value from the supplied parameters.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh create(const char *label = nullptr);


        /// Creates or converts a value from triangles representation.
        ///
        /// @param triangles `triangles` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh from_triangles(span<const Core::Triangle> triangles, const char *label = nullptr);


        /// Creates or converts a value from vertices representation.
        ///
        /// @param vertices `vertices` value used by the operation.
        /// @param indices `indices` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh from_vertices(span<const GeometryVertex> vertices, span<const u32> indices,
                                                 const char *label = nullptr);

        /// Performs the uv sphere operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh uv_sphere(const UvSphereParams &params = {}, const char *label = nullptr);
        /// Performs the ico sphere operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh ico_sphere(const IcoSphereParams &params = {}, const char *label = nullptr);
        /// Performs the cylinder operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh cylinder(const CylinderParams &params = {}, const char *label = nullptr);
        /// Performs the cone operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh cone(const ConeParams &params = {}, const char *label = nullptr);
        /// Performs the cube operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh cube(const CubeParams &params = {}, const char *label = nullptr);
        /// Performs the rectangular prism operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh rectangular_prism(const RectangularPrismParams &params = {},
                                                     const char *label = nullptr);
        /// Performs the tetrahedron operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh tetrahedron(const TetrahedronParams &params = {}, const char *label = nullptr);
        /// Performs the plane operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh plane(const PlaneParams &params = {}, const char *label = nullptr);
        /// Performs the torus operation for `Mesh` using the supplied arguments.
        ///
        /// @param params `params` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Mesh torus(const TorusParams &params = {}, const char *label = nullptr);

        /// Returns the current or globally available vertices value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const GeometryVertex> vertices() const noexcept;
        /// Returns the current or globally available indices value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const u32> indices() const noexcept;
        /// Returns the current or globally available label value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const UString &label() const noexcept;
        /// Sets the label for this `Mesh`.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_label(UString label) noexcept;
        /// Sets the vertex color for this `Mesh`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_vertex_color(const glm::vec4 &color) noexcept;


        /// Returns the triangle count for this `Mesh`.
        ///
        /// @return Returns the current triangle count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize triangle_count() const noexcept;


        /// Computes the estimated GPU bytes required by the supplied values.
        ///
        /// @return Returns the current estimated GPU bytes value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 estimated_gpu_bytes() const noexcept;


        /// Reports whether GPU resident holds for this `Mesh`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_gpu_resident() const noexcept;
        /// Returns the GPU handle associated with this `Mesh`.
        ///
        /// @return Returns the current GPU handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MeshHandle gpu_handle() const noexcept;

      private:
        friend class Renderer;
        /// Marks uploaded using the supplied arguments and current state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void mark_uploaded(MeshHandle handle) noexcept;
        /// Marks evicted using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void mark_evicted() noexcept;

        vector<GeometryVertex> vertices_;
        vector<u32> indices_;
        UString label_;
        MeshHandle handle_{};
        bool gpu_resident_ = false;
    };

} // namespace SFT::Renderer
