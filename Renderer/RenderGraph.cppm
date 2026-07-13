module;

#pragma region Imports
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Renderer:RenderGraph;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;

using std::function;
using std::span;
using std::string;
using std::string_view;
using std::vector;

export namespace SFT::Renderer {

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
        explicit RenderGraphRenderPassBuilder(string label = {}) : label_(std::move(label)) {}

        RenderGraphRenderPassBuilder &add_color_attachment(const RenderGraphColorAttachmentDesc &attachment) {
            color_attachments_.push_back(attachment);
            return *this;
        }

        RenderGraphRenderPassBuilder &set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment) {
            depth_stencil_attachment_ = attachment;
            has_depth_stencil_attachment_ = true;
            return *this;
        }

        RenderGraphRenderPassBuilder &add_sampled_texture(const RenderGraphSampledTextureReadDesc &read) {
            sampled_texture_reads_.push_back(read);
            return *this;
        }

        RenderGraphRenderPassBuilder &set_render_area(const RHI::Rect2D &render_area) noexcept {
            render_area_ = render_area;
            return *this;
        }

        RenderGraphRenderPassBuilder &set_view_mask(u32 view_mask) noexcept {
            view_mask_ = view_mask;
            return *this;
        }

        RenderGraphRenderPassBuilder &set_execute(RenderGraphExecuteFn execute) noexcept {
            execute_ = std::move(execute);
            return *this;
        }

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
        [[nodiscard]] RenderGraphTextureHandle import_texture(const RenderGraphImportedTextureDesc &desc) {
            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .imported = desc,
                .is_transient = false,
                .texture = desc.texture,
                .default_view = desc.default_view,
                .format = desc.format,
                .extent = desc.extent,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .current_layout = desc.initial_layout,
                .current_stage = desc.initial_stage,
                .current_access = desc.initial_access,
                .label = desc.label ? desc.label : "",
            });
            return handle;
        }

        [[nodiscard]] RenderGraphTextureHandle create_texture(const RenderGraphTextureDesc &desc) {
            const RenderGraphTextureHandle handle{static_cast<u32>(textures_.size())};
            textures_.push_back(TextureRecord{
                .transient = desc,
                .is_transient = true,
                .format = desc.format,
                .extent = desc.extent,
                .final_layout = desc.final_layout,
                .final_stage = desc.final_stage,
                .final_access = desc.final_access,
                .current_layout = desc.initial_layout,
                .current_stage = desc.initial_stage,
                .current_access = desc.initial_access,
                .label = desc.label ? desc.label : "",
            });
            return handle;
        }

        [[nodiscard]] RenderGraphRenderPassBuilder &add_render_pass(string_view label) {
            const u32 index = static_cast<u32>(render_passes_.size());
            render_passes_.emplace_back(string{label});
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Render, .index = index});
            return render_passes_.back();
        }

        void add_blit_pass(const RenderGraphBlitDesc &desc) {
            const u32 index = static_cast<u32>(blit_passes_.size());
            blit_passes_.push_back(desc);
            ordered_passes_.push_back(OrderedPass{.kind = PassKind::Blit, .index = index});
        }

        [[nodiscard]] RenderGraphTextureAccess texture_access(RenderGraphTextureHandle handle) const noexcept {
            const TextureRecord *record = texture_record(handle);
            if (record == nullptr) {
                return {};
            }
            return RenderGraphTextureAccess{
                .texture = record->texture,
                .default_view = record->default_view,
                .format = record->format,
                .extent = record->extent,
                .current_layout = record->current_layout,
            };
        }

        [[nodiscard]] Core::RendererResult execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder) {
            if (Core::RendererResult created = create_transient_resources(device); !created.has_value()) {
                destroy_transient_resources(device);
                return created;
            }

            for (const OrderedPass &ordered : ordered_passes_) {
                Core::RendererResult result = {};
                switch (ordered.kind) {
                    case PassKind::Render: {
                        RenderGraphRenderPassBuilder &pass = render_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label_, [&]() {
                            return execute_render_pass(encoder, pass);
                        });
                        break;
                    }
                    case PassKind::Blit: {
                        const RenderGraphBlitDesc &pass = blit_passes_[ordered.index];
                        result = with_debug_group(encoder, pass.label ? pass.label : "render graph blit", [&]() {
                            return execute_blit_pass(encoder, pass);
                        });
                        break;
                    }
                }
                if (!result.has_value()) {
                    destroy_transient_resources(device);
                    return result;
                }
            }

            Core::RendererResult final_transitions = transition_to_final_states(encoder);
            if (!final_transitions.has_value()) {
                destroy_transient_resources(device);
            }
            return final_transitions;
        }

        void destroy_transient_resources(RHI::RhiDevice &device) noexcept {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }
                if (record.default_view) {
                    device.destroy_texture_view(record.default_view);
                }
                if (record.texture) {
                    device.destroy_texture(record.texture);
                }
                record.texture = {};
                record.default_view = {};
            }
        }

        // Hands the created transient textures/views to the caller (appending to its vectors) and clears
        // them from the graph, so a later destroy_transient_resources() is a no-op. This is the async
        // model's handoff: once a frame is submitted, its transient targets must outlive the graph object
        // and be destroyed only when the frame's fence retires — the caller owns that deferred cleanup.
        void take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views) {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }
                if (record.texture) {
                    textures.push_back(record.texture);
                }
                if (record.default_view) {
                    views.push_back(record.default_view);
                }
                record.texture = {};
                record.default_view = {};
            }
        }

        void reset() noexcept {
            ordered_passes_.clear();
            render_passes_.clear();
            blit_passes_.clear();
            textures_.clear();
        }

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

        [[nodiscard]] TextureRecord *texture_record(RenderGraphTextureHandle handle) noexcept {
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

        [[nodiscard]] const TextureRecord *texture_record(RenderGraphTextureHandle handle) const noexcept {
            if (!handle || handle.index >= textures_.size()) {
                return nullptr;
            }
            return &textures_[handle.index];
        }

        [[nodiscard]] Core::RendererResult transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access) {
            TextureRecord *record = texture_record(handle);
            if (record == nullptr || !record->texture) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph pass references an unknown texture.");
            }

            if (record->current_layout == next_layout && record->current_stage == next_stage &&
                record->current_access == next_access) {
                return {};
            }

            const RHI::TextureBarrier barrier{
                .texture = record->texture,
                .src_stage = record->current_stage,
                .src_access = record->current_access,
                .dst_stage = next_stage,
                .dst_access = next_access,
                .old_layout = record->current_layout,
                .new_layout = next_layout,
            };
            encoder.barrier({}, {}, span<const RHI::TextureBarrier>{&barrier, 1});
            record->current_layout = next_layout;
            record->current_stage = next_stage;
            record->current_access = next_access;
            return {};
        }

        [[nodiscard]] Core::RendererResult execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass) {
            for (const RenderGraphSampledTextureReadDesc &read : pass.sampled_texture_reads_) {
                Core::RendererResult transition = transition_texture(encoder,
                                                                     read.texture,
                                                                     RHI::TextureLayout::ShaderReadOnly,
                                                                     read.stages,
                                                                     read.access);
                if (!transition.has_value()) {
                    return transition;
                }
            }

            vector<RHI::ColorAttachment> color_attachments;
            color_attachments.reserve(pass.color_attachments_.size());

            for (const RenderGraphColorAttachmentDesc &attachment : pass.color_attachments_) {
                TextureRecord *record = texture_record(attachment.texture);
                if (record == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph color attachment references an unknown texture.");
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::ColorAttachment,
                                                                     RHI::PipelineStage::ColorAttachmentOutput,
                                                                     RHI::AccessFlags::ColorAttachmentWrite);
                if (!transition.has_value()) {
                    return transition;
                }
                color_attachments.push_back(RHI::ColorAttachment{
                    .view = attachment.view ? attachment.view : record->default_view,
                    .load_op = attachment.load_op,
                    .store_op = attachment.store_op,
                    .clear_color = attachment.clear_color,
                });
            }

            RHI::DepthStencilAttachment depth_stencil{};
            if (pass.has_depth_stencil_attachment_) {
                const RenderGraphDepthStencilAttachmentDesc &attachment = pass.depth_stencil_attachment_;
                TextureRecord *record = texture_record(attachment.texture);
                if (record == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Render graph depth/stencil attachment references an unknown texture.");
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     attachment.texture,
                                                                     RHI::TextureLayout::DepthStencilAttachment,
                                                                     RHI::PipelineStage::EarlyFragmentTests | RHI::PipelineStage::LateFragmentTests,
                                                                     RHI::AccessFlags::DepthStencilAttachmentRead | RHI::AccessFlags::DepthStencilAttachmentWrite);
                if (!transition.has_value()) {
                    return transition;
                }
                depth_stencil = RHI::DepthStencilAttachment{
                    .view = attachment.view ? attachment.view : record->default_view,
                    .depth_load_op = attachment.depth_load_op,
                    .depth_store_op = attachment.depth_store_op,
                    .stencil_load_op = attachment.stencil_load_op,
                    .stencil_store_op = attachment.stencil_store_op,
                    .clear_value = attachment.clear_value,
                };
            }

            const RHI::RenderPassDesc pass_desc{
                .color_attachments = span<const RHI::ColorAttachment>{color_attachments.data(), color_attachments.size()},
                .depth_stencil = depth_stencil,
                .render_area = pass.render_area_,
                .view_mask = pass.view_mask_,
                .label = pass.label_.empty() ? nullptr : pass.label_.c_str(),
            };
            auto render_pass = encoder.begin_render_pass(pass_desc);
            if (!render_pass) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    string("begin render graph pass '") + pass.label_ + "' failed: " + render_pass.error().message);
            }

            if (pass.execute_) {
                RenderGraphContext context{*this, encoder, **render_pass};
                Core::RendererResult result = pass.execute_(context);
                if (!result.has_value()) {
                    (*render_pass)->end();
                    return result;
                }
            }
            (*render_pass)->end();
            return {};
        }

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

        [[nodiscard]] Core::RendererResult execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass) {
            TextureRecord *source = texture_record(pass.source);
            TextureRecord *destination = texture_record(pass.destination);
            if (source == nullptr || destination == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Render graph blit pass references an unknown texture.");
            }

            Core::RendererResult src_transition = transition_texture(encoder,
                                                                     pass.source,
                                                                     RHI::TextureLayout::TransferSrc,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferRead);
            if (!src_transition.has_value()) {
                return src_transition;
            }
            Core::RendererResult dst_transition = transition_texture(encoder,
                                                                     pass.destination,
                                                                     RHI::TextureLayout::TransferDst,
                                                                     RHI::PipelineStage::Transfer,
                                                                     RHI::AccessFlags::TransferWrite);
            if (!dst_transition.has_value()) {
                return dst_transition;
            }

            const RHI::TextureBlit blit{
                .src_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .src_min = RHI::Offset3D{0, 0, 0},
                .src_max = RHI::Offset3D{static_cast<i32>(source->extent.width),
                                         static_cast<i32>(source->extent.height),
                                         static_cast<i32>(source->extent.depth_or_layers)},
                .dst_subresource = RHI::TextureSubresourceLayers{.mip_level = 0, .base_array_layer = 0, .array_layer_count = 1},
                .dst_min = RHI::Offset3D{0, 0, 0},
                .dst_max = RHI::Offset3D{static_cast<i32>(destination->extent.width),
                                         static_cast<i32>(destination->extent.height),
                                         static_cast<i32>(destination->extent.depth_or_layers)},
            };
            encoder.blit_texture(source->texture, destination->texture, blit, pass.filter);
            return {};
        }

        [[nodiscard]] Core::RendererResult create_transient_resources(RHI::RhiDevice &device) {
            for (TextureRecord &record : textures_) {
                if (!record.is_transient) {
                    continue;
                }

                auto texture_handle = device.create_texture(RHI::TextureDesc{
                    .dimension = RHI::TextureDimension::Dim2D,
                    .format = record.transient.format,
                    .extent = record.transient.extent,
                    .mip_levels = record.transient.mip_levels,
                    .samples = record.transient.samples,
                    .usage = record.transient.usage,
                    .label = record.label.empty() ? "render graph transient texture" : record.label.c_str(),
                });
                if (!texture_handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture '") + record.label + "' failed: " + texture_handle.error().message);
                }

                auto view_handle = device.create_texture_view(RHI::TextureViewDesc{
                    .texture = *texture_handle,
                    .view_type = RHI::TextureViewType::View2D,
                    .label = record.label.empty() ? "render graph transient texture view" : record.label.c_str(),
                });
                if (!view_handle) {
                    device.destroy_texture(*texture_handle);
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        string("create render graph transient texture view '") + record.label + "' failed: " + view_handle.error().message);
                }

                record.texture = *texture_handle;
                record.default_view = *view_handle;
            }
            return {};
        }

        [[nodiscard]] Core::RendererResult transition_to_final_states(RHI::CommandEncoder &encoder) {
            for (usize i = 0; i < textures_.size(); ++i) {
                TextureRecord &record = textures_[i];
                if (record.final_layout == RHI::TextureLayout::Undefined) {
                    continue;
                }
                if (record.current_layout == record.final_layout &&
                    record.current_stage == record.final_stage &&
                    record.current_access == record.final_access) {
                    continue;
                }
                Core::RendererResult transition = transition_texture(encoder,
                                                                     RenderGraphTextureHandle{static_cast<u32>(i)},
                                                                     record.final_layout,
                                                                     record.final_stage,
                                                                     record.final_access);
                if (!transition.has_value()) {
                    return transition;
                }
            }
            return {};
        }

        vector<TextureRecord> textures_;
        vector<OrderedPass> ordered_passes_;
        vector<RenderGraphRenderPassBuilder> render_passes_;
        vector<RenderGraphBlitDesc> blit_passes_;
    };

    inline RenderGraphContext::RenderGraphContext(RenderGraph &graph,
                                                  RHI::CommandEncoder &command_encoder,
                                                  RHI::RenderPassEncoder &render_pass) noexcept
        : graph_(&graph), command_encoder_(&command_encoder), render_pass_(&render_pass) {}

    inline RHI::CommandEncoder &RenderGraphContext::command_encoder() const noexcept {
        return *command_encoder_;
    }

    inline RHI::RenderPassEncoder &RenderGraphContext::render_pass() const noexcept {
        return *render_pass_;
    }

    inline RenderGraphTextureAccess RenderGraphContext::texture(RenderGraphTextureHandle handle) const noexcept {
        return graph_ != nullptr ? graph_->texture_access(handle) : RenderGraphTextureAccess{};
    }

} // namespace SFT::Renderer
