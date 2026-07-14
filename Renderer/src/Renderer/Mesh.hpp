#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include "Handles.hpp"
#include "Geometry.hpp"

using std::span;
using std::string;
using std::vector;

namespace SFT::Renderer {

    // Extrusion/revolution/normal axis for primitives that aren't rotationally symmetric about every
    // axis (cylinder, cone, plane). Follows the SolidWorks convention the caller asked for: Y means
    // "a profile in the XZ plane, extruded/revolved along Y" — i.e. the shape's natural axis when
    // authored is Y, and X/Z just remap that same shape onto a different axis.
    enum class Axis : u8 { X, Y, Z };

    struct UvSphereParams {
        f32 radius = 0.5f;
        u32 rings = 16;    // latitude segments (pole to pole)
        u32 segments = 32; // longitude segments (around the equator)
    };

    struct IcoSphereParams {
        f32 radius = 0.5f;
        u32 subdivisions = 2; // 0 = base icosahedron (20 faces)
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
        f32 size = 1.0f; // edge length
    };

    struct RectangularPrismParams {
        glm::vec3 extents{1.0f, 1.0f, 1.0f}; // full width (X), height (Y), depth (Z)
    };

    struct TetrahedronParams {
        f32 size = 1.0f; // edge length of the regular tetrahedron
    };

    struct PlaneParams {
        f32 width = 1.0f;  // extent along the tangential X-like axis
        f32 depth = 1.0f;  // extent along the tangential Z-like axis
        u32 width_segments = 1;
        u32 depth_segments = 1;
        Axis axis = Axis::Y; // direction the plane's normal faces
    };

    struct TorusParams {
        f32 major_radius = 0.5f; // distance from the center to the tube's centerline
        f32 minor_radius = 0.2f; // tube thickness
        u32 major_segments = 32; // segments around the ring
        u32 minor_segments = 16; // segments around the tube
    };

    // CPU-side mesh data: points + indices, plain value type (copying deep-copies the geometry, same
    // as copying a vector). A Mesh is CPU-resident from construction; it only becomes GPU-resident
    // once handed to Renderer::upload(), which stamps a handle back onto it. Copies of an uploaded
    // Mesh keep pointing at the same GPU resource (they share the handle), so passing meshes around
    // by value never implicitly re-uploads or duplicates GPU memory.
    class Mesh {
      public:
        Mesh() = default;

        [[nodiscard]] static Mesh create(const char *label = nullptr);

        // Builds a mesh from raw triangles in model space (origin at Vec3(0), Y+ up). Each triangle
        // gets its own three vertices with a flat face normal — geometry from disparate triangles
        // isn't assumed to share a smooth surface.
        [[nodiscard]] static Mesh from_triangles(span<const Core::Triangle> triangles, const char *label = nullptr);

        [[nodiscard]] static Mesh uv_sphere(const UvSphereParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh ico_sphere(const IcoSphereParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh cylinder(const CylinderParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh cone(const ConeParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh cube(const CubeParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh rectangular_prism(const RectangularPrismParams &params = {},
                                                     const char *label = nullptr);
        [[nodiscard]] static Mesh tetrahedron(const TetrahedronParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh plane(const PlaneParams &params = {}, const char *label = nullptr);
        [[nodiscard]] static Mesh torus(const TorusParams &params = {}, const char *label = nullptr);

        [[nodiscard]] span<const GeometryVertex> vertices() const noexcept { return vertices_; }
        [[nodiscard]] span<const u32> indices() const noexcept { return indices_; }
        [[nodiscard]] const string &label() const noexcept { return label_; }
        void set_label(string label) noexcept { label_ = std::move(label); }
        void set_vertex_color(const glm::vec4 &color) noexcept {
            for (GeometryVertex &vertex : vertices_) {
                vertex.color = color;
            }
        }

        // False until this exact Mesh (or a copy sharing its handle) has been uploaded via
        // Renderer::upload(). CPU-side vertices()/indices() stay populated either way — uploading
        // does not move the data off the CPU, it just also puts a copy on the GPU.
        [[nodiscard]] bool is_gpu_resident() const noexcept { return gpu_resident_; }
        [[nodiscard]] MeshHandle gpu_handle() const noexcept { return handle_; }

      private:
        friend class Renderer;
        void mark_uploaded(MeshHandle handle) noexcept {
            handle_ = handle;
            gpu_resident_ = true;
        }
        void mark_evicted() noexcept {
            handle_ = {};
            gpu_resident_ = false;
        }

        vector<GeometryVertex> vertices_;
        vector<u32> indices_;
        string label_;
        MeshHandle handle_{};
        bool gpu_resident_ = false;
    };

} // namespace SFT::Renderer
