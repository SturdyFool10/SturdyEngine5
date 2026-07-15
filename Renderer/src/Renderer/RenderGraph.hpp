#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::function;
using std::span;
using std::string;
using std::string_view;
using std::vector;

namespace SFT::Renderer {

    // Small, stable typed handle for graph-local textures. The graph deliberately does not expose raw
    // vector indices so future graph compilation can move resources/passes without changing callers.
    struct RenderGraphTextureHandle {
        u32 index = ~0u;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != ~0u; }
        [[nodiscard]] friend constexpr bool operator==(RenderGraphTextureHandle, RenderGraphTextureHandle) noexcept = default;
    };

    struct RenderGraphTextureDesc {
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        u32 mip_levels = 1;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::TextureUsage usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled;

        // State at graph entry for the newly-created texture. Transients almost always start Undefined.
        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;

        // Desired state after the final graph use. Undefined means no final transition. Since transient
        // textures are destroyed after execute(), most internal targets can leave this Undefined unless a
        // pass explicitly needs a terminal layout for debugging/capture consistency.
        RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;

        const char *label = nullptr;
    };

    // A texture already owned outside the graph: swapchain images, persistent history buffers, imported
    // shadow atlases, etc.
    struct RenderGraphImportedTextureDesc {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};

        // State at graph entry. Swapchain acquisition commonly starts Undefined; persistent resources will
        // usually enter in ShaderReadOnly/ColorAttachment/etc. The graph tracks from here.
        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;

        // Desired state at graph exit. Presentable swapchain images use Present; sampled history buffers
        // use ShaderReadOnly. Undefined means "leave in last graph-written state".
        RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;

        const char *label = nullptr;
    };

    struct RenderGraphColorAttachmentDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureViewHandle view{}; // null => imported texture default view
        RHI::LoadOp load_op = RHI::LoadOp::Clear;
        RHI::StoreOp store_op = RHI::StoreOp::Store;
        RHI::ClearColor clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct RenderGraphDepthStencilAttachmentDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureViewHandle view{}; // null => imported texture default view
        RHI::LoadOp depth_load_op = RHI::LoadOp::Clear;
        RHI::StoreOp depth_store_op = RHI::StoreOp::Store;
        RHI::LoadOp stencil_load_op = RHI::LoadOp::DontCare;
        RHI::StoreOp stencil_store_op = RHI::StoreOp::DontCare;
        RHI::ClearDepthStencil clear_value{};
    };

    // Declares that a pass samples a texture through shader resource bindings. The graph does not create
    // the bind group for the shader — materials/post effects still own binding — but it does make the
    // texture's layout and memory visibility correct before the pass callback records draws/dispatches.
    struct RenderGraphSampledTextureReadDesc {
        RenderGraphTextureHandle texture{};
        RHI::PipelineStage stages = RHI::PipelineStage::FragmentShader;
        RHI::AccessFlags access = RHI::AccessFlags::ShaderRead;
    };

    struct RenderGraphTextureAccess {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return texture && default_view; }
    };

    class RenderGraph;

    class RenderGraphContext {
      public:
        RenderGraphContext(RenderGraph &graph, RHI::CommandEncoder &command_encoder, RHI::RenderPassEncoder &render_pass) noexcept;

        [[nodiscard]] RHI::CommandEncoder &command_encoder() const noexcept;
        [[nodiscard]] RHI::RenderPassEncoder &render_pass() const noexcept;
        [[nodiscard]] RenderGraphTextureAccess texture(RenderGraphTextureHandle handle) const noexcept;

      private:
        RenderGraph *graph_ = nullptr;
        RHI::CommandEncoder *command_encoder_ = nullptr;
        RHI::RenderPassEncoder *render_pass_ = nullptr;
    };

    using RenderGraphExecuteFn = function<Core::RendererResult(RenderGraphContext &)>;

    struct RenderGraphBlitDesc {
        RenderGraphTextureHandle source{};
        RenderGraphTextureHandle destination{};
        RHI::Filter filter = RHI::Filter::Linear;
        const char *label = nullptr;
    };

    class RenderGraphRenderPassBuilder {
      public:
        explicit RenderGraphRenderPassBuilder(string label = {});

        RenderGraphRenderPassBuilder &add_color_attachment(const RenderGraphColorAttachmentDesc &attachment);

        RenderGraphRenderPassBuilder &set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment);

        RenderGraphRenderPassBuilder &add_sampled_texture(const RenderGraphSampledTextureReadDesc &read);

        RenderGraphRenderPassBuilder &set_render_area(const RHI::Rect2D &render_area) noexcept;

        RenderGraphRenderPassBuilder &set_view_mask(u32 view_mask) noexcept;

        RenderGraphRenderPassBuilder &set_execute(RenderGraphExecuteFn execute) noexcept;

      private:
        friend class RenderGraph;

        string label_;
        vector<RenderGraphColorAttachmentDesc> color_attachments_;
        RenderGraphDepthStencilAttachmentDesc depth_stencil_attachment_{};
        bool has_depth_stencil_attachment_ = false;
        vector<RenderGraphSampledTextureReadDesc> sampled_texture_reads_;
        RHI::Rect2D render_area_{};
        u32 view_mask_ = 0;
        RenderGraphExecuteFn execute_;
    };

    class RenderGraph {
      public:
        [[nodiscard]] RenderGraphTextureHandle import_texture(const RenderGraphImportedTextureDesc &desc);

        [[nodiscard]] RenderGraphTextureHandle create_texture(const RenderGraphTextureDesc &desc);

        [[nodiscard]] RenderGraphRenderPassBuilder &add_render_pass(string_view label);

        void add_blit_pass(const RenderGraphBlitDesc &desc);

        [[nodiscard]] RenderGraphTextureAccess texture_access(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] Core::RendererResult execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder);

        void destroy_transient_resources(RHI::RhiDevice &device) noexcept;

        // Hands the created transient textures/views to the caller (appending to its vectors) and clears
        // them from the graph, so a later destroy_transient_resources() is a no-op. This is the async
        // model's handoff: once a frame is submitted, its transient targets must outlive the graph object
        // and be destroyed only when the frame's fence retires — the caller owns that deferred cleanup.
        void take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views);

        void reset() noexcept;

      private:
        enum class PassKind : u8 {
            Render,
            Blit,
        };

        struct OrderedPass {
            PassKind kind = PassKind::Render;
            u32 index = 0;
        };
        struct TextureRecord {
            RenderGraphImportedTextureDesc imported{};
            RenderGraphTextureDesc transient{};
            bool is_transient = false;
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle default_view{};
            RHI::Format format = RHI::Format::Undefined;
            RHI::Extent3D extent{};
            RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
            RHI::PipelineStage final_stage = RHI::PipelineStage::None;
            RHI::AccessFlags final_access = RHI::AccessFlags::None;
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
            RHI::PipelineStage current_stage = RHI::PipelineStage::None;
            RHI::AccessFlags current_access = RHI::AccessFlags::None;
            string label;
        };

        [[nodiscard]] TextureRecord *texture_record(RenderGraphTextureHandle handle) noexcept;

        [[nodiscard]] const TextureRecord *texture_record(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] Core::RendererResult transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access);

        [[nodiscard]] Core::RendererResult execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass);

        template <typename Fn>
        [[nodiscard]] Core::RendererResult with_debug_group(RHI::CommandEncoder &encoder, string_view label, Fn &&fn) {
            string label_storage{label};
            if (!label_storage.empty()) {
                encoder.push_debug_group(label_storage.c_str());
            }
            Core::RendererResult result = fn();
            if (!label_storage.empty()) {
                encoder.pop_debug_group();
            }
            return result;
        }

        [[nodiscard]] Core::RendererResult execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass);

        [[nodiscard]] Core::RendererResult create_transient_resources(RHI::RhiDevice &device);

        [[nodiscard]] Core::RendererResult transition_to_final_states(RHI::CommandEncoder &encoder);

        vector<TextureRecord> textures_;
        vector<OrderedPass> ordered_passes_;
        vector<RenderGraphRenderPassBuilder> render_passes_;
        vector<RenderGraphBlitDesc> blit_passes_;
    };

    

    

    

    

} // namespace SFT::Renderer
