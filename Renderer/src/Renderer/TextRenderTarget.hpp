#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Renderer/Text/Text.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {


    /// GPU resources a recorded text render must keep alive until its command buffer has finished
    /// executing. Hand back to `TextRenderTarget::release` once the submission's fence has signalled.
    struct TextRenderRecording {
        vector<RHI::BufferHandle> transient_buffers;
        TextAtlasRetiredResources retired_atlas_resources;
    };

    class TextRenderTarget {
      public:
        struct Config {
            u32 width = 0;
            u32 height = 0;


            RHI::Format format = RHI::Format::RGBA8Unorm;
        };

        /// Constructs a `TextRenderTarget` in its default state.
        ///
        /// @note This function does not throw exceptions.
        TextRenderTarget() noexcept = default;

        /// Creates a `TextRenderTarget` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<TextRenderTarget> create(RHI::RhiDevice &device, const Config &config);


        /// Renders the requested content using the current rendering state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param atlas `atlas` value used by the operation.
        /// @param pipeline Pipeline used or affected by the operation.
        /// @param glyphs `glyphs` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render(RHI::RhiDevice &device, TextAtlas &atlas, TextPipeline &pipeline,
                                                  span<const GlyphPlacement> glyphs);


        /// Records the same work as `render` into a caller's command encoder instead of submitting it, so
        /// several targets (and other passes) can be updated in one submission with no per-call fence wait.
        ///
        /// The target's texture is left in `ShaderReadOnly` layout once `encoder` executes. The returned
        /// recording owns transient resources the recorded commands reference: keep it until the
        /// submission has completed, then pass it to `release`.
        ///
        /// @return Returns the recording on success; nothing needs cleaning up on failure.
        [[nodiscard]] Core::RendererExpected<TextRenderRecording> record(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                                         TextAtlas &atlas, TextPipeline &pipeline,
                                                                         span<const GlyphPlacement> glyphs);

        /// Destroys the resources held by a finished recording. Safe to call on an empty recording.
        static void release(RHI::RhiDevice &device, TextRenderRecording &recording) noexcept;

        /// Returns the current or globally available texture value.
        ///
        /// @return Returns the current texture value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::TextureHandle texture() const noexcept;
        /// Returns the current or globally available view value.
        ///
        /// @return Returns the current view value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::TextureViewHandle view() const noexcept;
        /// Returns the current or globally available sampler value.
        ///
        /// @return Returns the current sampler value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::SamplerHandle sampler() const noexcept;
        /// Returns the current or globally available width value.
        ///
        /// @return Returns the current width value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 width() const noexcept;
        /// Returns the current or globally available height value.
        ///
        /// @return Returns the current height value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 height() const noexcept;

        /// Destroys or releases the `TextRenderTarget` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        Config config_{};
        RHI::TextureHandle texture_{};
        RHI::TextureViewHandle view_{};
        RHI::SamplerHandle sampler_{};
        RHI::TextureLayout current_layout_ = RHI::TextureLayout::Undefined;
        TextFrameResources text_resources_{};
        const TextPipeline *resources_pipeline_ = nullptr;
    };

} // namespace SFT::Renderer
