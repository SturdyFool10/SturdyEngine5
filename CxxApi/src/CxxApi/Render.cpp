#include <CxxApi/Render.hpp>

namespace SFT::CxxApi {

    FrameSettings frame_settings_defaults() noexcept { return FrameSettings{}; }

    FrameSettings frame_render_settings(const FrameBuilder &frame) noexcept { return frame.parameters.render_graph.description(); }

    void frame_set_render_settings(FrameBuilder &frame, const FrameSettings &settings) noexcept {
        frame.parameters.render_graph.description() = settings;
    }

} // namespace SFT::CxxApi
