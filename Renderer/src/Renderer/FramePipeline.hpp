#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RenderGraph.hpp>
#include <Renderer/RenderGraphModule.hpp>
#include <Renderer/Scene.hpp>

/// The renderer's frame, as an ordered list of named features that anyone can rearrange.
///
/// Every stage of the post-scene half of a frame (motion blur, anti-aliasing, custom effect stages, bloom,
/// tone mapping, debug text, UI) is a `FrameFeature` registered in the renderer's `FramePipeline` under a
/// name. The engine's own features are registered exactly the way yours are, so anything the built-ins do,
/// a replacement can do:
///
/// ```cpp
/// FramePipeline &pipeline = renderer.frame_pipeline();
/// pipeline.replace("tone_mapping", [](FrameBuildContext &frame) {          // wholesale replacement
///     ...frame.graph.add_render_pass(...) reading SceneHdrColor, writing PresentationTarget...
///     return Core::RendererResult{};
/// });
/// pipeline.insert_after("bloom", "my_lens_flare", my_feature);               // add a stage
/// pipeline.set_enabled("motion_blur", false);                                // switch one off
/// pipeline.remove("effects_after_bloom");
/// ```
///
/// Features talk to each other through the render graph's *blackboard* (`RenderGraphSemantics`): each
/// documents which semantic textures it consumes and publishes, and a replacement honours the same
/// contract (tone mapping consumes `SceneHdrColor` and writes `PresentationTarget`, for instance).
namespace SFT::Renderer {

    class Renderer;

    /// What a feature is given each frame.
    struct FrameBuildContext {
        Renderer &renderer;
        RenderGraph &graph;
        /// Semantic textures shared between features (`RenderGraphSemantics::*`).
        RenderGraphBlackboard &resources;
        /// Render and presentation extents, and helpers for sizing transient textures.
        RenderGraphModuleBuildContext &module;
        /// Everything the caller asked this frame to do.
        const RenderGraphSettings &settings;
        RHI::RhiDevice &device;
        /// Bind groups a feature creates for this frame only; the renderer frees them when the frame is
        /// done. Pass this to `Renderer::record_fullscreen_effect`.
        std::vector<RHI::BindGroupHandle> &transient_bind_groups;
        /// Format of the texture that reaches the display, and how it is encoded.
        RHI::Format output_format = RHI::Format::Undefined;
        bool hdr_output = false;
        Core::HdrColorSpaceMode hdr_color_space{};
        /// True when the frame is only a UI overlay drawn straight onto the target (no scene, no tone
        /// mapping): the scene-side features have nothing to do.
        bool direct_overlay_presentation = false;
        /// The presentation target (swapchain or off-screen image) as a graph texture.
        RenderGraphTextureHandle final_output{};
        /// Internal state the engine's own features share. Opaque to everyone else.
        void *builtin = nullptr;
    };

    using FrameFeatureFn = std::function<Core::RendererResult(FrameBuildContext &)>;

    enum class FramePipelineErrorCode : u8 {
        DuplicateName,
        UnknownName,
        EmptyName,
    };

    struct FramePipelineError {
        FramePipelineErrorCode code = FramePipelineErrorCode::UnknownName;
        std::string message;
    };

    template <class Value>
    using FramePipelineExpected = std::expected<Value, FramePipelineError>;

    class FramePipeline {
      public:
        /// Appends a feature after every existing one.
        FramePipelineExpected<void> add(std::string name, FrameFeatureFn build);
        /// Inserts a feature immediately before/after the named one.
        FramePipelineExpected<void> insert_before(std::string_view anchor, std::string name, FrameFeatureFn build);
        FramePipelineExpected<void> insert_after(std::string_view anchor, std::string name, FrameFeatureFn build);
        /// Swaps the implementation of a feature, keeping its place and enabled state.
        FramePipelineExpected<void> replace(std::string_view name, FrameFeatureFn build);
        FramePipelineExpected<void> remove(std::string_view name);
        /// A disabled feature stays in the list but is skipped.
        FramePipelineExpected<void> set_enabled(std::string_view name, bool enabled);
        /// Wraps a feature: `wrapper` receives the original implementation and decides whether/how to call
        /// it (run something before or after, run it twice, skip it conditionally).
        FramePipelineExpected<void> wrap(std::string_view name,
                                         std::function<Core::RendererResult(FrameBuildContext &, const FrameFeatureFn &)> wrapper);

        [[nodiscard]] bool contains(std::string_view name) const noexcept;
        [[nodiscard]] bool enabled(std::string_view name) const noexcept;
        /// Feature names in execution order.
        [[nodiscard]] std::vector<std::string> names() const;

        /// Runs every enabled feature in order, stopping at the first failure.
        [[nodiscard]] Core::RendererResult build(FrameBuildContext &context) const;

      private:
        struct Entry {
            std::string name;
            FrameFeatureFn build;
            bool enabled = true;
        };
        [[nodiscard]] Entry *find(std::string_view name) noexcept;
        [[nodiscard]] const Entry *find(std::string_view name) const noexcept;

        std::vector<Entry> entries_;
    };

} // namespace SFT::Renderer
