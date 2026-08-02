#pragma once

#include <Engine/Engine.hpp>

#include <memory>
#include <optional>

namespace SFT::UiWorkbench {

    class WorkbenchUi;

    class UiWorkbenchGameLogic final : public Engine::GameLogic {
      public:
        UiWorkbenchGameLogic();
        ~UiWorkbenchGameLogic() override;

        [[nodiscard]] Engine::GameLogicResult on_engine_initialized(Engine::Engine &engine) override;
        [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) override;
        void on_shutdown(Engine::Engine &engine) noexcept override;

      private:
        std::unique_ptr<WorkbenchUi> ui_;
    };

    [[nodiscard]] std::unique_ptr<Engine::GameLogic> create_ui_workbench_game_logic();

} // namespace SFT::UiWorkbench
