

#include <Foundation/Foundation.hpp>
#include <Async/Async.hpp>
#include <Ecs/World.hpp>
#include <Core/Core.hpp>
#include <Renderer/Renderer.hpp>
#include <Engine/Engine.hpp>
#include <Runtime/Runtime.hpp>

#include <glm/vec3.hpp>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const UString name{"engine kit access test"_ustr};

    const bool async_reachable = !SFT::Async::Scheduler::is_running();

    SFT::Ecs::ComponentRegistry ecs_registry;
    SFT::Ecs::World ecs_world{ecs_registry};

    const SFT::Core::Extent2D extent{1, 1};

    const auto tone_mapping = SFT::Renderer::ToneMappingOperator::Agx;

    const SFT::Engine::EngineConfig engine_config{};

    const SFT::Runtime::RuntimeConfig runtime_config{};

    const glm::vec3 position{1.0f, 2.0f, 3.0f};

    const bool ok = !name.empty() && async_reachable && &ecs_world.registry() == &ecs_registry &&
                    extent.x == 1 &&
                    tone_mapping == SFT::Renderer::ToneMappingOperator::Agx &&
                    engine_config.app_name != nullptr &&
                    runtime_config.primary_window_title.empty() == false &&
                    position.x == 1.0f;
    return ok ? 0 : 1;
}
