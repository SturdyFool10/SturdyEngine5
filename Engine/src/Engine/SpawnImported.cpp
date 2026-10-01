#include <Engine/SpawnImported.hpp>

#include <Engine/AssetManager.hpp>
#include <Engine/EcsRendering.hpp>

namespace SFT::Engine {

    usize find_clip(const std::vector<std::shared_ptr<const Animation::Clip>> &clips, std::string_view name) noexcept {
        for (usize i = 0; i < clips.size(); ++i) {
            if (clips[i]->name == name) {
                return i;
            }
        }
        return clips.size();
    }

    namespace {

        [[nodiscard]] u32 initial_clip_index(const std::vector<std::shared_ptr<const Animation::Clip>> &clips,
                                             const SpawnImportedOptions &options) {
            if (!options.auto_play || clips.empty()) {
                return static_cast<u32>(clips.size()); // out of range = no clip
            }
            if (options.initial_clip.empty()) {
                return 0;
            }
            return static_cast<u32>(find_clip(clips, options.initial_clip));
        }

    } // namespace

    SpawnedModel spawn_imported(Ecs::World &world, AssetManager &assets, const GltfImportResult &imported,
                                const SpawnImportedOptions &options) {
        SpawnedModel spawned;

        const bool animated_hierarchy = imported.scene_skeleton && !imported.scene_clips.empty();
        if (animated_hierarchy) {
            auto rig = std::make_shared<HierarchyRig>();
            rig->skeleton = imported.scene_skeleton;
            rig->clips = imported.scene_clips;
            rig->base = options.transform;
            rig->clip_index = initial_clip_index(rig->clips, options);
            rig->speed = options.speed;
            rig->loop = options.loop;
            rig->playing = options.auto_play;
            Animation::local_to_model(*rig->skeleton, Animation::rest_pose_of(*rig->skeleton), rig->model);
            spawned.hierarchy = std::move(rig);
        }

        for (const GltfNodeInstance &instance : imported.instances) {
            if (!instance.model) {
                continue;
            }
            const bool skinned = instance.skin >= 0 && static_cast<usize>(instance.skin) < imported.skins.size();
            if (skinned || assets.is_skinnable(instance.model)) {
                auto clone = assets.create_skinned_instance(instance.model, instance.name);
                if (!clone) {
                    continue;
                }
                spawned.owned_models.push_back(*clone);
                SkeletonAnimator animator{
                    .model = *clone,
                    .speed = options.speed,
                    .loop = options.loop,
                    .playing = options.auto_play,
                    .morph_target = instance.name.cpp_string(),
                };
                if (skinned) {
                    const GltfSkin &skin = imported.skins[static_cast<usize>(instance.skin)];
                    animator.skeleton = skin.skeleton;
                    animator.clips = skin.clips;
                } else {
                    animator.clips = imported.scene_clips; // blend-shape tracks live in the scene clips
                }
                animator.clip_index = initial_clip_index(animator.clips, options);
                const Ecs::Entity entity = world.spawn(
                    WorldTransform{.value = options.transform * instance.world_transform},
                    ModelRenderer{.model = *clone}, std::move(animator));
                spawned.entities.push_back(entity);
                spawned.animators.push_back(entity);
                continue;
            }

            if (animated_hierarchy && instance.scene_joint >= 0) {
                const u32 joint = static_cast<u32>(instance.scene_joint);
                const glm::mat4 initial = options.transform * spawned.hierarchy->model[joint];
                spawned.entities.push_back(world.spawn(
                    WorldTransform{.value = initial}, ModelRenderer{.model = instance.model},
                    HierarchyJoint{.rig = spawned.hierarchy, .joint = joint}));
            } else {
                spawned.entities.push_back(world.spawn(
                    WorldTransform{.value = options.transform * instance.world_transform},
                    ModelRenderer{.model = instance.model}));
            }
        }
        return spawned;
    }

} // namespace SFT::Engine
