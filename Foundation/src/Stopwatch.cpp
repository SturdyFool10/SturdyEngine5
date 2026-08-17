#include <Foundation/src/Stopwatch.hpp>


namespace SFT::Foundation {

    /// Returns the current or globally available elapsed seconds value.
    ///
    /// @return Returns the current elapsed seconds value.
    /// @note This function does not throw exceptions.
    f64 Stopwatch::elapsed_seconds() const noexcept {
        return std::chrono::duration<f64>(std::chrono::high_resolution_clock::now() - start_).count();
    }

    /// Returns the current or globally available elapsed human value.
    ///
    /// @return Returns the current elapsed human value.
    /// @note This function does not throw exceptions.
    std::string Stopwatch::elapsed_human() const noexcept { return human_readable_time(elapsed_seconds()); }

} // namespace SFT::Foundation


namespace SFT::Foundation {

    /// Performs the stopwatch operation for `Foundation` using the supplied arguments.
    ///
    /// @note This function does not throw exceptions.
    Stopwatch::Stopwatch() noexcept : start_(std::chrono::high_resolution_clock::now()) {}

} // namespace SFT::Foundation

