#pragma once

#include <Engine/FbxImport.hpp>
#include <Engine/GltfImport.hpp>

#include <Animation/Retarget.hpp>

#include <Ecs/World.hpp>
#include <Engine/EcsAnimation.hpp>

#include <Animation/GraphJson.hpp>
#include <span>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace SFT::Engine {

    /// What every importer returns: placed model instances, skins, lights and the clips that animate them.
    using ImportedScene = GltfImportResult;

    /// Formats `import_model`/`import_animations` understand, for UI file filters and error messages.
    [[nodiscard]] std::vector<std::string_view> supported_model_extensions();
    [[nodiscard]] std::vector<std::string_view> supported_animation_extensions();

    /// Loads a model file, choosing the importer from the extension: `.gltf`/`.glb` (glTF 2.0: Blender, Blockbench,
    /// Maya/3ds Max/Houdini/Unity/Unreal/Godot exports, Sketchfab), `.fbx` and `.obj` (everything else: Maya, 3ds Max,
    /// Cinema 4D, Mixamo, marketplace assets). Animation-only formats (`.bvh`) are rejected here; use
    /// `import_animations`.
    [[nodiscard]] AssetExpected<ImportedScene> import_model(AssetManager &assets, const std::filesystem::path &source,
                                                            Asset shader);

    /// One skeleton and the clips authored for it.
    struct ImportedAnimations {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> clips;
    };

    /// Loads only the animation from a file: `.bvh` mocap, or any `.gltf`/`.glb`/`.fbx` (meshes are ignored, so
    /// Mixamo "without skin" downloads and animation-only exports work). Pair with `adapt_clips` to play the result
    /// on a different character.
    [[nodiscard]] AssetExpected<ImportedAnimations> import_animations(AssetManager &assets,
                                                                      const std::filesystem::path &source);

    /// Moves imported clips onto `target`: by bone name when the rigs share a layout, through the humanoid mapping
    /// when they only share a body plan (see `Animation::adapt_clip`).
    [[nodiscard]] std::vector<std::shared_ptr<const Animation::Clip>> adapt_clips(
        const ImportedAnimations &animations, const Animation::Skeleton &target,
        const Animation::RetargetOptions &options = {});

    /// Parses an animation graph document (see `Animation::load_graph_json` for the schema) against the skeleton and
    /// clips a character uses.
    [[nodiscard]] AssetExpected<std::shared_ptr<const Animation::GraphDef>> parse_animation_graph(
        std::string_view json, const Animation::Skeleton &skeleton,
        std::span<const std::shared_ptr<const Animation::Clip>> clips);

    /// Reads a graph document from a file.
    [[nodiscard]] AssetExpected<std::shared_ptr<const Animation::GraphDef>> load_animation_graph(
        const std::filesystem::path &source, const Animation::Skeleton &skeleton,
        std::span<const std::shared_ptr<const Animation::Clip>> clips);

    /// Retargets imported clips onto the entity's skeleton and adds them to its `SkeletonAnimator`, so
    /// `animation_play` can play them by name. Returns how many clips were added.
    usize animation_adopt_clips(Ecs::World &world, Ecs::Entity entity, const ImportedAnimations &animations,
                                const Animation::RetargetOptions &options = {});

    /// Builds a graph from `json` using the entity's own skeleton and clips, then drives it with that graph.
    [[nodiscard]] AssetExpected<bool> animation_attach_graph_json(Ecs::World &world, Ecs::Entity entity, std::string_view json);

} // namespace SFT::Engine
