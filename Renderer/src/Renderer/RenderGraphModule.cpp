#include <Renderer/src/Renderer/RenderGraphModule.hpp>


namespace SFT::Renderer {

    /// Resets the object to its baseline state.
    ///
    /// @return Returns the current reset value.
    /// @note This function does not throw exceptions.
    void RenderGraphBlackboard::reset() noexcept { texture_entries_.clear(); }

    /// Returns the texture count for this `Renderer`.
    ///
    /// @return Returns the current texture count value.
    /// @note This function does not throw exceptions.
    usize RenderGraphBlackboard::texture_count() const noexcept { return texture_entries_.size(); }

    /// Renders texture extent using the current rendering state.
    ///
    /// @return Returns the current render texture extent value.
    /// @note This function does not throw exceptions.
    RHI::Extent3D RenderGraphModuleBuildContext::render_texture_extent() const noexcept {
        return RHI::Extent3D{
            .width = render_extent.x,
            .height = render_extent.y,
            .depth_or_layers = 1,
        };
    }

} // namespace SFT::Renderer


namespace SFT::Renderer {

    /// Renders the requested content using the current rendering state.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RenderGraphBlackboard::RenderGraphBlackboard() { texture_entries_.reserve(8); }

} // namespace SFT::Renderer

