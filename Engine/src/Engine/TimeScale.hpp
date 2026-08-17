#pragma once

#include <Ecs/src/Resource.hpp>
#include <Foundation/src/Foundation.hpp>

namespace SFT::Engine {


    class TimeScale {
      public:
        /// Performs the set operation for `TimeScale` using the supplied arguments.
        ///
        /// @param scale `scale` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set(f64 scale) noexcept;

        /// Returns the current or globally available value value.
        ///
        /// @return Returns the current value value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 value() const noexcept;

      private:
        f64 scale_ = 1.0;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::TimeScale, "sturdy.engine.time_scale");
