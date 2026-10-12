#pragma once

#include <Ecs/Component.hpp>

namespace SFT::Engine {

    /// Records the field layout of the engine's own plain-data components that other languages share (`Transform`, `WorldTransform`,
    /// `Parent`), so a Rust or C# definition of `"sturdy.engine.transform"` is checked against the C++ one instead of being trusted
    /// because the sizes happen to match. These are declared by hand because reflection does not cover glm types; every offset and
    /// size is guarded by a `static_assert` in the implementation, so the declaration cannot drift from the C++ struct.
    ///
    /// Called by `Engine::Engine`. Idempotent.
    void describe_engine_component_layouts(Ecs::ComponentRegistry &registry);

} // namespace SFT::Engine
