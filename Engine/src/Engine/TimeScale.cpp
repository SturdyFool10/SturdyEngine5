#include <Engine/src/Engine/TimeScale.hpp>


namespace SFT::Engine {

    void TimeScale::set(f64 scale) noexcept { scale_ = scale > 0.0 ? scale : 0.0; }

    f64 TimeScale::value() const noexcept { return scale_; }

} // namespace SFT::Engine

