#include <Engine/Application.hpp>
#include <Engine/GameLogic.hpp>

#include <memory>
#include <optional>
#include <type_traits>

namespace {

    class StandaloneGameLogic final : public SFT::Engine::GameLogic {
      public:
        [[nodiscard]] SFT::Engine::GameLogicResult on_engine_initialized(
            SFT::Engine::Engine & /*engine*/) override {
            return {};
        }

        [[nodiscard]] std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(
            SFT::Engine::Engine & /*engine*/,
            SFT::Core::RenderSurfaceHandle /*surface*/,
            const SFT::Core::FrameInput & /*frame*/) override {
            return std::nullopt;
        }
    };

    [[nodiscard]] std::unique_ptr<SFT::Engine::GameLogic> create_game_logic() {
        return std::make_unique<StandaloneGameLogic>();
    }

    static_assert(std::is_base_of_v<SFT::Engine::GameLogic, SFT::Engine::ApplicationClient>);
    static_assert(!std::is_base_of_v<SFT::Engine::ApplicationClient, StandaloneGameLogic>);

} // namespace

int main() {
    const SFT::Engine::GameLogicFactory factory = &create_game_logic;
    return factory() ? 0 : 1;
}
