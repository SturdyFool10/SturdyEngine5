#pragma once

#include <Engine/Engine.hpp>

#include <memory>
#include <optional>

namespace SFT::UiWorkbench {

    class WorkbenchUi;

    class UiWorkbenchGameLogic final : public Engine::GameLogic {
      public:
        /// Constructs a `UiWorkbenchGameLogic` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        UiWorkbenchGameLogic();
        /// Destroys the `UiWorkbenchGameLogic` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~UiWorkbenchGameLogic() override;

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
        std::unique_ptr<WorkbenchUi> ui_;
    };

    /// Creates a UI workbench game logic from the supplied parameters.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::unique_ptr<Engine::GameLogic> create_ui_workbench_game_logic();

} // namespace SFT::UiWorkbench
