#pragma once

#include "Asset.hpp"

#include <filesystem>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace SFT::Engine {

    class AssetManager;

    // One glTF scene node's flattened world-space use of a mesh. glTF nodes/scenes are baked to
    // world transforms at import time rather than kept as a live parent/child tree: Ecs/ has no
    // Parent/Children hierarchy component yet (plans/gltf-import.md flags the node/scene mapping as
    // depending on one), so there is nothing to attach a live tree to today. A caller that wants a
    // real scene graph spawns one Ecs entity per instance from this flat list
    // (Engine::WorldTransform{instance.world_transform}, Engine::ModelRenderer{instance.model}).
    struct GltfNodeInstance {
        UString name;
        Asset model{};
        glm::mat4 world_transform{1.0f};
    };

    enum class GltfLightKind : u8 {
        Directional,
        Point,
        Spot,
    };

    // A KHR_lights_punctual node, flattened to world space the same way GltfNodeInstance is.
    // `radiance` is already converted from glTF's photometric units (lux for directional, candela
    // for point/spot) to this engine's radiometric scale via the standard /683 lm/W factor (the
    // same convention Filament and the Khronos glTF sample viewer use); `inner_cone_cos`/
    // `outer_cone_cos` are already converted from glTF's radian cone half-angles. `range` is only
    // meaningful for Point/Spot (glTF has no directional range; 0 means "unspecified", left as this
    // engine's own PointLight/SpotLight default in that case).
    struct GltfLightInstance {
        UString name;
        GltfLightKind kind = GltfLightKind::Point;
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 inner_cone_cos = 0.97f;
        f32 outer_cone_cos = 0.90f;
        glm::mat4 world_transform{1.0f};
    };

    struct GltfImportResult {
        // One Asset per glTF mesh (index-aligned with the source's meshes[]), each holding every
        // primitive of that mesh as one AssetManager model primitive. Multiple GltfNodeInstances may
        // reference the same model when glTF nodes share a mesh index — matching glTF semantics,
        // since materials are assigned per mesh primitive, not per node.
        std::vector<Asset> models;
        std::vector<GltfNodeInstance> instances;
        std::vector<GltfLightInstance> lights;
    };

    // Parses a glTF 2.0 asset (.gltf or .glb; external or embedded buffers/images) with cgltf and
    // uploads it through AssetManager's existing model/texture pipeline.
    //
    // `shader` must already be a loaded Shader asset (AssetManager::load_shader) whose material
    // template exposes the pbr-material-system.md parameter/texture-slot names: base_color_factor
    // (vec4), metallic_factor/roughness_factor/specular_factor/ior (f32), base_color_texture,
    // metallic_roughness_texture (Sampler2D) — e.g. Shaders/gbuffer_geometry.slang. Every material
    // parameter is set explicitly from the glTF value (or the glTF 2.0 spec default when the source
    // material omits it), since MaterialInstance parameters are otherwise zero-initialized.
    //
    // Skinning and morph targets are not parsed: plans/gltf-import.md defers both pending ECS
    // readiness for GPU skinning's compute pre-pass.
    [[nodiscard]] AssetExpected<GltfImportResult> import_gltf(
        AssetManager &assets,
        const std::filesystem::path &source,
        Asset shader);

} // namespace SFT::Engine
