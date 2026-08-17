#include <Engine/src/Engine/FrameTime.hpp>


namespace SFT::Engine {

    /// Performs the advance operation for `Engine` using the supplied arguments.
    ///
    /// @param unscaled_delta_seconds `unscaled_delta_seconds` value used by the operation.
    /// @param scale `scale` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void FrameTime::advance(f64 unscaled_delta_seconds, f64 scale) noexcept {
        unscaled_delta_seconds_ = unscaled_delta_seconds;
        delta_seconds_ = unscaled_delta_seconds * scale;
        ++tick_index_;
    }

    /// Returns the current or globally available delta seconds value.
    ///
    /// @return Returns the current delta seconds value.
    /// @note This function does not throw exceptions.
    f64 FrameTime::delta_seconds() const noexcept { return delta_seconds_; }

    /// Returns the current or globally available unscaled delta seconds value.
    ///
    /// @return Returns the current unscaled delta seconds value.
    /// @note This function does not throw exceptions.
    f64 FrameTime::unscaled_delta_seconds() const noexcept { return unscaled_delta_seconds_; }

    /// Computes the tick index required by the supplied values.
    ///
    /// @return Returns the current tick index value.
    /// @note This function does not throw exceptions.
    u64 FrameTime::tick_index() const noexcept { return tick_index_; }

} // namespace SFT::Engine

