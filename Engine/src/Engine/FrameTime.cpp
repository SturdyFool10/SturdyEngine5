#include <Engine/src/Engine/FrameTime.hpp>


namespace SFT::Engine {

    void FrameTime::advance(f64 unscaled_delta_seconds, f64 scale) noexcept {
        unscaled_delta_seconds_ = unscaled_delta_seconds;
        delta_seconds_ = unscaled_delta_seconds * scale;
        ++tick_index_;
    }

    f64 FrameTime::delta_seconds() const noexcept { return delta_seconds_; }

    f64 FrameTime::unscaled_delta_seconds() const noexcept { return unscaled_delta_seconds_; }

    u64 FrameTime::tick_index() const noexcept { return tick_index_; }

} // namespace SFT::Engine

