// Consumer-facing header/link smoke test for Sturdy::EngineKit and the lower-level GLM API engine
// code commonly uses directly. Exact aggregate membership is asserted in EngineKit/CMakeLists.txt;
// this test deliberately touches no GPU/window/device.
//
// Ecs is exercised directly (not just via Engine's own World, which is private) since it's a
// first-class member of the "core" set, not merely a transitive dependency of Engine.
#include <Foundation/src/Foundation.hpp>
#include <Async/src/Async.hpp>
#include <Ecs/src/World.hpp>
#include <Core/Core.hpp>
#include <Renderer/Renderer.hpp>
#include <Engine/Engine.hpp>
#include <Runtime/Runtime.hpp>

#include <glm/vec3.hpp>

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
