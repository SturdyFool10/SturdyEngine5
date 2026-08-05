#pragma once

#include <Foundation/src/Types.hpp>
#include <Foundation/src/Utils.hpp>

#include <chrono>
#include <string>

namespace SFT::Foundation {

    // A tiny wall-clock stopwatch for startup/loading diagnostics — `high_resolution_clock` to match
    // the one hand-rolled duration measurement that already existed before this ("Long frame detected",
    // Engine/src/Engine/ApplicationImpl.cpp), not `steady_clock`. Not for hot per-frame paths; this is
    // for the "how long did this one-shot startup step take" case.
    //
    // ```cpp
    // Stopwatch sw;
    // compile_shader(...);
    // log_info("compiled '{}' in {}", name, sw.elapsed_human());
    // ```
    class Stopwatch {
      public:
        Stopwatch() noexcept : start_(std::chrono::high_resolution_clock::now()) {}

        [[nodiscard]] f64 elapsed_seconds() const noexcept {
            return std::chrono::duration<f64>(std::chrono::high_resolution_clock::now() - start_).count();
        }

        [[nodiscard]] std::string elapsed_human() const noexcept { return human_readable_time(elapsed_seconds()); }

      private:
        std::chrono::high_resolution_clock::time_point start_;
    };

} // namespace SFT::Foundation
