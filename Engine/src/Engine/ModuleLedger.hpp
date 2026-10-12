#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <Ecs/World.hpp>
#include <Foundation/Foundation.hpp>

namespace SFT::Engine {

    /// Marks the code running on this thread as belonging to a game-logic module, so whatever it registers (systems, resources,
    /// event channels) is attributed to that module and can be taken away with it when the module is unloaded or reloaded.
    /// `CompositeGameLogic` opens one around each module's `on_engine_initialized`. Scopes nest; the innermost wins.
    class ModuleScope {
      public:
        explicit ModuleScope(std::string module);
        ~ModuleScope();
        ModuleScope(const ModuleScope &) = delete;
        ModuleScope &operator=(const ModuleScope &) = delete;

        /// The module registering on this thread right now; empty when there is none.
        [[nodiscard]] static std::string_view current() noexcept;

      private:
        std::string previous_;
    };

    /// Remembers how to undo each registration a module made, per world. Bindings call `note` right after they register something that
    /// another module's unload must be able to remove; `release` undoes a module's registrations, newest first.
    ///
    /// What a module creates goes away with it: a resource or event channel it created is destroyed (other modules that read it then
    /// see it as missing), and its systems are removed. Components cannot be unregistered, so they stay.
    class ModuleLedger {
      public:
        /// Records `release` against the module in scope on this thread. Does nothing, and returns false, when no scope is open
        /// (a registration made outside any module lives until its own removal call). `release` must tolerate its target already
        /// being gone, because the owner may have removed it first.
        static bool note(const Ecs::World &world, std::function<void()> release);

        /// Runs and forgets everything `module` registered on `world`, newest first. Must not run while the schedule does (call it
        /// from `Engine::defer_to_update_boundary`). Returns how many registrations it undid.
        static usize release(const Ecs::World &world, std::string_view module);

        /// How many registrations are recorded for `module` on `world`.
        [[nodiscard]] static usize count(const Ecs::World &world, std::string_view module);
    };

} // namespace SFT::Engine
