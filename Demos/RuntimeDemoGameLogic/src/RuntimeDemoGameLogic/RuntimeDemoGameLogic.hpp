#pragma once

#include <Engine/Engine.hpp>
#include <Ecs/Module.hpp>

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


    struct HdrToggleState {
        bool toggle_requested = false;
        bool cycle_color_space_requested = false;
        bool refresh_metadata_requested = false;
    };


    struct BloomThresholdChanged {
        f32 threshold = 1.25f;
        bool threshold_view = false;
    };


    enum class RuntimeRenderingMode : u8 {
        NativeRaster,
        SpectralPathTracing,
    };

    struct RuntimeRenderingState {
        RuntimeRenderingMode mode = RuntimeRenderingMode::NativeRaster;
        u32 raster_msaa_samples = 4;
        bool spectral_path_tracing_available = false;
    };

    struct RuntimeRenderingSettingsChanged {
        RuntimeRenderingMode mode = RuntimeRenderingMode::NativeRaster;
        u32 raster_msaa_samples = 4;
    };

    struct SpectralPathTracingKeyboardControls {
        u32 sample_step = 1;
        u32 bounce_step = 1;
    };

    struct SpectralPathTracingTuningState {
        u32 samples_per_pixel = 1;
        u32 max_bounces = 4;
    };

    struct SpectralPathTracingSettingsChanged {
        u32 samples_per_pixel = 1;
        u32 max_bounces = 4;
    };


    struct FlyCameraState {
        bool move_forward = false;
        bool move_backward = false;
        bool move_left = false;
        bool move_right = false;
        bool move_up = false;
        bool move_down = false;

        f32 look_delta_x = 0.0f;
        f32 look_delta_y = 0.0f;


        f32 yaw_degrees = 0.0f;
        f32 pitch_degrees = 0.0f;
    };


    class RuntimeDemoGameLogic final : public Engine::GameLogic {
      public:
        /// Constructs a `RuntimeDemoGameLogic` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RuntimeDemoGameLogic();

        /// Handles the engine initialized event.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Engine::GameLogicResult on_engine_initialized(Engine::Engine &engine) override;
        /// Requests render frame using the supplied arguments and current state.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) override;
        /// Handles the shutdown event.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void on_shutdown(Engine::Engine &engine) noexcept override;

      private:
        /// Creates a demo content from the supplied parameters.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Engine::AssetResult create_demo_content(Engine::Engine &engine);
        /// Configures render extraction using the supplied arguments and current state.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void configure_render_extraction(Engine::Engine &engine);
        /// Configures event systems using the supplied arguments and current state.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void configure_event_systems(Engine::Engine &engine);
        /// Spawns demo entities.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void spawn_demo_entities(Engine::Engine &engine);


        /// Performs the handle HDR controls operation for `RuntimeDemoGameLogic` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void handle_hdr_controls(Engine::Engine &engine, Core::RenderSurfaceHandle surface);

        Engine::EngineConfig engine_config_{};
        Engine::Asset gltf_shader_{};
        Engine::Asset reference_floor_model_{};
        std::vector<Engine::GltfNodeInstance> gltf_instances_{};
        std::vector<Engine::GltfLightInstance> gltf_lights_{};
        Engine::Camera camera_{};
        Engine::RenderGraph render_graph_{};
        Ecs::Entity bloom_controls_entity_{};
        Ecs::Entity camera_control_entity_{};
        Ecs::Entity hdr_controls_entity_{};
        Ecs::Entity runtime_rendering_entity_{};
        Ecs::Entity spectral_path_tracing_controls_entity_{};
        Ecs::EventModule<BloomThresholdChanged> bloom_threshold_events_{};
        Ecs::EventModule<RuntimeRenderingSettingsChanged> runtime_rendering_events_{};
        Ecs::EventModule<SpectralPathTracingSettingsChanged> spectral_path_tracing_events_{};
    };

    /// Creates a runtime demo game logic from the supplied parameters.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::unique_ptr<Engine::GameLogic> create_runtime_demo_game_logic();

} // namespace SFT::Runtime

SFT_ECS_COMPONENT(SFT::Runtime::BloomKeyboardControls, "sturdy.runtime.bloom_keyboard_controls");
SFT_ECS_COMPONENT(SFT::Runtime::BloomTuningState, "sturdy.runtime.bloom_tuning_state");
SFT_ECS_COMPONENT(SFT::Runtime::HdrToggleState, "sturdy.runtime.hdr_toggle_state");
SFT_ECS_EVENT(SFT::Runtime::BloomThresholdChanged, "sturdy.runtime.bloom_threshold_changed");
SFT_ECS_COMPONENT(SFT::Runtime::FlyCameraState, "sturdy.runtime.fly_camera_state");
SFT_ECS_COMPONENT(SFT::Runtime::RuntimeRenderingState, "sturdy.runtime.rendering_state");
SFT_ECS_EVENT(SFT::Runtime::RuntimeRenderingSettingsChanged, "sturdy.runtime.rendering_settings_changed");
SFT_ECS_COMPONENT(SFT::Runtime::SpectralPathTracingKeyboardControls, "sturdy.runtime.spectral_path_tracing_keyboard_controls");
SFT_ECS_COMPONENT(SFT::Runtime::SpectralPathTracingTuningState, "sturdy.runtime.spectral_path_tracing_tuning_state");
SFT_ECS_EVENT(SFT::Runtime::SpectralPathTracingSettingsChanged, "sturdy.runtime.spectral_path_tracing_settings_changed");
