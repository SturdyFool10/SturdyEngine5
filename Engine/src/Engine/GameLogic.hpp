#pragma once

#include <Foundation/src/Foundation.hpp>

#include <expected>
#include <memory>
#include <optional>

#include "EngineModule.hpp"
#include <Core/Core.hpp>

namespace SFT::Engine {

    struct GameLogicError {
        UString message;
    };

    using GameLogicResult = std::expected<void, GameLogicError>;

    // Host-independent lifecycle for one game/application session. A standalone Runtime host and a
    // future Editor play session can both drive this contract while retaining independent process,
    // window, title, pause/step, and viewport policy.
    class GameLogic {
      public:
        virtual ~GameLogic() = default;

        // Installs resources, systems, assets, and initial entities into one Engine session.
        [[nodiscard]] virtual GameLogicResult on_engine_initialized(Engine &engine) = 0;

        // Produces game-owned policy for one view. The host chooses which surfaces/views to request
        // and when; returning nullopt skips rendering that view without affecting simulation.
        [[nodiscard]] virtual std::optional<RenderFrameParameters> request_render_frame(
            Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) = 0;

        // Called exactly once after successful initialization, while Engine and its GPU device are
        // still valid and after queued CPU submissions and GPU work have been drained.
        virtual void on_shutdown(Engine & /*engine*/) noexcept {}
    };

    // Explicit static-link composition seam. Products reference a concrete factory symbol directly;
    // there is no global registrar, linker-section discovery, or whole-archive requirement. Editors
    // can call the same factory for each fresh play/preview session.
    using GameLogicFactory = std::unique_ptr<GameLogic> (*)();

} // namespace SFT::Engine
