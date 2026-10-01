#pragma once

// Pieces every model importer (glTF, FBX, ...) needs: the material parameter block the engine's PBR shader
// expects, geometry fix-ups for formats that omit normals/tangents, and the helpers that turn a node
// hierarchy into an `Animation::Skeleton`.

#include <Engine/AssetManager.hpp>

#include <Animation/Skeleton.hpp>
#include <Renderer/Geometry.hpp>

#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Engine::Detail {

    /// Every scalar/vector parameter of the PBR material template (names match the shader).
    struct ImportMaterialValues {
        glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
        f32 metallic_factor = 1.0f;
        f32 roughness_factor = 1.0f;
        f32 specular_factor = 1.0f;
        f32 ior = 1.5f;
        f32 transmission_factor = 0.0f;
        f32 dispersion_cauchy_b = 0.0042f;
        f32 absorption_coefficient = 0.0f;
        f32 alpha_cutoff = 0.0f;
        f32 occlusion_strength = 1.0f;
        glm::vec4 emissive_factor{0.0f};
        f32 emissive_strength = 1.0f;
        f32 metallic_roughness_channels_rg = 0.0f;
    };

    /// Writes all of `values` into one primitive of a model.
    [[nodiscard]] AssetResult apply_material_values(AssetManager &assets, Asset model, usize primitive,
                                                    const ImportMaterialValues &values);

    /// Smooth per-vertex normals from the triangles (for meshes that have none).
    void generate_normals(std::vector<Renderer::GeometryVertex> &vertices, std::span<const u32> indices);

    /// Per-vertex tangents (xyz + handedness in w) from positions, normals and UVs.
    void generate_tangents(std::vector<Renderer::GeometryVertex> &vertices, std::span<const u32> indices);

    /// Decomposes a column-major affine matrix into a joint transform (translation, rotation, scale).
    [[nodiscard]] Animation::JointTransform decompose_transform(const glm::mat4 &matrix);

} // namespace SFT::Engine::Detail
