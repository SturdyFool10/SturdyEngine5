#include <RuntimeDemoGameLogic/RuntimeDemoGameLogic.hpp>

int main() {
    std::unique_ptr<SFT::Engine::GameLogic> game_logic =
        SFT::Runtime::create_runtime_demo_game_logic();
    return game_logic ? 0 : 1;
}
