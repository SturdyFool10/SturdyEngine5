#include <Engine/EcsAnimation.hpp>

#include <Engine/AssetManager.hpp>

#include <algorithm>

namespace SFT::Engine {

    namespace {

        usize clip_by_name(const std::vector<std::shared_ptr<const Animation::Clip>> &clips, std::string_view name) {
            for (usize i = 0; i < clips.size(); ++i) {
                if (clips[i]->name.cpp_string_view() == name) return i;
            }
            return clips.size();
        }

    } // namespace

    bool animation_play(Ecs::World &world, Ecs::Entity entity, std::string_view clip, bool loop, f32 speed) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            const usize index = clip_by_name(animator->clips, clip);
            if (index >= animator->clips.size()) return false;
            animator->clip_index = static_cast<u32>(index);
            animator->time = 0.0f;
            animator->loop = loop;
            animator->speed = speed;
            animator->playing = true;
            return true;
        }
        if (auto joint = world.get_component<HierarchyJoint>(entity); joint && joint->rig) {
            std::lock_guard lock{joint->rig->mutex};
            const usize index = clip_by_name(joint->rig->clips, clip);
            if (index >= joint->rig->clips.size()) return false;
            joint->rig->clip_index = static_cast<u32>(index);
            joint->rig->time = 0.0f;
            joint->rig->loop = loop;
            joint->rig->speed = speed;
            joint->rig->playing = true;
            return true;
        }
        return false;
    }

    bool animation_set_speed(Ecs::World &world, Ecs::Entity entity, f32 speed) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            animator->speed = speed;
            return true;
        }
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) {
            player->speed = speed;
            return true;
        }
        if (auto joint = world.get_component<HierarchyJoint>(entity); joint && joint->rig) {
            joint->rig->speed = speed;
            return true;
        }
        return false;
    }

    bool animation_set_playing(Ecs::World &world, Ecs::Entity entity, bool playing) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            animator->playing = playing;
            return true;
        }
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) {
            player->playing = playing;
            return true;
        }
        if (auto joint = world.get_component<HierarchyJoint>(entity); joint && joint->rig) {
            joint->rig->playing = playing;
            return true;
        }
        return false;
    }

    std::vector<std::string> animation_clip_names(Ecs::World &world, Ecs::Entity entity) {
        std::vector<std::string> names;
        const auto collect = [&names](const std::vector<std::shared_ptr<const Animation::Clip>> &clips) {
            for (const auto &clip : clips) names.push_back(clip->name.cpp_string());
        };
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            collect(animator->clips);
        } else if (auto joint = world.get_component<HierarchyJoint>(entity); joint && joint->rig) {
            collect(joint->rig->clips);
        }
        return names;
    }

    bool animation_add_clips(Ecs::World &world, Ecs::Entity entity,
                             const std::vector<std::shared_ptr<const Animation::Clip>> &clips) {
        auto animator = world.get_component<SkeletonAnimator>(entity);
        if (!animator) return false;
        animator->clips.insert(animator->clips.end(), clips.begin(), clips.end());
        return true;
    }

    bool animation_attach_graph(Ecs::World &world, Ecs::Entity entity, std::shared_ptr<const Animation::GraphDef> graph) {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        Asset model{};
        std::string morph_target;
        f32 speed = 1.0f;
        bool playing = true;
        {
            auto animator = world.get_component<SkeletonAnimator>(entity);
            if (!animator || !animator->skeleton || !graph) return false;
            skeleton = animator->skeleton;
            model = animator->model;
            morph_target = animator->morph_target;
            speed = animator->speed;
            playing = animator->playing;
        }
        AnimationGraphPlayer player = make_graph_player(model, std::move(skeleton), std::move(graph));
        player.morph_target = std::move(morph_target);
        player.speed = speed;
        player.playing = playing;
        world.remove_component<SkeletonAnimator>(entity);
        world.add_component(entity, std::move(player));
        return true;
    }

    bool animation_set_float(Ecs::World &world, Ecs::Entity entity, std::string_view name, f32 value) {
        auto player = world.get_component<AnimationGraphPlayer>(entity);
        if (!player || !player->graph) return false;
        player->graph->set_float(name, value);
        return true;
    }

    bool animation_set_bool(Ecs::World &world, Ecs::Entity entity, std::string_view name, bool value) {
        auto player = world.get_component<AnimationGraphPlayer>(entity);
        if (!player || !player->graph) return false;
        player->graph->set_bool(name, value);
        return true;
    }

    bool animation_set_trigger(Ecs::World &world, Ecs::Entity entity, std::string_view name) {
        auto player = world.get_component<AnimationGraphPlayer>(entity);
        if (!player || !player->graph) return false;
        player->graph->set_trigger(name);
        return true;
    }

    std::string animation_graph_state(Ecs::World &world, Ecs::Entity entity) {
        auto player = world.get_component<AnimationGraphPlayer>(entity);
        if (!player || !player->graph) return {};
        return std::string{player->graph->current_state_name()};
    }

    u32 animation_joint_index(Ecs::World &world, Ecs::Entity entity, std::string_view name) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity); animator && animator->skeleton) {
            return animator->skeleton->find_joint(name);
        }
        if (auto player = world.get_component<AnimationGraphPlayer>(entity); player && player->skeleton) {
            return player->skeleton->find_joint(name);
        }
        return Animation::no_joint;
    }

    int animation_add_two_bone_ik(Ecs::World &world, Ecs::Entity entity, const Animation::TwoBoneIk &ik) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            animator->two_bone_iks.push_back(ik);
            return static_cast<int>(animator->two_bone_iks.size() - 1);
        }
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) {
            player->two_bone_iks.push_back(ik);
            return static_cast<int>(player->two_bone_iks.size() - 1);
        }
        return -1;
    }

    int animation_add_look_at(Ecs::World &world, Ecs::Entity entity, const Animation::LookAtIk &ik) {
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            animator->look_ats.push_back(ik);
            return static_cast<int>(animator->look_ats.size() - 1);
        }
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) {
            player->look_ats.push_back(ik);
            return static_cast<int>(player->look_ats.size() - 1);
        }
        return -1;
    }

    bool animation_update_two_bone_target(Ecs::World &world, Ecs::Entity entity, usize index, glm::vec3 target, f32 weight) {
        const auto apply = [&](auto &list) {
            if (index >= list.size()) return false;
            list[index].target = target;
            list[index].weight = weight;
            return true;
        };
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) return apply(animator->two_bone_iks);
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) return apply(player->two_bone_iks);
        return false;
    }

    bool animation_update_look_at_target(Ecs::World &world, Ecs::Entity entity, usize index, glm::vec3 target, f32 weight) {
        const auto apply = [&](auto &list) {
            if (index >= list.size()) return false;
            list[index].target = target;
            list[index].weight = weight;
            return true;
        };
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) return apply(animator->look_ats);
        if (auto player = world.get_component<AnimationGraphPlayer>(entity)) return apply(player->look_ats);
        return false;
    }

    void HierarchyRig::evaluate(u64 tick, f32 delta_seconds) {
        std::lock_guard lock{mutex};
        if (evaluated_tick == tick || !skeleton) {
            return;
        }
        evaluated_tick = tick;
        const Animation::Clip *clip = clip_index < clips.size() ? clips[clip_index].get() : nullptr;
        if (playing && clip != nullptr) {
            time += delta_seconds * speed;
            time = loop && clip->duration > 0.0f ? Animation::wrap_time(*clip, time, true)
                                                  : std::clamp(time, 0.0f, clip->duration);
        }
        if (clip != nullptr) {
            Animation::sample_clip(*skeleton, *clip, time, false, pose);
        } else {
            pose = Animation::rest_pose_of(*skeleton);
        }
        Animation::local_to_model(*skeleton, pose, model);
    }

    void apply_iks(const Animation::Skeleton &skeleton, Animation::Pose &pose,
                   const std::vector<Animation::TwoBoneIk> &two_bone, const std::vector<Animation::LookAtIk> &look_at,
                   const std::vector<Animation::ChainIk> &chains) {
        for (const Animation::ChainIk &ik : chains) {
            if (ik.weight > 0.0f) (void)Animation::solve_chain(skeleton, pose, ik);
        }
        for (const Animation::TwoBoneIk &ik : two_bone) {
            if (ik.weight > 0.0f) (void)Animation::solve_two_bone(skeleton, pose, ik);
        }
        for (const Animation::LookAtIk &ik : look_at) {
            if (ik.weight > 0.0f) Animation::solve_look_at(skeleton, pose, ik);
        }
    }

    AnimationGraphPlayer make_graph_player(Asset skinned_model, std::shared_ptr<const Animation::Skeleton> skeleton,
                                           std::shared_ptr<const Animation::GraphDef> graph) {
        AnimationGraphPlayer player;
        player.model = skinned_model;
        player.graph = std::make_shared<Animation::GraphInstance>(std::move(graph), skeleton);
        player.skeleton = std::move(skeleton);
        return player;
    }

    Animation::RootMotionDelta tick_graph_player(AssetManager &assets, AnimationGraphPlayer &player, f32 delta_seconds) {
        Animation::RootMotionDelta root;
        player.events.clear();
        if (!player.graph || !player.skeleton) {
            return root;
        }
        player.graph->update(player.playing ? delta_seconds * player.speed : 0.0f, player.pose);
        player.events = player.graph->events();
        root = player.graph->root_motion();
        apply_iks(*player.skeleton, player.pose, player.two_bone_iks, player.look_ats, player.chain_iks);
        Animation::skin_matrices(*player.skeleton, player.pose, player.matrices);
        if (player.model) {
            // Blend shapes ride along on a graph's first clip that has them (mixing weights across clips is not defined).
            (void)assets.set_skinned_pose(player.model, player.matrices, player.morph_weights);
        }
        return root;
    }

    void tick_skeleton_animator(AssetManager &assets, SkeletonAnimator &animator, f32 delta_seconds) {
        if (!animator.model) {
            return;
        }
        const Animation::Clip *clip =
            animator.clip_index < animator.clips.size() ? animator.clips[animator.clip_index].get() : nullptr;
        if (animator.playing && clip != nullptr) {
            animator.time += delta_seconds * animator.speed;
            if (animator.loop && clip->duration > 0.0f) {
                animator.time = Animation::wrap_time(*clip, animator.time, true);
            } else {
                animator.time = std::clamp(animator.time, 0.0f, clip->duration);
            }
        }
        if (animator.skeleton) {
            if (clip != nullptr) {
                Animation::sample_clip(*animator.skeleton, *clip, animator.time, false, animator.pose);
            } else {
                animator.pose = Animation::rest_pose_of(*animator.skeleton);
            }
            apply_iks(*animator.skeleton, animator.pose, animator.two_bone_iks, animator.look_ats, animator.chain_iks);
            Animation::skin_matrices(*animator.skeleton, animator.pose, animator.matrices);
        } else {
            animator.matrices.assign(1, glm::mat4{1.0f});
        }
        animator.morph_weights.clear();
        if (clip != nullptr) {
            if (const Animation::MorphTrack *track = clip->find_morph_track(animator.morph_target)) {
                Animation::sample_weights(*track, Animation::wrap_time(*clip, animator.time, false), animator.morph_weights);
            }
        }
        (void)assets.set_skinned_pose(animator.model, animator.matrices, animator.morph_weights);
    }

} // namespace SFT::Engine
