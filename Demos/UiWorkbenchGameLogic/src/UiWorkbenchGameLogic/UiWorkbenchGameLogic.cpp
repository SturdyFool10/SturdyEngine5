#include <UiWorkbenchGameLogic/UiWorkbenchGameLogic.hpp>

#include <UiWorkbenchGameLogic/WorkbenchUi.hpp>

namespace SFT::UiWorkbench {

    /// Performs the UI workbench game logic operation for `UiWorkbench` using the supplied arguments.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UiWorkbenchGameLogic::UiWorkbenchGameLogic()
        : ui_(std::make_unique<WorkbenchUi>()) {}

    /// Destroys the `UiWorkbench` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    UiWorkbenchGameLogic::~UiWorkbenchGameLogic() = default;

    /// Handles the on engine initialized callback and updates the associated platform state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Engine::GameLogicResult UiWorkbenchGameLogic::on_engine_initialized(Engine::Engine &engine) {
        return ui_->initialize(engine);
    }

    /// Requests render frame using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<Engine::RenderFrameParameters> UiWorkbenchGameLogic::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle surface,
        const Core::FrameInput &frame) {
        return ui_->render(engine, surface, frame);
    }

    /// Handles the on shutdown callback and updates the associated platform state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiWorkbenchGameLogic::on_shutdown(Engine::Engine &engine) noexcept {
        ui_->shutdown(engine);
    }

    /// Creates a UI workbench game logic from the supplied parameters.
    ///
    /// @return Returns the current create UI workbench game logic value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::unique_ptr<Engine::GameLogic> create_ui_workbench_game_logic() {
        return std::make_unique<UiWorkbenchGameLogic>();
    }

} // namespace SFT::UiWorkbench
