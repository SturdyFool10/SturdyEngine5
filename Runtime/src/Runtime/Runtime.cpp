#include <Runtime/Runtime.hpp>

#include <ApplicationHost/src/EntryPoint.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace SFT::Runtime {

    namespace {

        class RuntimeApplicationClient final : public Engine::ApplicationClient {
          public:
            RuntimeApplicationClient(RuntimeConfig config, Engine::GameLogicFactory factory)
                : config_(std::move(config)),
                  game_logic_(factory != nullptr ? factory() : nullptr) {}

            [[nodiscard]] bool has_game_logic() const noexcept { return game_logic_ != nullptr; }

            [[nodiscard]] const Engine::ApplicationConfig &application_config() const noexcept override {
                return config_.application;
            }

            [[nodiscard]] Engine::GameLogicResult on_engine_initialized(Engine::Engine &engine) override {
                return game_logic_->on_engine_initialized(engine);
            }

            [[nodiscard]] UString primary_window_title(
                Engine::Engine & /*engine*/,
                const Engine::ApplicationFrameStats & /*stats*/) override {
                return config_.primary_window_title;
            }

            [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
                Engine::Engine &engine,
                Core::RenderSurfaceHandle surface,
                const Core::FrameInput &frame) override {
                return game_logic_->request_render_frame(engine, surface, frame);
            }

            void on_shutdown(Engine::Engine &engine) noexcept override {
                game_logic_->on_shutdown(engine);
            }

          private:
            RuntimeConfig config_{};
            std::unique_ptr<Engine::GameLogic> game_logic_;
        };

    } // namespace

    i32 run(
        const Foundation::CliArgs &args,
        RuntimeConfig config,
        Engine::GameLogicFactory game_logic_factory) {
        if (game_logic_factory == nullptr) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "runtime.game_logic.factory_missing",
                .summary = "Runtime cannot start without a GameLogic factory",
                .context = "Runtime::run",
                .cause_code = {},
                .cause = {},
                .details = {},
                .help = "pass an explicit factory function that returns a GameLogic instance",
            });
            return 1;
        }

        RuntimeApplicationClient client{std::move(config), game_logic_factory};
        if (!client.has_game_logic()) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "runtime.game_logic.factory_returned_null",
                .summary = "the GameLogic factory returned no session",
                .context = "Runtime::run",
                .cause_code = "engine.game_logic.null",
                .cause = "the selected factory returned a null unique_ptr",
                .details = {},
                .help = "return a newly constructed GameLogic instance from the factory",
            });
            return 1;
        }

        return ApplicationHost::run(client, args);
    }

} // namespace SFT::Runtime
