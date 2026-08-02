#include <UiWorkbenchGameLogic/UiWorkbenchGameLogic.hpp>

#include <UiWorkbenchGameLogic/WorkbenchUi.hpp>

namespace SFT::UiWorkbench {

    UiWorkbenchGameLogic::UiWorkbenchGameLogic()
        : ui_(std::make_unique<WorkbenchUi>()) {}

    UiWorkbenchGameLogic::~UiWorkbenchGameLogic() = default;

    Engine::GameLogicResult UiWorkbenchGameLogic::on_engine_initialized(Engine::Engine &engine) {
        return ui_->initialize(engine);
    }

    std::optional<Engine::RenderFrameParameters> UiWorkbenchGameLogic::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle surface,
        const Core::FrameInput &frame) {
        return ui_->render(engine, surface, frame);
    }

    void UiWorkbenchGameLogic::on_shutdown(Engine::Engine &engine) noexcept {
        ui_->shutdown(engine);
    }

    std::unique_ptr<Engine::GameLogic> create_ui_workbench_game_logic() {
        return std::make_unique<UiWorkbenchGameLogic>();
    }

} // namespace SFT::UiWorkbench
