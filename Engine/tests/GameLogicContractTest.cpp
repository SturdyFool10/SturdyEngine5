#include <Engine/Application.hpp>
#include <Engine/GameLogic.hpp>

#include <memory>
#include <optional>
#include <type_traits>

namespace {

    class StandaloneGameLogic final : public SFT::Engine::GameLogic {
      public:
        /// Handles the on engine initialized callback and updates the associated platform state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] SFT::Engine::GameLogicResult on_engine_initialized(
            SFT::Engine::Engine &           ) override {
            return {};
        }

        /// Requests render frame using the supplied arguments and current state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(
            SFT::Engine::Engine &           ,
            SFT::Core::RenderSurfaceHandle            ,
            const SFT::Core::FrameInput &          ) override {
            return std::nullopt;
        }
    };

    /// Creates a game logic from the supplied parameters.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::unique_ptr<SFT::Engine::GameLogic> create_game_logic() {
        return std::make_unique<StandaloneGameLogic>();
    }

    static_assert(std::is_base_of_v<SFT::Engine::GameLogic, SFT::Engine::ApplicationClient>);
    static_assert(!std::is_base_of_v<SFT::Engine::ApplicationClient, StandaloneGameLogic>);

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const SFT::Engine::GameLogicFactory factory = &create_game_logic;
    return factory() ? 0 : 1;
}
