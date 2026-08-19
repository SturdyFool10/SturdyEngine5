#include <Engine/Application.hpp>

namespace {

    class ProbeClient final : public SFT::Engine::ApplicationClient {
      public:
        /// Returns the current or globally available application config value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const SFT::Engine::ApplicationConfig &application_config() const noexcept override {
            return config_;
        }

        /// Performs the primary window title operation for `ProbeClient` using the supplied arguments.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString primary_window_title(
            SFT::Engine::Engine &,
            const SFT::Engine::ApplicationFrameStats &) override {
            return UString{"GLFW pay-for-use probe"};
        }

        /// Handles the on engine initialized callback and updates the associated platform state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] SFT::Engine::GameLogicResult on_engine_initialized(
            SFT::Engine::Engine &) override {
            return {};
        }

        /// Requests render frame using the supplied arguments and current state.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
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

/// Runs the executable entry point and returns its process exit status.
///
/// @param argc `argc` value used by the operation.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main(int argc, char **) {
    ProbeClient client;
    SFT::Engine::Application application{client};


    return argc == 42 ? (application.initialize() ? 0 : 1) : 0;
}
