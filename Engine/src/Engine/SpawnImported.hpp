#pragma once

#include <Ecs/World.hpp>
#include <Engine/EcsAnimation.hpp>
#include <Engine/GltfImport.hpp>

#include <memory>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

namespace SFT::Engine {

    class AssetManager;

    struct SpawnImportedOptions {
        /// Placed on top of whatever the file itself says.
        glm::mat4 transform{1.0f};
        /// Start playing an animation as soon as the entities exist.
        bool auto_play = true;
        /// Name of the clip to play first; empty = the file's first. A name that matches nothing plays nothing.
        std::string_view initial_clip;
        bool loop = true;
        f32 speed = 1.0f;
    };

    struct SpawnedModel {
        /// Every entity created (renderers, lights are not included).
        std::vector<Ecs::Entity> entities;
        /// The per-instance deformable model assets (skinned characters, blend-shape meshes) this call created;
        /// unload them when the entities are gone.
        std::vector<Asset> owned_models;
        /// Entities carrying a `SkeletonAnimator` (characters and blend-shape meshes).
        std::vector<Ecs::Entity> animators;
        /// Shared playback state of the node-hierarchy animation (null when the file has none). Change
        /// `clip_index`, `speed`, `playing`, `loop` or `base` here at any time.
        std::shared_ptr<HierarchyRig> hierarchy;
    };

    /// Creates the entities for an imported glTF scene and wires up every kind of animation it carries:
    /// skinned characters get a private deformable copy plus a `SkeletonAnimator`, blend-shape-only meshes get the
    /// same without a skeleton, and plain nodes follow a shared `HierarchyRig` (doors, props, rigid parts).
    [[nodiscard]] SpawnedModel spawn_imported(Ecs::World &world, AssetManager &assets, const GltfImportResult &imported,
                                              const SpawnImportedOptions &options = {});

    /// Index of the clip called `name` in `clips`, or `clips.size()` when absent.
    [[nodiscard]] usize find_clip(const std::vector<std::shared_ptr<const Animation::Clip>> &clips,
                                  std::string_view name) noexcept;

} // namespace SFT::Engine
