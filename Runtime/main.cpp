import Sturdy.Engine;

using SFT::Engine::Application;

int main() {
    Application app;
    if (app.initialize()) {
        app.run();
    }

    return 0;
}
