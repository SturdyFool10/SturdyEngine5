#pragma once

#include <Engine/Engine.hpp>
#include <Ecs/src/Resource.hpp>

#include <memory>
#include <optional>
#include <vector>

/// A small graphical ECS demo: an on-screen grid of keys that lights up as the physical keyboard is
/// pressed/released, in real time, straight from Engine::KeyboardEvent. Deliberately avoids the
/// UI/Clay text-rendering path (no first-party consumer exercises UiContext::begin_layout() end to
/// end yet — see Engine/src/Engine/EcsUi.hpp) so this demo stays on primitives already proven to
/// work (Mesh/ModelRenderer/WorldTransform, the same path RuntimeDemoGameLogic uses): each key is an
/// individually-colored flat quad, distinguished by size/position rather than a text label. Adding
/// labels later is a natural (and separately scoped) follow-up once something in this codebase
/// actually exercises that UI path for real.
namespace SFT::KeyboardTester {

    /// One physical key this demo tracks, addressed by index into KeyboardGridState::pressed —
    /// GameLogic owns the per-index model Asset (spawned once) and the static layout (key code,
    /// size, grid position); the resource below only holds the live boolean a keyboard system writes.
    /// Kept as one array-shaped resource (mirroring Engine's own FrameTime/TimeScale/WindowState
    /// pattern: a plain member Engine::Application binds by reference) rather than one ECS
    /// entity/component pair per key — there is no per-key gameplay state beyond "is it down right
    /// now," so one resource a single system updates is simpler than ~60 per-entity systems.
    struct KeyboardGridState {
        std::vector<bool> pressed;
    };

    /// Host-independent demo/game session — same GameLogic contract RuntimeDemoGameLogic implements,
    /// so Runtime (or a future Editor) can drive either without knowing which one it's running.
    class KeyboardTesterGameLogic final : public Engine::GameLogic {
      public:
        KeyboardTesterGameLogic();

        [[nodiscard]] Engine::GameLogicResult on_engine_initialized(Engine::Engine &engine) override;
        [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) override;
        void on_shutdown(Engine::Engine &engine) noexcept override;

      private:
        [[nodiscard]] Engine::AssetResult create_key_models(Engine::Engine &engine);
        void configure_render_extraction(Engine::Engine &engine);
        void configure_keyboard_tracking(Engine::Engine &engine);
        void spawn_key_entities(Engine::Engine &engine);
        /// Applies this frame's KeyboardGridState transitions to each changed key's emissive
        /// material color — the one place with the Engine&/AssetManager pair needed to do that;
        /// the keyboard-tracking system (configure_keyboard_tracking) only ever touches CPU-side
        /// ECS state, never assets/materials, matching this codebase's "ECS systems never touch
        /// GPU-adjacent objects directly" rule.
        void apply_key_color_changes(Engine::Engine &engine);

        Engine::Asset key_shader_{};
        Engine::Camera camera_{};
        Engine::RenderGraph render_graph_{};
        KeyboardGridState grid_state_{};
        std::vector<bool> grid_state_last_applied_{};
        std::vector<Engine::Asset> key_models_{};
    };

    [[nodiscard]] std::unique_ptr<Engine::GameLogic> create_keyboard_tester_game_logic();

} // namespace SFT::KeyboardTester

SFT_ECS_RESOURCE(SFT::KeyboardTester::KeyboardGridState, "sturdy.keyboard_tester.grid_state");
