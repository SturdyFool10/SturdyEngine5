#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Engine/Camera.hpp>
#include <Engine/Engine.hpp>
#include <Runtime/Runtime.hpp>

#include <CxxApi/Types/Camera.hpp>
#include <CxxApi/Types/Runtime.hpp>

namespace SFT::CxxApi {

    class FrameBuilder;

    /// A game's callbacks. The binding implements this once, forwarding to its own closures (the only
    /// C++ a `cxx` binding writes for the runtime); see plans/cxx-api.md, "Callbacks".
    class GameLogicCallbacks {
      public:
        virtual ~GameLogicCallbacks() = default;

        /// Called once the engine is up. Return false and fill `error` to abort startup.
        virtual bool on_engine_initialized(Engine::Engine &engine, std::string &error) = 0;

        /// Called per surface per frame. Fill `frame` and return true to render, or return false to
        /// skip this frame.
        virtual bool request_render_frame(Engine::Engine &engine, const FrameInfo &info, FrameBuilder &frame) = 0;

        /// Called once before the engine shuts down.
        virtual void on_shutdown(Engine::Engine &engine) noexcept = 0;
    };

    /// A runtime configuration under construction: `Runtime::RuntimeConfig` plus the strings it
    /// refers to, which must outlive `runtime_run`.
    class RuntimeConfig {
      public:
        Runtime::RuntimeConfig config{};
        std::string window_title{"Sturdy application"};
        std::string app_name{"Sturdy Engine 5"};
    };

    /// The frame a `GameLogicCallbacks::request_render_frame` call is describing.
    class FrameBuilder {
      public:
        FrameBuilder(Engine::Engine &engine, Core::RenderSurfaceHandle surface, const FrameInfo &info,
                     Engine::RenderFrameParameters &parameters) noexcept
            : engine(engine), surface(surface), info(info), parameters(parameters) {}

        Engine::Engine &engine;
        Core::RenderSurfaceHandle surface;
        FrameInfo info;
        Engine::RenderFrameParameters &parameters;
    };

    // ---- runtime configuration ----

    /// The engine's default options.
    [[nodiscard]] RuntimeOptions runtime_options_defaults() noexcept;
    [[nodiscard]] std::unique_ptr<RuntimeConfig> runtime_config_new();
    void runtime_config_set_options(RuntimeConfig &config, const RuntimeOptions &options) noexcept;
    [[nodiscard]] RuntimeOptions runtime_config_options(const RuntimeConfig &config) noexcept;
    void runtime_config_set_window_title(RuntimeConfig &config, const std::string &title);
    void runtime_config_set_app_name(RuntimeConfig &config, const std::string &name);
    void runtime_config_set_shaders_directory(RuntimeConfig &config, const std::string &directory);
    /// Forces a physical device by the id the GPU inventory reports; empty lets the engine choose.
    void runtime_config_set_graphics_device_id(RuntimeConfig &config, const std::string &device_id);
    /// Adds an RHI feature (by `RHI::feature_name`) the device must support. Throws on unknown names.
    void runtime_config_require_feature(RuntimeConfig &config, const std::string &feature);
    /// Adds an RHI feature enabled when the device supports it. Throws on unknown names.
    void runtime_config_request_feature(RuntimeConfig &config, const std::string &feature);
    /// Every RHI feature name the two functions above accept.
    [[nodiscard]] std::unique_ptr<std::vector<std::string>> rhi_feature_names();

    /// Runs the whole application on the calling thread and returns the process exit status. Only one
    /// runtime may run per process at a time; a second concurrent call throws.
    [[nodiscard]] std::int32_t runtime_run(std::unique_ptr<RuntimeConfig> config, const std::vector<std::string> &args,
                                           std::unique_ptr<GameLogicCallbacks> logic);

    /// Registers the ECS -> renderer extraction systems (models, gizmos, directional/spot/point
    /// lights) on `engine`'s extraction schedule. `runtime_run` does this before
    /// `on_engine_initialized`; call it yourself only when driving the engine another way.
    void engine_configure_default_render_extraction(Engine::Engine &engine);

    // ---- per-frame description ----

    [[nodiscard]] FrameInfo frame_info(const FrameBuilder &frame) noexcept;
    /// Builds this frame's camera from `desc`. The engine keeps temporal history per surface;
    /// `desc.reset_history` marks a camera cut.
    void frame_set_camera(FrameBuilder &frame, const CameraDesc &desc);
    /// Uses a camera you own and maintain yourself, including its temporal history
    /// (`camera_commit_frame`).
    void frame_set_camera_object(FrameBuilder &frame, const Engine::Camera &camera);
    void frame_set_lighting(FrameBuilder &frame, const SceneLightingDesc &lighting) noexcept;
    void frame_set_debug_label(FrameBuilder &frame, const std::string &label);

} // namespace SFT::CxxApi
