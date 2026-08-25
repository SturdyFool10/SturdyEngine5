#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {

    /// Per-frame constants shared by `svgf_temporal_accumulate.slang` and `svgf_atrous.slang`.
    struct alignas(16) SvgfFrameConstants {
        glm::mat4 inverse_view_projection{1.0f};
        glm::mat4 previous_view_projection{1.0f};
        /// x/y = render extent in pixels, z = 1.0 when history buffers hold valid data from a prior
        /// frame (0.0 immediately after a resolution-change reallocation), w = frame index.
        glm::vec4 extent_history_valid_frame_index{};
        /// x = temporal blend alpha, y = phi-normal, z = phi-depth, w = phi-luminance (a-trous
        /// edge-stopping strengths).
        glm::vec4 temporal_phi_params{};
    };
    static_assert(sizeof(SvgfFrameConstants) == 64 * 2 + 16 * 2);

    /// Push-constant payload for `svgf_atrous.slang`: the wavelet step size doubles each of the
    /// `RestirGiSettings::svgf_atrous_iterations` dispatches (1, 2, 4, 8, 16, ...), so it varies within
    /// a frame and is not part of `SvgfFrameConstants`.
    struct SvgfAtrousConstants {
        u32 step_size;
        /// 1 on the first (step_size == 1) iteration: that iteration's output is also the one copied
        /// back into next frame's temporal history (see svgf_atrous.slang's file header), so it needs
        /// to know to write the history outputs in addition to the ping-pong chain.
        u32 write_history;
    };
    static_assert(sizeof(SvgfAtrousConstants) == 8);

} // namespace SFT::Renderer
