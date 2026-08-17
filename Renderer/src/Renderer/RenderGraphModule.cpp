#include <Renderer/src/Renderer/RenderGraphModule.hpp>


namespace SFT::Renderer {

    void RenderGraphBlackboard::reset() noexcept { texture_entries_.clear(); }

    usize RenderGraphBlackboard::texture_count() const noexcept { return texture_entries_.size(); }

    RHI::Extent3D RenderGraphModuleBuildContext::render_texture_extent() const noexcept {
        return RHI::Extent3D{
            .width = render_extent.x,
            .height = render_extent.y,
            .depth_or_layers = 1,
        };
    }

} // namespace SFT::Renderer


namespace SFT::Renderer {

    RenderGraphBlackboard::RenderGraphBlackboard() { texture_entries_.reserve(8); }

} // namespace SFT::Renderer

