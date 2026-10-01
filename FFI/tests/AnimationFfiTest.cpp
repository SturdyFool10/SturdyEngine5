/// Exercises the animation C ABI against a real `Engine`, headless: clip playback control, graph attachment and
/// parameters, animation-set import + retarget, and IK registration. Posing models needs a GPU and is covered by the
/// engine tests and the demo; everything here is component state.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "AnimationFfiTest: %s\n", description);
            ++failures;
        }
    }

    std::shared_ptr<SFT::Animation::Clip> make_clip(const char *name, float x_from, float x_to) {
        auto clip = std::make_shared<SFT::Animation::Clip>();
        clip->name = name;
        clip->channels.resize(3);
        clip->channels[0].translation.times = {0.0f, 1.0f};
        clip->channels[0].translation.values = {x_from, 0, 0, x_to, 0, 0};
        clip->recompute_duration();
        return clip;
    }


} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;
    namespace Animation = SFT::Animation;

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    const SturdyEngine engine{engine_handle.token()};

    auto skeleton = std::make_shared<Animation::Skeleton>();
    skeleton->names = {"Hips", "Spine", "Hand"};
    skeleton->parents = {Animation::no_joint, 0, 1};
    skeleton->rest_pose = {Animation::JointTransform{}, Animation::JointTransform{.translation = {0, 1, 0}},
                           Animation::JointTransform{.translation = {1, 0, 0}}};
    skeleton->inverse_bind.assign(3, glm::mat4(1.0f));

    SFT::Engine::SkeletonAnimator animator;
    animator.skeleton = skeleton;
    animator.clips = {make_clip("Idle", 0, 0), make_clip("Walk", 0, 2)};
    animator.playing = false;
    const SFT::Ecs::Entity spawned = engine_object.ecs_world().spawn(SFT::Engine::WorldTransform{}, std::move(animator));
    const SturdyEntity entity{spawned.index, spawned.generation};

    // ---- clips ---------------------------------------------------------------------------------
    uint32_t count = 0;
    check(sturdy_animation_clip_count(engine, entity, &count) == STURDY_OK && count == 2, "two clips listed");
    char name[64] = {};
    size_t length = 0;
    check(sturdy_animation_clip_name(engine, entity, 1, name, sizeof(name), &length) == STURDY_OK &&
              std::strcmp(name, "Walk") == 0,
          "clip name readable");
    check(sturdy_animation_clip_name(engine, entity, 9, name, sizeof(name), &length) == STURDY_ERROR_OUT_OF_RANGE,
          "bad clip index is out of range");

    check(sturdy_animation_play(engine, entity, "Walk", STURDY_TRUE, 2.0f) == STURDY_OK, "play by name");
    {
        auto animator_ref = engine_object.ecs_world().get_component<SFT::Engine::SkeletonAnimator>(spawned);
        check(animator_ref && animator_ref->clip_index == 1 && animator_ref->playing && animator_ref->speed == 2.0f &&
                  animator_ref->loop,
              "play switched clip, speed, loop and started playback");
    }
    check(sturdy_animation_play(engine, entity, "Nope", STURDY_TRUE, 1.0f) == STURDY_ERROR_NOT_AVAILABLE,
          "unknown clip is reported");
    check(sturdy_animation_set_playing(engine, entity, STURDY_FALSE) == STURDY_OK, "pause");
    check(sturdy_animation_set_speed(engine, entity, 0.5f) == STURDY_OK, "speed");
    check(sturdy_animation_play(engine, SturdyEntity{12345, 1}, "Walk", STURDY_TRUE, 1.0f) == STURDY_ERROR_ENTITY_NOT_ALIVE,
          "dead entity is rejected");
    check(sturdy_animation_play(engine, entity, nullptr, STURDY_TRUE, 1.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "null clip name is rejected");

    // ---- animation set import + adopt ----------------------------------------------------------
    const auto dir = std::filesystem::temp_directory_path() / "sturdy_animation_ffi_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "wave.bvh") << "HIERARCHY\nROOT Hips\n{\nOFFSET 0 1 0\nCHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n"
                                           "JOINT Spine\n{\nOFFSET 0 1 0\nCHANNELS 3 Zrotation Xrotation Yrotation\nEnd Site\n{\nOFFSET 0 1 0\n}\n}\n}\n"
                                           "MOTION\nFrames: 2\nFrame Time: 0.5\n0 0 0 0 0 0 0 0 0\n0 0 0 0 0 0 90 0 0\n";
    }
    SturdyAnimationSet set{};
    const std::string wave_path = (dir / "wave.bvh").string();
    check(sturdy_animation_set_import(engine, wave_path.c_str(), &set) == STURDY_OK, "BVH imports as an animation set");
    check(sturdy_animation_set_clip_count(set, &count) == STURDY_OK && count == 1, "set has one clip");
    check(sturdy_animation_set_clip_name(set, 0, name, sizeof(name), &length) == STURDY_OK && std::strcmp(name, "wave") == 0,
          "set clip named after the file");
    uint32_t added = 0;
    check(sturdy_animation_adopt(engine, entity, set, &added) == STURDY_OK && added == 1, "clips adopted");
    check(sturdy_animation_clip_count(engine, entity, &count) == STURDY_OK && count == 3, "entity now lists three clips");
    check(sturdy_animation_play(engine, entity, "wave", STURDY_FALSE, 1.0f) == STURDY_OK, "adopted clip plays by name");
    check(sturdy_animation_set_release(set) == STURDY_OK, "set released");
    check(sturdy_animation_set_release(set) == STURDY_ERROR_HANDLE_EXPIRED, "double release reported");
    check(sturdy_animation_set_import(engine, (dir / "missing.bvh").string().c_str(), &set) != STURDY_OK, "missing file fails");

    // ---- animation graph -------------------------------------------------------------------------
    const char *graph = R"({
        "parameters": [{"name": "go", "type": "trigger"}, {"name": "speed", "type": "float"}],
        "nodes": [
            {"id": "idle", "type": "clip", "clip": "Idle"},
            {"id": "walk", "type": "clip", "clip": "Walk"},
            {"id": "sm", "type": "state_machine", "entry": "Idle",
             "states": [{"name": "Idle", "node": "idle"}, {"name": "Walk", "node": "walk"}],
             "transitions": [{"from": "Idle", "to": "Walk", "duration": 0.1, "conditions": [{"param": "go", "op": "trigger"}]}]}
        ],
        "layers": [{"node": "sm"}]
    })";
    check(sturdy_animation_graph_attach(engine, entity, "{ broken", 0) == STURDY_ERROR_NOT_AVAILABLE, "bad graph document is reported");
    check(std::strlen(sturdy_last_error_message()) > 0, "the failure carries a message");
    check(sturdy_animation_graph_attach(engine, entity, graph, 0) == STURDY_OK, "graph attaches");
    check(sturdy_animation_graph_set_float(engine, entity, "speed", 1.0f) == STURDY_OK, "graph float parameter");
    check(sturdy_animation_graph_trigger(engine, entity, "go") == STURDY_OK, "graph trigger");
    {
        auto player = engine_object.ecs_world().get_component<SFT::Engine::AnimationGraphPlayer>(spawned);
        check(static_cast<bool>(player), "the entity now carries a graph player");
        if (player) {
            (void)SFT::Engine::tick_graph_player(engine_object.assets(), *player, 0.05f);
        }
    }
    check(sturdy_animation_graph_state(engine, entity, name, sizeof(name), &length) == STURDY_OK && std::strcmp(name, "Walk") == 0,
          "trigger moved the state machine to Walk");
    check(sturdy_animation_play(engine, entity, "Walk", STURDY_TRUE, 1.0f) == STURDY_ERROR_NOT_AVAILABLE,
          "clip playback is not meaningful on a graph player");

    // ---- IK ---------------------------------------------------------------------------------------
    uint32_t ik_index = 99;
    const float pole[3] = {0, 0, 1};
    check(sturdy_animation_ik_add_two_bone(engine, entity, "Hips", "Spine", "Hand", pole, &ik_index) == STURDY_OK && ik_index == 0,
          "IK constraint added");
    const float target[3] = {0.5f, 1.0f, 0.0f};
    check(sturdy_animation_ik_set_target(engine, entity, ik_index, target, 1.0f) == STURDY_OK, "IK target set");
    check(sturdy_animation_ik_set_target(engine, entity, 5, target, 1.0f) == STURDY_ERROR_NOT_AVAILABLE, "unknown IK index reported");
    check(sturdy_animation_ik_add_two_bone(engine, entity, "Hips", "Nothing", "Hand", nullptr, nullptr) == STURDY_ERROR_NOT_AVAILABLE,
          "unknown joint name reported");
    uint32_t look_index = 99;
    const float forward[3] = {0, 0, 1};
    check(sturdy_animation_look_at_add(engine, entity, "Spine", forward, 60.0f, &look_index) == STURDY_OK && look_index == 0,
          "look-at constraint added");
    check(sturdy_animation_look_at_set_target(engine, entity, look_index, target, 0.5f) == STURDY_OK, "look-at target set");

    std::filesystem::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
