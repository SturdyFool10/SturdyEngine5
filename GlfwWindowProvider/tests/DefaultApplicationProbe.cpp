#include <Engine/Application.hpp>

namespace {

    class ProbeClient final : public SFT::Engine::ApplicationClient {
      public:
        [[nodiscard]] const SFT::Engine::ApplicationConfig &application_config() const noexcept override {
            return config_;
        }

        [[nodiscard]] UString primary_window_title(
            SFT::Engine::Engine &,
            const SFT::Engine::ApplicationFrameStats &) override {
            return UString{"GLFW pay-for-use probe"};
        }

        [[nodiscard]] SFT::Engine::GameLogicResult on_engine_initialized(
            SFT::Engine::Engine &) override {
            return {};
        }

        [[nodiscard]] std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(
            SFT::Engine::Engine &,
            SFT::Core::RenderSurfaceHandle,
            const SFT::Core::FrameInput &) override {
            return std::nullopt;
        }

      private:
        SFT::Engine::ApplicationConfig config_{};
    };

} // namespace

int main(int argc, char **) {
    ProbeClient client;
    SFT::Engine::Application application{client};


    return argc == 42 ? (application.initialize() ? 0 : 1) : 0;
}
