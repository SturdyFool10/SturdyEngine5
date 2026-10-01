#pragma once

#include <Engine/Asset.hpp>

#include <Animation/Clip.hpp>
#include <Animation/Morph.hpp>
#include <Animation/Skeleton.hpp>

#include <filesystem>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace SFT::Engine {

    class AssetManager;


    struct GltfNodeInstance {
        UString name;
        Asset model{};
        glm::mat4 world_transform{1.0f};
        /// Index into `GltfImportResult::skins`, or -1 for a rigid mesh. A skinned instance's
        /// `world_transform` is identity: the joints already carry the node placement. Pose a copy made with
        /// `AssetManager::create_skinned_instance(model)` using the skin's skeleton and clips.
        i32 skin = -1;
        /// Joint of this node in `GltfImportResult::scene_skeleton` (for animating the node's transform), or -1.
        i32 scene_joint = -1;
    };

    /// A glTF skin: joint hierarchy (parent-first, including non-joint ancestors) plus every animation that
    /// targets it, with channels already addressed by skeleton joint index.
    struct GltfSkin {
        UString name;
        std::shared_ptr<const Animation::Skeleton> skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> clips;
    };

    enum class GltfLightKind : u8 {
        Directional,
        Point,
        Spot,
    };


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


        std::vector<Asset> models;
        std::vector<GltfNodeInstance> instances;
        std::vector<GltfLightInstance> lights;
        std::vector<GltfSkin> skins;
        /// Every node of the file as one parent-first hierarchy (identity inverse binds) and every animation
        /// addressed to it, for animating plain objects (doors, props, rigid parts). Null when the file has no nodes.
        std::shared_ptr<const Animation::Skeleton> scene_skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> scene_clips;
    };


    /// With `animations_only` no meshes, materials or textures are created (so `assets` is untouched and
    /// `shader` may be empty): only skeletons and clips come back.
    /// Imports gltf using the supplied arguments and current state.
    ///
    /// @param assets `assets` value used by the operation.
    /// @param source Source value or resource.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] AssetExpected<GltfImportResult> import_gltf(
        AssetManager &assets,
        const std::filesystem::path &source,
        Asset shader,
        bool animations_only = false);

} // namespace SFT::Engine
