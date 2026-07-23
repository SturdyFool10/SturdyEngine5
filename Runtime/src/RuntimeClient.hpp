#pragma once

#include <Engine/Engine.hpp>
#include <Ecs/src/Module.hpp>

#include <optional>
#include <vector>

namespace SFT::Runtime {

    struct BloomKeyboardControls {
        f32 threshold_step = 0.05f;
    };

    struct BloomTuningState {
        f32 threshold = 1.25f;
        bool threshold_view = false;
    };

    // Consumer-defined event: registered and consumed through the exact same ECS API as Engine's
    // built-in keyboard/window/mouse events.
    struct BloomThresholdChanged {
        f32 threshold = 1.25f;
        bool threshold_view = false;
    };

    // WASD+QE held-key state and accumulated right-drag mouse-look delta, written by an
    // update_schedule event system and consumed (with the actual per-frame movement math) in
    // request_render_frame — the same split configure_event_systems()/request_render_frame() already
    // uses for BloomKeyboardControls/BloomTuningState, since request_render_frame is the only place
    // with real per-frame delta time (Core::FrameInput::delta_seconds; ECS systems have no dt
    // resource today).
    struct FlyCameraState {
        bool move_forward = false;
        bool move_backward = false;
        bool move_left = false;
        bool move_right = false;
        bool move_up = false;
        bool move_down = false;
        // Accumulated since the last time request_render_frame consumed it (right mouse button held).
        f32 look_delta_x = 0.0f;
        f32 look_delta_y = 0.0f;
        // Absolute orientation, tracked separately from Camera's quaternion to avoid the roll drift
        // that composing incremental yaw_pitch_roll() rotations frame-over-frame would accumulate.
        f32 yaw_degrees = 0.0f;
        f32 pitch_degrees = 0.0f;
    };

    // Simulated native API consumer. Everything here could live in a game executable or editor:
    // Engine owns mechanisms and lifetimes; this client chooses policy, content, and ECS behavior.
    class RuntimeClient final : public Engine::ApplicationClient {
      public:
        RuntimeClient();

        [[nodiscard]] const Engine::ApplicationConfig &application_config() const noexcept override;
        [[nodiscard]] Engine::ApplicationResult on_engine_initialized(Engine::Engine &engine) override;
        [[nodiscard]] UString primary_window_title(
            Engine::Engine &engine,
            const Engine::ApplicationFrameStats &stats) override;

        [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) override;

      private:
        [[nodiscard]] Engine::AssetResult create_demo_content(Engine::Engine &engine);
        void configure_render_extraction(Engine::Engine &engine);
        void configure_event_systems(Engine::Engine &engine);
        void spawn_demo_entities(Engine::Engine &engine);

        Engine::ApplicationConfig config_{};
        Engine::Asset gltf_shader_{};
        Engine::Asset gizmo_shader_{};
        std::vector<Engine::GltfNodeInstance> gltf_instances_{};
        std::vector<Engine::GltfLightInstance> gltf_lights_{};
        Engine::Camera camera_{};
        Engine::RenderGraph render_graph_{};
        Ecs::Entity bloom_controls_entity_{};
        Ecs::Entity camera_control_entity_{};
        Ecs::EventModule<BloomThresholdChanged> bloom_threshold_events_{};
    };

} // namespace SFT::Runtime

SFT_ECS_COMPONENT(SFT::Runtime::BloomKeyboardControls, "sturdy.runtime.bloom_keyboard_controls");
SFT_ECS_COMPONENT(SFT::Runtime::BloomTuningState, "sturdy.runtime.bloom_tuning_state");
SFT_ECS_EVENT(SFT::Runtime::BloomThresholdChanged, "sturdy.runtime.bloom_threshold_changed");
SFT_ECS_COMPONENT(SFT::Runtime::FlyCameraState, "sturdy.runtime.fly_camera_state");
