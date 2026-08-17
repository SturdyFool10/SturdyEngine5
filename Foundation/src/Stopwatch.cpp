#include <Foundation/src/Stopwatch.hpp>


namespace SFT::Foundation {

    f64 Stopwatch::elapsed_seconds() const noexcept {
        return std::chrono::duration<f64>(std::chrono::high_resolution_clock::now() - start_).count();
    }

    std::string Stopwatch::elapsed_human() const noexcept { return human_readable_time(elapsed_seconds()); }

} // namespace SFT::Foundation


namespace SFT::Foundation {

    Stopwatch::Stopwatch() noexcept : start_(std::chrono::high_resolution_clock::now()) {}

} // namespace SFT::Foundation

