#pragma once

#include <Engine/Engine.hpp>

#include <optional>

namespace SFT::Runtime {

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
        struct DemoResources {
            Engine::Asset shader{};
            Engine::Asset floor{};
            Engine::Asset sphere{};
            Engine::Asset cube{};
            Engine::Asset torus{};
            Engine::Asset cone{};
        };

        [[nodiscard]] Engine::AssetResult create_demo_content(Engine::Engine &engine);
        void configure_render_extraction(Engine::Engine &engine);
        void spawn_demo_entities(Engine::Engine &engine);

        Engine::ApplicationConfig config_{};
        DemoResources demo_{};
        Engine::Camera camera_{};
        Engine::RenderGraph render_graph_{};
    };

} // namespace SFT::Runtime
