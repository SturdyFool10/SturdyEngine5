#pragma once

// Every per-frame render setting. These are the engine's own records, not restatements: the RenderSettings
// package is written to the same rules as Types/ (<cstdint> only, fixed C arrays, `enum class : std::uint32_t`,
// standard-layout and trivially copyable), so bindgen mirrors `SFT::RenderSettings::*` directly and nothing is
// converted at the boundary.

#include <RenderSettings/RenderSettings.hpp>

namespace SFT::CxxApi {

    using FrameSettings = ::SFT::RenderSettings::FrameSettings;

} // namespace SFT::CxxApi
