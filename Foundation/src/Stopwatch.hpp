#pragma once

#include <Foundation/src/Types.hpp>
#include <Foundation/src/Utils.hpp>

#include <chrono>
#include <string>

namespace SFT::Foundation {


    class Stopwatch {
      public:
        /// Constructs a `Stopwatch` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Stopwatch() noexcept;

        /// Returns the current or globally available elapsed seconds value.
        ///
        /// @return Returns the current elapsed seconds value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 elapsed_seconds() const noexcept;

        /// Returns the current or globally available elapsed human value.
        ///
        /// @return Returns the current elapsed human value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string elapsed_human() const noexcept;

      private:
        std::chrono::high_resolution_clock::time_point start_;
    };

} // namespace SFT::Foundation
