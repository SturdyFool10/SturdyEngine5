#pragma once

#include <Ecs/src/System.hpp>
#include <Ecs/src/World.hpp>

namespace SFT::Ecs {

    // Bundles a reusable piece of ECS logic — its own resources/event types and systems — into a
    // unit a consumer registers as one step, instead of hand-copying bind_resource()/add_system()
    // calls into application setup code for every feature. A Module typically owns its resource
    // state as ordinary members; those members must outlive every World/Schedule build() bound them
    // into, matching World::bind_resource's own non-owning-reference contract.
    class Module {
      public:
        virtual ~Module() = default;

        // Binds this module's resources into `world` and registers its systems into `schedule`.
        // Called once, in registration order relative to any other module sharing an event/resource
        // type, before Schedule::run() is ever called — see Schedule::validate_event_ordering() for
        // why registration order matters for EventWriter/EventReader pairs specifically.
        virtual void build(World &world, Schedule &schedule) = 0;
    };

    // Minimal owning module for a consumer-defined event channel. The payload still uses
    // SFT_ECS_EVENT for its stable canonical key; this class only removes repetitive ownership and
    // bind_resource boilerplate. Keep the module alive as long as the World using it.
    template <class T>
    class EventModule final : public Module {
      public:
        void build(World &world, Schedule &schedule) override {
            (void)schedule;
            world.bind_resource(events_);
        }

        [[nodiscard]] Events<T> &events() noexcept { return events_; }
        [[nodiscard]] const Events<T> &events() const noexcept { return events_; }

      private:
        Events<T> events_{};
    };

} // namespace SFT::Ecs
