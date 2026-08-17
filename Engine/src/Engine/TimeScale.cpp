#include <Engine/src/Engine/TimeScale.hpp>


namespace SFT::Engine {

    /// Performs the set operation for `Engine` using the supplied arguments.
    ///
    /// @param scale `scale` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TimeScale::set(f64 scale) noexcept { scale_ = scale > 0.0 ? scale : 0.0; }

    /// Returns the current or globally available value value.
    ///
    /// @return Returns the current value value.
    /// @note This function does not throw exceptions.
    f64 TimeScale::value() const noexcept { return scale_; }

} // namespace SFT::Engine

