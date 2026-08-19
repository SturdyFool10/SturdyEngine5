#pragma once

#include <Ecs/Resource.hpp>
#include <Foundation/Foundation.hpp>

namespace SFT::Engine {


    class FrameTime {
      public:
        /// Performs the advance operation for `FrameTime` using the supplied arguments.
        ///
        /// @param unscaled_delta_seconds `unscaled_delta_seconds` value used by the operation.
        /// @param scale `scale` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void advance(f64 unscaled_delta_seconds, f64 scale) noexcept;

        /// Returns the current or globally available delta seconds value.
        ///
        /// @return Returns the current delta seconds value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 delta_seconds() const noexcept;
        /// Returns the current or globally available unscaled delta seconds value.
        ///
        /// @return Returns the current unscaled delta seconds value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 unscaled_delta_seconds() const noexcept;
        /// Computes the tick index required by the supplied values.
        ///
        /// @return Returns the current tick index value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 tick_index() const noexcept;

      private:
        f64 delta_seconds_ = 0.0;
        f64 unscaled_delta_seconds_ = 0.0;
        u64 tick_index_ = 0;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::FrameTime, "sturdy.engine.frame_time");
