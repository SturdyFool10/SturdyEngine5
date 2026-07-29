#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Core/Core.hpp>

#include "Handles.hpp"

namespace SFT::Renderer {

    using OffscreenRenderTargetHandle = Handle<struct OffscreenRenderTargetTag>;

    // Initial off-screen endpoint contract. The backing image is BGRA8UnormSrgb so the complete
    // display pipeline—including fixed-function sRGB encoding, debug text, and UI alpha blending—has
    // identical semantics to the ordinary SDR surface path. Broader linear/HDR encodings require a
    // separate final-output encoding pass and are intentionally not implied by this description.
    struct OffscreenRenderTargetDescription {
        Core::Extent2D extent{};
        UString label;
    };

} // namespace SFT::Renderer
