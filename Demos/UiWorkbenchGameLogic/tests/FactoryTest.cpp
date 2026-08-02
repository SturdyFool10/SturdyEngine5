#include <UiWorkbenchGameLogic/UiWorkbenchGameLogic.hpp>

int main() {
    std::unique_ptr<SFT::Engine::GameLogic> game_logic =
        SFT::UiWorkbench::create_ui_workbench_game_logic();
    return game_logic ? 0 : 1;
}
