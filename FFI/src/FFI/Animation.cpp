/// C ABI for controlling animation on entities: playback, animation graphs, retargeting clips from separate files,
/// and IK constraints.
///
/// Everything here forwards to `Engine/EcsAnimation.hpp` and `Engine/ModelImport.hpp`; the work lives in the C++
/// stack. This file only checks handles and arguments and translates types.

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <glm/vec3.hpp>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u64;

    std::mutex g_set_mutex;
    std::map<u64, std::unique_ptr<SFT::Engine::ImportedAnimations>> g_sets;

    [[nodiscard]] SFT::Ecs::Entity to_engine_entity(SturdyEntity entity) noexcept {
        return SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation};
    }

    [[nodiscard]] SturdyResult resolve_set(SturdyAnimationSet set, SFT::Engine::ImportedAnimations **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(set.token, HandleKind::AnimationSet, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<SFT::Engine::ImportedAnimations *>(pointer);
        return STURDY_OK;
    }

    /// Runs `body(world)` for a live engine and entity, mapping a false return to "nothing to animate".
    template <class Body>
    [[nodiscard]] SturdyResult with_entity(SturdyEngine engine, SturdyEntity entity, const char *what, Body &&body) {
        return guarded([&]() -> SturdyResult {
            SFT::Engine::Engine *resolved_engine = nullptr;
            const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
            if (resolved != STURDY_OK) {
                return resolved;
            }
            SFT::Ecs::World &world = resolved_engine->ecs_world();
            const SFT::Ecs::Entity target = to_engine_entity(entity);
            if (!world.is_alive(target)) {
                return set_error(STURDY_ERROR_ENTITY_NOT_ALIVE, "the entity is not alive");
            }
            if (!body(world, target)) {
                return set_error(STURDY_ERROR_NOT_AVAILABLE, what);
            }
            return STURDY_OK;
        });
    }

    [[nodiscard]] glm::vec3 to_vec3(const float *v) noexcept { return {v[0], v[1], v[2]}; }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_animation_play(SturdyEngine engine, SturdyEntity entity, const char *clip_name,
                                                   SturdyBool loop, float speed) {
    if (clip_name == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "clip_name must not be null");
    }
    return with_entity(engine, entity, "the entity has no animation, or no clip with that name",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           return SFT::Engine::animation_play(world, e, clip_name, loop != 0, speed);
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_speed(SturdyEngine engine, SturdyEntity entity, float speed) {
    return with_entity(engine, entity, "the entity has no animation",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) { return SFT::Engine::animation_set_speed(world, e, speed); });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_playing(SturdyEngine engine, SturdyEntity entity, SturdyBool playing) {
    return with_entity(engine, entity, "the entity has no animation",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) { return SFT::Engine::animation_set_playing(world, e, playing != 0); });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_clip_count(SturdyEngine engine, SturdyEntity entity, uint32_t *out_count) {
    if (out_count == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
    }
    return with_entity(engine, entity, "the entity has no animation clips",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           *out_count = static_cast<uint32_t>(SFT::Engine::animation_clip_names(world, e).size());
                           return true;
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_clip_name(SturdyEngine engine, SturdyEntity entity, uint32_t index,
                                                        char *buffer, size_t capacity, size_t *out_length) {
    SturdyResult copied = STURDY_OK;
    const SturdyResult result = with_entity(engine, entity, "the entity has no animation clips",
                                            [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                                                const auto names = SFT::Engine::animation_clip_names(world, e);
                                                if (index >= names.size()) {
                                                    copied = set_error(STURDY_ERROR_OUT_OF_RANGE, "clip index is out of range");
                                                    return true;
                                                }
                                                copied = copy_string_out(names[index], buffer, capacity, out_length);
                                                return true;
                                            });
    return result != STURDY_OK ? result : copied;
}

SturdyResult STURDY_ABI_CALL sturdy_animation_graph_attach(SturdyEngine engine, SturdyEntity entity,
                                                           const char *graph_json, size_t graph_length) {
    if (graph_json == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "graph_json must not be null");
    }
    std::string failure;
    const SturdyResult result = with_entity(engine, entity, "the graph could not be attached",
                                            [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                                                const std::string_view text = graph_length != 0 ? std::string_view{graph_json, graph_length}
                                                                                                : std::string_view{graph_json};
                                                auto attached = SFT::Engine::animation_attach_graph_json(world, e, text);
                                                if (!attached) {
                                                    failure = attached.error().message.cpp_string();
                                                    return false;
                                                }
                                                return true;
                                            });
    if (result != STURDY_OK && !failure.empty()) {
        return set_error(STURDY_ERROR_NOT_AVAILABLE, failure);
    }
    return result;
}

SturdyResult STURDY_ABI_CALL sturdy_animation_graph_set_float(SturdyEngine engine, SturdyEntity entity,
                                                              const char *name, float value) {
    if (name == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
    }
    return with_entity(engine, entity, "the entity has no animation graph",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) { return SFT::Engine::animation_set_float(world, e, name, value); });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_graph_set_bool(SturdyEngine engine, SturdyEntity entity,
                                                             const char *name, SturdyBool value) {
    if (name == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
    }
    return with_entity(engine, entity, "the entity has no animation graph",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) { return SFT::Engine::animation_set_bool(world, e, name, value != 0); });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_graph_trigger(SturdyEngine engine, SturdyEntity entity, const char *name) {
    if (name == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
    }
    return with_entity(engine, entity, "the entity has no animation graph",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) { return SFT::Engine::animation_set_trigger(world, e, name); });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_graph_state(SturdyEngine engine, SturdyEntity entity, char *buffer,
                                                          size_t capacity, size_t *out_length) {
    SturdyResult copied = STURDY_OK;
    const SturdyResult result = with_entity(engine, entity, "the entity has no animation graph",
                                            [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                                                copied = copy_string_out(SFT::Engine::animation_graph_state(world, e), buffer, capacity, out_length);
                                                return true;
                                            });
    return result != STURDY_OK ? result : copied;
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_import(SturdyEngine engine, const char *source, SturdyAnimationSet *out_set) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr || out_set == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and output pointer must not be null");
        }
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        auto imported = SFT::Engine::import_animations(resolved_engine->assets(), std::string{source});
        if (!imported) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, imported.error().message.cpp_string_view());
        }
        auto owned = std::make_unique<SFT::Engine::ImportedAnimations>(std::move(*imported));
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::AnimationSet, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_set_mutex};
            g_sets.emplace(token, std::move(owned));
        }
        out_set->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_release(SturdyAnimationSet set) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::ImportedAnimations *result = nullptr;
        const SturdyResult resolved = resolve_set(set, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(set.token);
        const std::lock_guard<std::mutex> lock{g_set_mutex};
        g_sets.erase(set.token);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_clip_count(SturdyAnimationSet set, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::ImportedAnimations *result = nullptr;
        const SturdyResult resolved = resolve_set(set, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(result->clips.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_set_clip_name(SturdyAnimationSet set, uint32_t index, char *buffer,
                                                            size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::ImportedAnimations *result = nullptr;
        const SturdyResult resolved = resolve_set(set, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->clips.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "clip index is out of range");
        }
        return copy_string_out(result->clips[index]->name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_adopt(SturdyEngine engine, SturdyEntity entity, SturdyAnimationSet set,
                                                    uint32_t *out_added) {
    SFT::Engine::ImportedAnimations *animations = nullptr;
    if (const SturdyResult resolved = guarded([&]() { return resolve_set(set, &animations); }); resolved != STURDY_OK) {
        return resolved;
    }
    return with_entity(engine, entity, "the entity is not a skeletal character",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           const SFT::usize added = SFT::Engine::animation_adopt_clips(world, e, *animations);
                           if (out_added != nullptr) {
                               *out_added = static_cast<uint32_t>(added);
                           }
                           return added > 0;
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_ik_add_two_bone(SturdyEngine engine, SturdyEntity entity,
                                                              const char *root_joint, const char *mid_joint,
                                                              const char *tip_joint, const float pole[3],
                                                              uint32_t *out_index) {
    if (root_joint == nullptr || mid_joint == nullptr || tip_joint == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "joint names must not be null");
    }
    return with_entity(engine, entity, "the entity has no skeleton, or a joint name is not in it",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           SFT::Animation::TwoBoneIk ik;
                           ik.root = SFT::Engine::animation_joint_index(world, e, root_joint);
                           ik.mid = SFT::Engine::animation_joint_index(world, e, mid_joint);
                           ik.tip = SFT::Engine::animation_joint_index(world, e, tip_joint);
                           if (ik.root == SFT::Animation::no_joint || ik.mid == SFT::Animation::no_joint ||
                               ik.tip == SFT::Animation::no_joint) {
                               return false;
                           }
                           if (pole != nullptr) {
                               ik.pole = to_vec3(pole);
                           }
                           ik.weight = 0.0f; // off until the first target is set
                           const int index = SFT::Engine::animation_add_two_bone_ik(world, e, ik);
                           if (index < 0) {
                               return false;
                           }
                           if (out_index != nullptr) {
                               *out_index = static_cast<uint32_t>(index);
                           }
                           return true;
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_ik_set_target(SturdyEngine engine, SturdyEntity entity, uint32_t index,
                                                            const float target[3], float weight) {
    if (target == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "target must not be null");
    }
    return with_entity(engine, entity, "no such IK constraint on the entity",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           return SFT::Engine::animation_update_two_bone_target(world, e, index, to_vec3(target), weight);
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_look_at_add(SturdyEngine engine, SturdyEntity entity, const char *joint,
                                                          const float forward_axis[3], float max_angle_degrees,
                                                          uint32_t *out_index) {
    if (joint == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "joint name must not be null");
    }
    return with_entity(engine, entity, "the entity has no skeleton, or the joint name is not in it",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           SFT::Animation::LookAtIk ik;
                           ik.joint = SFT::Engine::animation_joint_index(world, e, joint);
                           if (ik.joint == SFT::Animation::no_joint) {
                               return false;
                           }
                           if (forward_axis != nullptr) {
                               ik.forward_axis = to_vec3(forward_axis);
                           }
                           ik.max_angle_degrees = max_angle_degrees;
                           ik.weight = 0.0f;
                           const int index = SFT::Engine::animation_add_look_at(world, e, ik);
                           if (index < 0) {
                               return false;
                           }
                           if (out_index != nullptr) {
                               *out_index = static_cast<uint32_t>(index);
                           }
                           return true;
                       });
}

SturdyResult STURDY_ABI_CALL sturdy_animation_look_at_set_target(SturdyEngine engine, SturdyEntity entity,
                                                                 uint32_t index, const float target[3], float weight) {
    if (target == nullptr) {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, "target must not be null");
    }
    return with_entity(engine, entity, "no such look-at constraint on the entity",
                       [&](SFT::Ecs::World &world, SFT::Ecs::Entity e) {
                           return SFT::Engine::animation_update_look_at_target(world, e, index, to_vec3(target), weight);
                       });
}

} // extern "C"
