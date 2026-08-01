#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

int main() {
    std::unique_ptr<SFT::Engine::GameLogic> game_logic =
        SFT::KeyboardTester::create_keyboard_tester_game_logic();
    return game_logic ? 0 : 1;
}
