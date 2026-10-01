#pragma once

#include <Ecs/Entity.hpp>
#include <Ecs/Event.hpp>
#include <Ecs/Resource.hpp>
#include <Ecs/World.hpp>
#include <Engine/Asset.hpp>

#include <Animation/Clip.hpp>
#include <Animation/Graph.hpp>
#include <Animation/Ik.hpp>
#include <Animation/Skeleton.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Engine {

    class AssetManager;

    /// Plays one clip of a skeleton (optional for blend-shape-only models) onto a skinned model (`AssetManager::create_skinned_instance`). Attach it
    /// next to the entity's `ModelRenderer` that references `model`.
    struct SkeletonAnimator {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> clips;
        Asset model{};
        u32 clip_index = 0;
        f32 time = 0.0f;
        f32 speed = 1.0f;
        bool loop = true;
        bool playing = true;
        /// Name of the node whose blend-shape track drives the model's morph targets; empty = the clip's first one.
        std::string morph_target;
        /// Solved after the clip, every tick, in model space (set `target` each frame from gameplay). Entries with
        /// `weight` 0 are skipped, so leave them in place and fade the weight instead of adding/removing.
        std::vector<Animation::TwoBoneIk> two_bone_iks;
        std::vector<Animation::LookAtIk> look_ats;
        std::vector<Animation::ChainIk> chain_iks;

        // Scratch reused every tick.
        Animation::Pose pose;
        std::vector<glm::mat4> matrices;
        std::vector<f32> morph_weights;
    };

    /// Drives a skinned model from an animation graph (parameters -> state machine / blend trees / layers).
    /// Set parameters through `graph` (from a gameplay system, a script or the C ABI); read `events` for the
    /// markers the last tick passed. With `apply_root_motion` the extracted root movement is applied to the
    /// entity's `WorldTransform` (in the entity's own space), so locomotion clips move the character.
    struct AnimationGraphPlayer {
        std::shared_ptr<Animation::GraphInstance> graph;
        std::shared_ptr<const Animation::Skeleton> skeleton;
        Asset model{};
        bool apply_root_motion = true;
        f32 speed = 1.0f;
        bool playing = true;
        std::string morph_target;
        /// Solved after the graph, every tick, in model space (see `SkeletonAnimator`).
        std::vector<Animation::TwoBoneIk> two_bone_iks;
        std::vector<Animation::LookAtIk> look_ats;
        std::vector<Animation::ChainIk> chain_iks;

        std::vector<Animation::FiredEvent> events;
        Animation::Pose pose;
        std::vector<glm::mat4> matrices;
        std::vector<f32> morph_weights;
    };

    /// Sent once per marker an `AnimationGraphPlayer` passes (footsteps, spawn points, ...).
    struct AnimationEvent {
        Ecs::Entity entity{};
        std::string name;
        f32 clip_time = 0.0f;
    };

    /// Applies a component's IK lists to `pose` (feet on the ground, hands on props, head tracking).
    void apply_iks(const Animation::Skeleton &skeleton, Animation::Pose &pose,
                   const std::vector<Animation::TwoBoneIk> &two_bone, const std::vector<Animation::LookAtIk> &look_at,
                   const std::vector<Animation::ChainIk> &chains);

    /// Builds a player for a skinned model (`AssetManager::create_skinned_instance`) from a graph. Instance the
    /// graph yourself (`std::make_shared<GraphInstance>`) to drive several characters from one `GraphDef`.
    [[nodiscard]] AnimationGraphPlayer make_graph_player(Asset skinned_model,
                                                         std::shared_ptr<const Animation::Skeleton> skeleton,
                                                         std::shared_ptr<const Animation::GraphDef> graph);

    /// Advances the graph, poses the model, and returns the root motion of this tick.
    [[nodiscard]] Animation::RootMotionDelta tick_graph_player(AssetManager &assets, AnimationGraphPlayer &player,
                                                               f32 delta_seconds);

    // ---- Controlling animation on entities, from anywhere (gameplay code, scripts, the C ABI) -------------------
    //
    // These look at whichever animation component the entity carries (`SkeletonAnimator`, `AnimationGraphPlayer`,
    // or a `HierarchyJoint` sharing a rig) and return false when there is nothing to control. They use the world's
    // direct component access, so call them from game logic callbacks, not from inside a scheduled system.

    /// Plays the clip called `clip` from its start. On a graph player this is not meaningful (drive its
    /// parameters instead) and returns false.
    bool animation_play(Ecs::World &world, Ecs::Entity entity, std::string_view clip, bool loop = true,
                        f32 speed = 1.0f);
    bool animation_set_speed(Ecs::World &world, Ecs::Entity entity, f32 speed);
    bool animation_set_playing(Ecs::World &world, Ecs::Entity entity, bool playing);
    /// Names of the clips the entity can play (empty for a graph player, whose clips live in its graph).
    [[nodiscard]] std::vector<std::string> animation_clip_names(Ecs::World &world, Ecs::Entity entity);
    /// Appends clips (already adapted to the entity's skeleton, see `adapt_clips`) to a `SkeletonAnimator`.
    bool animation_add_clips(Ecs::World &world, Ecs::Entity entity,
                             const std::vector<std::shared_ptr<const Animation::Clip>> &clips);
    /// Swaps a `SkeletonAnimator` for an `AnimationGraphPlayer` driven by `graph`, keeping its skeleton, model and
    /// blend-shape target. The graph must have been built for that skeleton.
    bool animation_attach_graph(Ecs::World &world, Ecs::Entity entity, std::shared_ptr<const Animation::GraphDef> graph);
    /// Graph parameters (no effect when the entity has no graph player).
    bool animation_set_float(Ecs::World &world, Ecs::Entity entity, std::string_view name, f32 value);
    bool animation_set_bool(Ecs::World &world, Ecs::Entity entity, std::string_view name, bool value);
    bool animation_set_trigger(Ecs::World &world, Ecs::Entity entity, std::string_view name);
    /// Current state-machine state of the entity's graph.
    [[nodiscard]] std::string animation_graph_state(Ecs::World &world, Ecs::Entity entity);
    /// Index of the joint called `name` in the entity's skeleton (`Animation::no_joint` when there is none).
    [[nodiscard]] u32 animation_joint_index(Ecs::World &world, Ecs::Entity entity, std::string_view name);
    /// Adds an IK constraint to an animator/graph player (returns its index in the matching list, or -1).
    int animation_add_two_bone_ik(Ecs::World &world, Ecs::Entity entity, const Animation::TwoBoneIk &ik);
    int animation_add_look_at(Ecs::World &world, Ecs::Entity entity, const Animation::LookAtIk &ik);
    bool animation_update_two_bone_target(Ecs::World &world, Ecs::Entity entity, usize index, glm::vec3 target, f32 weight);
    bool animation_update_look_at_target(Ecs::World &world, Ecs::Entity entity, usize index, glm::vec3 target, f32 weight);

    /// Shared playback state of an animated node hierarchy (doors, props, rigid parts). Every entity that follows a
    /// joint holds a `HierarchyJoint` pointing at one rig; the first of them to run each frame advances and
    /// evaluates it. Control playback by editing the rig through any `HierarchyJoint`/the spawn result.
    struct HierarchyRig {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> clips;
        glm::mat4 base{1.0f};
        u32 clip_index = 0;
        f32 time = 0.0f;
        f32 speed = 1.0f;
        bool loop = true;
        bool playing = true;

        // Evaluation state, guarded by `mutex`.
        std::mutex mutex;
        u64 evaluated_tick = ~0ull;
        Animation::Pose pose;
        std::vector<glm::mat4> model;

        /// Advances by `delta_seconds` once per `tick` and recomputes the joint matrices.
        void evaluate(u64 tick, f32 delta_seconds);
    };

    /// Makes the entity's `WorldTransform` follow one joint of a `HierarchyRig`.
    struct HierarchyJoint {
        std::shared_ptr<HierarchyRig> rig;
        u32 joint = 0;
    };

    /// Advances time, samples the active clip (or the rest pose when there is none) and poses the model.
    void tick_skeleton_animator(AssetManager &assets, SkeletonAnimator &animator, f32 delta_seconds);

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::HierarchyJoint, "sturdy.engine.hierarchy_joint");
SFT_ECS_COMPONENT(SFT::Engine::AnimationGraphPlayer, "sturdy.engine.animation_graph_player");
SFT_ECS_EVENT(SFT::Engine::AnimationEvent, "sturdy.engine.animation_event");
SFT_ECS_COMPONENT(SFT::Engine::SkeletonAnimator, "sturdy.engine.skeleton_animator");
