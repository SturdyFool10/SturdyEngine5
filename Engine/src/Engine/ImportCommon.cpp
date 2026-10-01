#include <Engine/ImportCommon.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace SFT::Engine::Detail {

    AssetResult apply_material_values(AssetManager &assets, Asset model, usize primitive,
                                      const ImportMaterialValues &v) {
        AssetResult set = assets.set_model_vec4(model, primitive, "base_color_factor", v.base_color_factor);
        const auto float_param = [&](const char *name, f32 value) {
            if (set) set = assets.set_model_float(model, primitive, name, value);
        };
        float_param("metallic_factor", v.metallic_factor);
        float_param("roughness_factor", v.roughness_factor);
        float_param("specular_factor", v.specular_factor);
        float_param("ior", v.ior);
        float_param("transmission_factor", v.transmission_factor);
        float_param("dispersion_cauchy_b", v.dispersion_cauchy_b);
        float_param("absorption_coefficient", v.absorption_coefficient);
        float_param("alpha_cutoff", v.alpha_cutoff);
        float_param("occlusion_strength", v.occlusion_strength);
        if (set) set = assets.set_model_vec4(model, primitive, "emissive_factor", v.emissive_factor);
        float_param("emissive_strength", v.emissive_strength);
        float_param("metallic_roughness_channels_rg", v.metallic_roughness_channels_rg);
        return set;
    }

    void generate_normals(std::vector<Renderer::GeometryVertex> &vertices, std::span<const u32> indices) {
        std::vector<glm::vec3> accumulated(vertices.size(), glm::vec3{0.0f});
        for (usize i = 0; i + 2 < indices.size(); i += 3) {
            const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const glm::vec3 face_normal = glm::cross(vertices[b].position - vertices[a].position,
                                                     vertices[c].position - vertices[a].position);
            accumulated[a] += face_normal;
            accumulated[b] += face_normal;
            accumulated[c] += face_normal;
        }
        for (usize v = 0; v < vertices.size(); ++v) {
            const f32 length = glm::length(accumulated[v]);
            vertices[v].normal = length > 1e-8f ? accumulated[v] / length : glm::vec3{0.0f, 1.0f, 0.0f};
        }
    }

    void generate_tangents(std::vector<Renderer::GeometryVertex> &vertices, std::span<const u32> indices) {
        std::vector<glm::vec3> tan_accum(vertices.size(), glm::vec3{0.0f});
        std::vector<glm::vec3> bitan_accum(vertices.size(), glm::vec3{0.0f});
        for (usize i = 0; i + 2 < indices.size(); i += 3) {
            const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const glm::vec3 edge1 = vertices[b].position - vertices[a].position;
            const glm::vec3 edge2 = vertices[c].position - vertices[a].position;
            const glm::vec2 delta_uv1 = vertices[b].uv - vertices[a].uv;
            const glm::vec2 delta_uv2 = vertices[c].uv - vertices[a].uv;
            const f32 determinant = delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y;
            if (std::abs(determinant) < 1e-12f) {
                continue;
            }
            const f32 inv_determinant = 1.0f / determinant;
            const glm::vec3 tangent = inv_determinant * (delta_uv2.y * edge1 - delta_uv1.y * edge2);
            const glm::vec3 bitangent = inv_determinant * (delta_uv1.x * edge2 - delta_uv2.x * edge1);
            tan_accum[a] += tangent;
            tan_accum[b] += tangent;
            tan_accum[c] += tangent;
            bitan_accum[a] += bitangent;
            bitan_accum[b] += bitangent;
            bitan_accum[c] += bitangent;
        }
        for (usize v = 0; v < vertices.size(); ++v) {
            const glm::vec3 &normal = vertices[v].normal;
            glm::vec3 tangent = tan_accum[v] - normal * glm::dot(normal, tan_accum[v]);
            const f32 length = glm::length(tangent);
            if (length > 1e-8f) {
                tangent /= length;
            } else {
                const glm::vec3 reference = std::abs(normal.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f}
                                                                     : glm::vec3{0.0f, 1.0f, 0.0f};
                tangent = glm::normalize(glm::cross(reference, normal));
            }
            const f32 handedness = glm::dot(glm::cross(normal, tangent), bitan_accum[v]) < 0.0f ? -1.0f : 1.0f;
            vertices[v].tangent = glm::vec4{tangent, handedness};
        }
    }

    Animation::JointTransform decompose_transform(const glm::mat4 &m) {
        Animation::JointTransform t;
        t.translation = glm::vec3(m[3]);
        t.scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
        const glm::vec3 sx = t.scale.x > 1e-12f ? glm::vec3(m[0]) / t.scale.x : glm::vec3(1, 0, 0);
        const glm::vec3 sy = t.scale.y > 1e-12f ? glm::vec3(m[1]) / t.scale.y : glm::vec3(0, 1, 0);
        const glm::vec3 sz = t.scale.z > 1e-12f ? glm::vec3(m[2]) / t.scale.z : glm::vec3(0, 0, 1);
        t.rotation = glm::normalize(glm::quat_cast(glm::mat3(sx, sy, sz)));
        return t;
    }

} // namespace SFT::Engine::Detail
