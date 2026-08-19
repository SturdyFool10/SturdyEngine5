#include <Runtime/Runtime.hpp>

#include <ApplicationHost/EntryPoint.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace SFT::Runtime {

    namespace {

        class RuntimeApplicationClient final : public Engine::ApplicationClient {
          public:
            /// Constructs a `RuntimeApplicationClient` from the supplied initialization values.
            ///
            /// @param config Configuration values controlling the operation.
            /// @param factory `factory` value used by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            RuntimeApplicationClient(RuntimeConfig config, Engine::GameLogicFactory factory)
                : config_(std::move(config)),
                  game_logic_(factory != nullptr ? factory() : nullptr) {}

            /// Reports whether this `RuntimeApplicationClient` has game logic.
            ///
            /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] bool has_game_logic() const noexcept { return game_logic_ != nullptr; }

            /// Returns the current or globally available application config value.
            ///
            /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
            /// @note This function does not throw exceptions.
            [[nodiscard]] const Engine::ApplicationConfig &application_config() const noexcept override {
                return config_.application;
            }

            /// Handles the on engine initialized callback and updates the associated platform state.
            ///
            /// @param engine `engine` value used by the operation.
            ///
            /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
            /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
            [[nodiscard]] Engine::GameLogicResult on_engine_initialized(Engine::Engine &engine) override {
                return game_logic_->on_engine_initialized(engine);
            }

            /// Performs the primary window title operation for `RuntimeApplicationClient` using the supplied arguments.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] UString primary_window_title(
                Engine::Engine &           ,
                const Engine::ApplicationFrameStats &          ) override {
                return config_.primary_window_title;
            }

            /// Requests render frame using the supplied arguments and current state.
            ///
            /// @param engine `engine` value used by the operation.
            /// @param surface Surface used or affected by the operation.
            /// @param frame `frame` value used by the operation.
            ///
            /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
            [[nodiscard]] std::optional<Engine::RenderFrameParameters> request_render_frame(
                Engine::Engine &engine,
                Core::RenderSurfaceHandle surface,
                const Core::FrameInput &frame) override {
                return game_logic_->request_render_frame(engine, surface, frame);
            }

            /// Handles the on shutdown callback and updates the associated platform state.
            ///
            /// @param engine `engine` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            void on_shutdown(Engine::Engine &engine) noexcept override {
                game_logic_->on_shutdown(engine);
            }

          private:
            RuntimeConfig config_{};
            std::unique_ptr<Engine::GameLogic> game_logic_;
        };

    } // namespace

    /// Runs the requested work.
    ///
    /// @param args `args` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param game_logic_factory `game_logic_factory` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
