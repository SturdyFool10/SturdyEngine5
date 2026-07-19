// CPU-side geometry generation for Mesh's built-in primitive constructors. Every generator builds
// its shape canonically along +Y (SolidWorks-style: the profile lives in the XZ plane, extruded or
// revolved along Y) and, where an Axis parameter exists, a final basis-remap rotates that canonical
// shape onto the requested axis. Winding is authored as CCW-outward / right-handed; grid- and
// fan-based generators verify this per-triangle against an already-known-correct normal rather than
// relying on hand-derived trig signs, so a sign slip degrades to "still convex, still correct" rather
// than a silently inside-out mesh.
#pragma region Imports
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Renderer/Mesh.hpp>
#include <Renderer/Geometry.hpp>
#include <Core/Core.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {

    namespace {

        glm::vec3 remap_axis(glm::vec3 p, Axis axis) noexcept {
            switch (axis) {
                case Axis::X: return glm::vec3(p.y, -p.x, p.z);
                case Axis::Z: return glm::vec3(p.x, -p.z, p.y);
                case Axis::Y: default: return p;
            }
        }

        // Reorders (i0,i1,i2) so the triangle's own geometric winding agrees with the already-known
        // correct outward direction carried by the vertices themselves (their averaged normal).
        void emit_smooth_triangle(const vector<GeometryVertex> &verts, vector<u32> &idx, u32 i0, u32 i1, u32 i2) {
            const glm::vec3 face_normal =
                glm::cross(verts[i1].position - verts[i0].position, verts[i2].position - verts[i0].position);
            const glm::vec3 avg_normal = verts[i0].normal + verts[i1].normal + verts[i2].normal;
            if (glm::dot(face_normal, avg_normal) < 0.0f) {
                idx.push_back(i0);
                idx.push_back(i2);
                idx.push_back(i1);
            } else {
                idx.push_back(i0);
                idx.push_back(i1);
                idx.push_back(i2);
            }
        }

        void append_smooth_quad(const vector<GeometryVertex> &verts, vector<u32> &idx, u32 i0, u32 i1, u32 i2, u32 i3) {
            emit_smooth_triangle(verts, idx, i0, i1, i2);
            emit_smooth_triangle(verts, idx, i0, i2, i3);
        }

        // Flat-shaded triangle for faceted convex shapes: computes its own face normal and flips
        // winding if that normal doesn't point away from `shape_center`, so caller-supplied vertex
        // order never has to be pre-verified by hand.
        void append_flat_triangle(vector<GeometryVertex> &verts, vector<u32> &idx, glm::vec3 a, glm::vec3 b,
                                   glm::vec3 c, glm::vec3 shape_center) {
            glm::vec3 normal = glm::cross(b - a, c - a);
            const glm::vec3 face_center = (a + b + c) / 3.0f;
            if (glm::dot(normal, face_center - shape_center) < 0.0f) {
                std::swap(b, c);
                normal = -normal;
            }
            normal = glm::normalize(normal);
            const u32 base = static_cast<u32>(verts.size());
            verts.push_back(GeometryVertex{.position = a, .normal = normal, .uv = {0.0f, 0.0f}});
            verts.push_back(GeometryVertex{.position = b, .normal = normal, .uv = {1.0f, 0.0f}});
            verts.push_back(GeometryVertex{.position = c, .normal = normal, .uv = {0.0f, 1.0f}});
            idx.push_back(base);
            idx.push_back(base + 1);
            idx.push_back(base + 2);
        }

        struct BoxFace {
            int a, b, c; // axis indices with a x b = c (right-handed cyclic order)
            f32 sign;
        };

        void append_box(vector<GeometryVertex> &verts, vector<u32> &idx, glm::vec3 half) {
            constexpr BoxFace faces[6] = {
                {0, 1, 2, 1.0f}, {0, 1, 2, -1.0f}, // +-X
                {1, 2, 0, 1.0f}, {1, 2, 0, -1.0f}, // +-Y
                {2, 0, 1, 1.0f}, {2, 0, 1, -1.0f}, // +-Z
            };
            constexpr glm::vec2 corner_uv[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            constexpr glm::vec2 corner_sign[4] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

            for (const BoxFace &f : faces) {
                glm::vec3 normal{};
                normal[f.a] = f.sign;
                const u32 base = static_cast<u32>(verts.size());
                for (int k = 0; k < 4; ++k) {
                    // Negative faces walk the corner list backwards, which flips winding to match -a.
                    const int order = f.sign > 0.0f ? k : (3 - k);
                    glm::vec3 p{};
                    p[f.a] = f.sign * half[f.a];
                    p[f.b] = corner_sign[order].x * half[f.b];
                    p[f.c] = corner_sign[order].y * half[f.c];
                    verts.push_back(GeometryVertex{.position = p, .normal = normal, .uv = corner_uv[k]});
                }
                idx.push_back(base);
                idx.push_back(base + 1);
                idx.push_back(base + 2);
                idx.push_back(base);
                idx.push_back(base + 2);
                idx.push_back(base + 3);
            }
        }

        void append_disc_cap(vector<GeometryVertex> &verts, vector<u32> &idx, f32 radius, f32 y,
                              u32 radial_segments, Axis axis, bool facing_down) {
            const f32 pi = std::numbers::pi_v<f32>;
            const glm::vec3 normal = remap_axis(glm::vec3(0.0f, facing_down ? -1.0f : 1.0f, 0.0f), axis);

            const u32 center_index = static_cast<u32>(verts.size());
            verts.push_back(GeometryVertex{
                .position = remap_axis(glm::vec3(0.0f, y, 0.0f), axis), .normal = normal, .uv = {0.5f, 0.5f}});

            const u32 ring_base = static_cast<u32>(verts.size());
            for (u32 seg = 0; seg <= radial_segments; ++seg) {
                const f32 u = static_cast<f32>(seg) / static_cast<f32>(radial_segments);
                const f32 phi = u * 2.0f * pi;
                const glm::vec3 position(radius * std::cos(phi), y, radius * std::sin(phi));
                verts.push_back(GeometryVertex{
                    .position = remap_axis(position, axis),
                    .normal = normal,
                    .uv = {0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi)},
                });
            }
            for (u32 seg = 0; seg < radial_segments; ++seg) {
                emit_smooth_triangle(verts, idx, center_index, ring_base + seg, ring_base + seg + 1);
            }
        }

        // Shared builder for both cylinder (bottom_radius == top_radius) and cone (top_radius == 0).
        void append_frustum(vector<GeometryVertex> &verts, vector<u32> &idx, f32 bottom_radius, f32 top_radius,
                             f32 height, u32 radial_segments, bool capped, Axis axis) {
            radial_segments = radial_segments < 3 ? 3 : radial_segments;
            const f32 pi = std::numbers::pi_v<f32>;
            const f32 half_h = height * 0.5f;
            const f32 delta_r = top_radius - bottom_radius;

            const u32 side_base = static_cast<u32>(verts.size());
            for (u32 ring = 0; ring <= 1; ++ring) {
                const f32 v = static_cast<f32>(ring);
                const f32 y = -half_h + v * height;
                const f32 radius = bottom_radius + v * delta_r;
                for (u32 seg = 0; seg <= radial_segments; ++seg) {
                    const f32 u = static_cast<f32>(seg) / static_cast<f32>(radial_segments);
                    const f32 phi = u * 2.0f * pi;
                    const f32 cos_phi = std::cos(phi);
                    const f32 sin_phi = std::sin(phi);
                    const glm::vec3 position(radius * cos_phi, y, radius * sin_phi);
                    // Exact tangent-plane normal for a cone frustum: tilts away from pure-radial by
                    // an amount proportional to the taper (delta_r), so a cone doesn't shade like a
                    // cylinder with a pinched cap.
                    const glm::vec3 normal =
                        glm::normalize(glm::vec3(height * cos_phi, bottom_radius - top_radius, height * sin_phi));
                    verts.push_back(GeometryVertex{
                        .position = remap_axis(position, axis),
                        .normal = remap_axis(normal, axis),
                        .uv = {u, v},
                    });
                }
            }

            const u32 row_stride = radial_segments + 1;
            for (u32 seg = 0; seg < radial_segments; ++seg) {
                const u32 i0 = side_base + seg;
                const u32 i1 = side_base + row_stride + seg;
                const u32 i2 = i1 + 1;
                const u32 i3 = i0 + 1;
                append_smooth_quad(verts, idx, i0, i1, i2, i3);
            }

            if (capped) {
                if (bottom_radius > 0.0f) {
                    append_disc_cap(verts, idx, bottom_radius, -half_h, radial_segments, axis, true);
                }
                if (top_radius > 0.0f) {
                    append_disc_cap(verts, idx, top_radius, half_h, radial_segments, axis, false);
                }
            }
        }

    } // namespace

    Mesh Mesh::create(const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        return mesh;
    }

    Mesh Mesh::from_triangles(span<const Core::Triangle> triangles, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        mesh.vertices_.reserve(triangles.size() * 3);
        mesh.indices_.reserve(triangles.size() * 3);
        for (const Core::Triangle &tri : triangles) {
            const glm::vec3 normal = glm::normalize(glm::cross(tri.b - tri.a, tri.c - tri.a));
            const u32 base = static_cast<u32>(mesh.vertices_.size());
            mesh.vertices_.push_back(GeometryVertex{.position = tri.a, .normal = normal, .uv = {0.0f, 0.0f}});
            mesh.vertices_.push_back(GeometryVertex{.position = tri.b, .normal = normal, .uv = {1.0f, 0.0f}});
            mesh.vertices_.push_back(GeometryVertex{.position = tri.c, .normal = normal, .uv = {0.0f, 1.0f}});
            mesh.indices_.push_back(base);
            mesh.indices_.push_back(base + 1);
            mesh.indices_.push_back(base + 2);
        }
        return mesh;
    }

    Mesh Mesh::uv_sphere(const UvSphereParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        const u32 rings = params.rings < 2 ? 2 : params.rings;
        const u32 segments = params.segments < 3 ? 3 : params.segments;
        const f32 pi = std::numbers::pi_v<f32>;

        vector<GeometryVertex> &verts = mesh.vertices_;
        verts.reserve(static_cast<usize>(rings + 1) * (segments + 1));
        for (u32 ring = 0; ring <= rings; ++ring) {
            const f32 v = static_cast<f32>(ring) / static_cast<f32>(rings);
            const f32 theta = v * pi;
            const f32 sin_theta = std::sin(theta);
            const f32 cos_theta = std::cos(theta);
            for (u32 seg = 0; seg <= segments; ++seg) {
                const f32 u = static_cast<f32>(seg) / static_cast<f32>(segments);
                const f32 phi = u * 2.0f * pi;
                const glm::vec3 dir(sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi));
                verts.push_back(GeometryVertex{.position = dir * params.radius, .normal = dir, .uv = {u, v}});
            }
        }

        vector<u32> &idx = mesh.indices_;
        const u32 row_stride = segments + 1;
        for (u32 ring = 0; ring < rings; ++ring) {
            for (u32 seg = 0; seg < segments; ++seg) {
                const u32 i0 = ring * row_stride + seg;
                const u32 i1 = i0 + row_stride;
                const u32 i2 = i1 + 1;
                const u32 i3 = i0 + 1;
                append_smooth_quad(verts, idx, i0, i1, i2, i3);
            }
        }
        return mesh;
    }

    Mesh Mesh::ico_sphere(const IcoSphereParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";

        const f32 golden = (1.0f + std::sqrt(5.0f)) * 0.5f;
        vector<glm::vec3> positions = {
            glm::normalize(glm::vec3(-1.0f, golden, 0.0f)), glm::normalize(glm::vec3(1.0f, golden, 0.0f)),
            glm::normalize(glm::vec3(-1.0f, -golden, 0.0f)), glm::normalize(glm::vec3(1.0f, -golden, 0.0f)),
            glm::normalize(glm::vec3(0.0f, -1.0f, golden)), glm::normalize(glm::vec3(0.0f, 1.0f, golden)),
            glm::normalize(glm::vec3(0.0f, -1.0f, -golden)), glm::normalize(glm::vec3(0.0f, 1.0f, -golden)),
            glm::normalize(glm::vec3(golden, 0.0f, -1.0f)), glm::normalize(glm::vec3(golden, 0.0f, 1.0f)),
            glm::normalize(glm::vec3(-golden, 0.0f, -1.0f)), glm::normalize(glm::vec3(-golden, 0.0f, 1.0f)),
        };

        struct Face {
            u32 a, b, c;
        };
        vector<Face> faces = {
            {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9},  {5, 11, 4}, {11, 10, 2},
            {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},  {4, 9, 5},
            {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
        };

        for (u32 level = 0; level < params.subdivisions; ++level) {
            vector<Face> next;
            next.reserve(faces.size() * 4);
            for (const Face &f : faces) {
                const glm::vec3 ab = glm::normalize(positions[f.a] + positions[f.b]);
                const glm::vec3 bc = glm::normalize(positions[f.b] + positions[f.c]);
                const glm::vec3 ca = glm::normalize(positions[f.c] + positions[f.a]);
                const u32 iab = static_cast<u32>(positions.size());
                positions.push_back(ab);
                const u32 ibc = static_cast<u32>(positions.size());
                positions.push_back(bc);
                const u32 ica = static_cast<u32>(positions.size());
                positions.push_back(ca);
                next.push_back({f.a, iab, ica});
                next.push_back({f.b, ibc, iab});
                next.push_back({f.c, ica, ibc});
                next.push_back({iab, ibc, ica});
            }
            faces = std::move(next);
        }

        mesh.vertices_.reserve(faces.size() * 3);
        mesh.indices_.reserve(faces.size() * 3);
        const f32 pi = std::numbers::pi_v<f32>;
        for (const Face &f : faces) {
            u32 a = f.a, b = f.b, c = f.c;
            const glm::vec3 face_normal = glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
            if (glm::dot(face_normal, positions[a] + positions[b] + positions[c]) < 0.0f) {
                std::swap(b, c);
            }
            for (u32 i : {a, b, c}) {
                const glm::vec3 dir = positions[static_cast<usize>(i)];
                const u32 vertex_index = static_cast<u32>(mesh.vertices_.size());
                const f32 u = 0.5f + std::atan2(dir.z, dir.x) / (2.0f * pi);
                const f32 v = 0.5f - std::asin(dir.y) / pi;
                mesh.vertices_.push_back(
                    GeometryVertex{.position = dir * params.radius, .normal = dir, .uv = {u, v}});
                mesh.indices_.push_back(vertex_index);
            }
        }
        return mesh;
    }

    Mesh Mesh::cylinder(const CylinderParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        append_frustum(mesh.vertices_, mesh.indices_, params.radius, params.radius, params.height,
                        params.radial_segments, params.capped, params.axis);
        return mesh;
    }

    Mesh Mesh::cone(const ConeParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        append_frustum(mesh.vertices_, mesh.indices_, params.radius, 0.0f, params.height, params.radial_segments,
                        params.capped, params.axis);
        return mesh;
    }

    Mesh Mesh::cube(const CubeParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        const glm::vec3 half(params.size * 0.5f);
        append_box(mesh.vertices_, mesh.indices_, half);
        return mesh;
    }

    Mesh Mesh::rectangular_prism(const RectangularPrismParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        append_box(mesh.vertices_, mesh.indices_, params.extents * 0.5f);
        return mesh;
    }

    Mesh Mesh::tetrahedron(const TetrahedronParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";

        const f32 s = params.size;
        const f32 base_circumradius = s / std::numbers::sqrt3_v<f32>;
        const f32 height = s * std::sqrt(2.0f / 3.0f);
        const f32 base_y = -height * 0.25f;
        const f32 apex_y = height * 0.75f;
        const f32 pi = std::numbers::pi_v<f32>;

        glm::vec3 base[3];
        for (int i = 0; i < 3; ++i) {
            const f32 angle = 2.0f * pi * (static_cast<f32>(i) / 3.0f);
            base[i] = glm::vec3(base_circumradius * std::cos(angle), base_y, base_circumradius * std::sin(angle));
        }
        const glm::vec3 apex(0.0f, apex_y, 0.0f);
        const glm::vec3 center(0.0f);

        append_flat_triangle(mesh.vertices_, mesh.indices_, base[0], base[1], base[2], center);
        append_flat_triangle(mesh.vertices_, mesh.indices_, apex, base[0], base[1], center);
        append_flat_triangle(mesh.vertices_, mesh.indices_, apex, base[1], base[2], center);
        append_flat_triangle(mesh.vertices_, mesh.indices_, apex, base[2], base[0], center);

        return mesh;
    }

    Mesh Mesh::plane(const PlaneParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        const u32 width_segments = params.width_segments < 1 ? 1 : params.width_segments;
        const u32 depth_segments = params.depth_segments < 1 ? 1 : params.depth_segments;
        const glm::vec3 normal = remap_axis(glm::vec3(0.0f, 1.0f, 0.0f), params.axis);

        vector<GeometryVertex> &verts = mesh.vertices_;
        for (u32 row = 0; row <= depth_segments; ++row) {
            const f32 v = static_cast<f32>(row) / static_cast<f32>(depth_segments);
            const f32 z = (v - 0.5f) * params.depth;
            for (u32 col = 0; col <= width_segments; ++col) {
                const f32 u = static_cast<f32>(col) / static_cast<f32>(width_segments);
                const f32 x = (u - 0.5f) * params.width;
                verts.push_back(GeometryVertex{
                    .position = remap_axis(glm::vec3(x, 0.0f, z), params.axis), .normal = normal, .uv = {u, v}});
            }
        }

        vector<u32> &idx = mesh.indices_;
        const u32 row_stride = width_segments + 1;
        for (u32 row = 0; row < depth_segments; ++row) {
            for (u32 col = 0; col < width_segments; ++col) {
                const u32 i0 = row * row_stride + col;
                const u32 i1 = i0 + row_stride;
                const u32 i2 = i1 + 1;
                const u32 i3 = i0 + 1;
                append_smooth_quad(verts, idx, i0, i1, i2, i3);
            }
        }
        return mesh;
    }

    Mesh Mesh::torus(const TorusParams &params, const char *label) {
        Mesh mesh;
        mesh.label_ = label ? label : "";
        const u32 major_segments = params.major_segments < 3 ? 3 : params.major_segments;
        const u32 minor_segments = params.minor_segments < 3 ? 3 : params.minor_segments;
        const f32 pi = std::numbers::pi_v<f32>;

        vector<GeometryVertex> &verts = mesh.vertices_;
        for (u32 major = 0; major <= major_segments; ++major) {
            const f32 u = static_cast<f32>(major) / static_cast<f32>(major_segments);
            const f32 theta = u * 2.0f * pi;
            const glm::vec3 ring_center(params.major_radius * std::cos(theta), 0.0f,
                                         params.major_radius * std::sin(theta));
            const glm::vec3 radial(std::cos(theta), 0.0f, std::sin(theta));
            for (u32 minor = 0; minor <= minor_segments; ++minor) {
                const f32 v = static_cast<f32>(minor) / static_cast<f32>(minor_segments);
                const f32 phi = v * 2.0f * pi;
                const glm::vec3 offset_dir = radial * std::cos(phi) + glm::vec3(0.0f, 1.0f, 0.0f) * std::sin(phi);
                verts.push_back(GeometryVertex{
                    .position = ring_center + offset_dir * params.minor_radius, .normal = offset_dir, .uv = {u, v}});
            }
        }

        vector<u32> &idx = mesh.indices_;
        const u32 row_stride = minor_segments + 1;
        for (u32 major = 0; major < major_segments; ++major) {
            for (u32 minor = 0; minor < minor_segments; ++minor) {
                const u32 i0 = major * row_stride + minor;
                const u32 i1 = i0 + row_stride;
                const u32 i2 = i1 + 1;
                const u32 i3 = i0 + 1;
                append_smooth_quad(verts, idx, i0, i1, i2, i3);
            }
        }
        return mesh;
    }

} // namespace SFT::Renderer
