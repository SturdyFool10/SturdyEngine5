#include <KeyboardTesterGameLogic/KeyboardTesterGameLogic.hpp>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    std::unique_ptr<SFT::Engine::GameLogic> game_logic =
        SFT::KeyboardTester::create_keyboard_tester_game_logic();
    return game_logic ? 0 : 1;
}
