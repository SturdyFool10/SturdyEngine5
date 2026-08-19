#pragma once

#include <Ecs/System.hpp>
#include <Ecs/World.hpp>

namespace SFT::Ecs {


    class Module {
      public:
        /// Destroys the `Module` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~Module() = default;


        /// Builds the requested object or derived state.
        ///
        /// @param world World used or affected by the operation.
        /// @param schedule `schedule` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void build(World &world, Schedule &schedule) = 0;
    };


    template <class T>
    class EventModule final : public Module {
      public:
        /// Builds the requested object or derived state.
        ///
        /// @param world World used or affected by the operation.
        /// @param schedule `schedule` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build(World &world, Schedule &schedule) override {
            (void)schedule;
            world.bind_resource(events_);
        }

        /// Returns the current or globally available events value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Events<T> &events() noexcept { return events_; }
        /// Returns the current or globally available events value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Events<T> &events() const noexcept { return events_; }

      private:
        Events<T> events_{};
    };

} // namespace SFT::Ecs
