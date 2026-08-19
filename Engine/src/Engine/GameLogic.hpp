#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <memory>
#include <optional>

#include <Engine/EngineModule.hpp>
#include <Core/Core.hpp>

namespace SFT::Engine {

    struct GameLogicError {
        UString message;
    };

    using GameLogicResult = std::expected<void, GameLogicError>;


    class GameLogic {
      public:
        /// Destroys the `GameLogic` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~GameLogic() = default;


        /// Handles the engine initialized event.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual GameLogicResult on_engine_initialized(Engine &engine) = 0;


        /// Requests render frame using the supplied arguments and current state.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        [[nodiscard]] virtual std::optional<RenderFrameParameters> request_render_frame(
            Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) = 0;


        /// Handles the shutdown event.
        ///
        /// @note This function does not throw exceptions.
        virtual void on_shutdown(Engine &           ) noexcept;
    };


    using GameLogicFactory = std::unique_ptr<GameLogic> (*)();

} // namespace SFT::Engine
