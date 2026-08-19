#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#pragma endregion

#include <Async/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::expected;
using std::function;
using std::span;
using std::unique_ptr;
using std::vector;

namespace SFT::Renderer {


    struct RenderGraphTextureHandle {
        u32 index = ~0u;
        /// Converts the `RenderGraphTextureHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != ~0u; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(RenderGraphTextureHandle, RenderGraphTextureHandle) noexcept = default;
    };

    struct RenderGraphBufferHandle {
        u32 index = ~0u;
        /// Converts the `RenderGraphBufferHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != ~0u; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(RenderGraphBufferHandle, RenderGraphBufferHandle) noexcept = default;
    };

    struct RenderGraphImportedBufferDesc {
        RHI::BufferHandle buffer{};
        u64 size = 0;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;
        const char *label = nullptr;
    };

    struct RenderGraphBufferAccessDesc {
        RenderGraphBufferHandle buffer{};
        RHI::PipelineStage stages = RHI::PipelineStage::None;
        RHI::AccessFlags access = RHI::AccessFlags::None;


        u64 offset = 0;
        u64 size = 0;
        bool read = true;
        bool write = false;
    };

    struct RenderGraphBufferAccess {
        RHI::BufferHandle buffer{};
        u64 size = 0;
        /// Converts the `RenderGraphBufferAccess` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
    };

    struct RenderGraphTextureDesc {
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        u32 mip_levels = 1;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::TextureUsage usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled;


        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;


        RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;

        const char *label = nullptr;
    };


    struct RenderGraphImportedTextureDesc {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        u32 mip_levels = 1;
        RHI::SampleCount samples = RHI::SampleCount::X1;


        RHI::TextureUsage usage = RHI::TextureUsage::None;


        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;


        RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;

        const char *label = nullptr;
    };

    struct RenderGraphColorAttachmentDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureViewHandle view{};
        RHI::TextureSubresourceRange subresources{};
        RenderGraphTextureHandle resolve_texture{};
        RHI::TextureViewHandle resolve_view{};
        RHI::TextureSubresourceRange resolve_subresources{};
        RHI::LoadOp load_op = RHI::LoadOp::Clear;
        RHI::StoreOp store_op = RHI::StoreOp::Store;
        RHI::ClearColor clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct RenderGraphDepthStencilAttachmentDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureViewHandle view{};
        RHI::TextureSubresourceRange subresources{};
        RenderGraphTextureHandle resolve_texture{};
        RHI::TextureViewHandle resolve_view{};
        RHI::TextureSubresourceRange resolve_subresources{};
        RHI::ResolveMode depth_resolve_mode = RHI::ResolveMode::Minimum;
        RHI::LoadOp depth_load_op = RHI::LoadOp::Clear;
        RHI::StoreOp depth_store_op = RHI::StoreOp::Store;
        RHI::LoadOp stencil_load_op = RHI::LoadOp::DontCare;
        RHI::StoreOp stencil_store_op = RHI::StoreOp::DontCare;
        RHI::ClearDepthStencil clear_value{};
    };


    struct RenderGraphSampledTextureReadDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureSubresourceRange subresources{};
        RHI::PipelineStage stages = RHI::PipelineStage::FragmentShader;
        RHI::AccessFlags access = RHI::AccessFlags::ShaderRead;
    };

    struct RenderGraphTextureAccess {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};

        RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
        /// Converts the `RenderGraphTextureAccess` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return texture && default_view; }
    };

    class RenderGraph;

    class RenderGraphContext {
      public:
        /// Constructs a `RenderGraphContext` from the supplied initialization values.
        ///
        /// @param graph `graph` value used by the operation.
        /// @param command_encoder `command_encoder` value used by the operation.
        /// @param render_pass Render-pass encoder that receives the draw commands.
        ///
        /// @note This function does not throw exceptions.
        RenderGraphContext(RenderGraph &graph, RHI::CommandEncoder &command_encoder, RHI::RenderPassEncoder &render_pass) noexcept;

        /// Returns the current or globally available command encoder value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::CommandEncoder &command_encoder() const noexcept;
        /// Renders pass using the current rendering state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RenderPassEncoder &render_pass() const noexcept;
        /// Performs the texture operation for `RenderGraphContext` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphTextureAccess texture(RenderGraphTextureHandle handle) const noexcept;
        /// Performs the buffer operation for `RenderGraphContext` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphBufferAccess buffer(RenderGraphBufferHandle handle) const noexcept;

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
        UString label;
    };


    struct RenderGraphCopyDesc {
        RenderGraphTextureHandle source{};
        RenderGraphTextureHandle destination{};
        UString label;
    };


    struct RenderGraphStorageTextureAccessDesc {
        RenderGraphTextureHandle texture{};
        bool read = false;
        bool write = true;
    };

    class RenderGraphComputeContext {
      public:
        /// Constructs a `RenderGraphComputeContext` from the supplied initialization values.
        ///
        /// @param graph `graph` value used by the operation.
        /// @param command_encoder `command_encoder` value used by the operation.
        /// @param compute_pass `compute_pass` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        RenderGraphComputeContext(RenderGraph &graph, RHI::CommandEncoder &command_encoder,
                                  RHI::ComputePassEncoder &compute_pass) noexcept;

        /// Returns the current or globally available command encoder value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::CommandEncoder &command_encoder() const noexcept;
        /// Computes pass using the supplied arguments and current state.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::ComputePassEncoder &compute_pass() const noexcept;
        /// Performs the texture operation for `RenderGraphComputeContext` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphTextureAccess texture(RenderGraphTextureHandle handle) const noexcept;
        /// Performs the buffer operation for `RenderGraphComputeContext` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphBufferAccess buffer(RenderGraphBufferHandle handle) const noexcept;

      private:
        RenderGraph *graph_ = nullptr;
        RHI::CommandEncoder *command_encoder_ = nullptr;
        RHI::ComputePassEncoder *compute_pass_ = nullptr;
    };

    using RenderGraphComputeExecuteFn = function<Core::RendererResult(RenderGraphComputeContext &)>;

    class RenderGraphComputePassBuilder {
      public:
        /// Constructs a `RenderGraphComputePassBuilder` from the supplied initialization values.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit RenderGraphComputePassBuilder(const ustr &label = {});


        /// Adds sampled texture using the supplied arguments and current state.
        ///
        /// @param texture Texture used or affected by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphComputePassBuilder &add_sampled_texture(RenderGraphTextureHandle texture);


        /// Adds storage texture using the supplied arguments and current state.
        ///
        /// @param access `access` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphComputePassBuilder &add_storage_texture(const RenderGraphStorageTextureAccessDesc &access);

        /// Adds buffer using the supplied arguments and current state.
        ///
        /// @param access `access` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphComputePassBuilder &add_buffer(const RenderGraphBufferAccessDesc &access);


        /// Sets the side effect for this `RenderGraphComputePassBuilder`.
        ///
        /// @param side_effect `side_effect` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphComputePassBuilder &set_side_effect(bool side_effect = true) noexcept;

        /// Sets the execute for this `RenderGraphComputePassBuilder`.
        ///
        /// @param execute `execute` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphComputePassBuilder &set_execute(RenderGraphComputeExecuteFn execute) noexcept;

      private:
        friend class RenderGraph;

        UString label_;
        vector<RenderGraphTextureHandle> sampled_texture_reads_;
        vector<RenderGraphStorageTextureAccessDesc> storage_textures_;
        vector<RenderGraphBufferAccessDesc> buffers_;
        bool side_effect_ = false;
        RenderGraphComputeExecuteFn execute_;
    };

    class RenderGraphRenderPassBuilder {
      public:
        /// Constructs a `RenderGraphRenderPassBuilder` from the supplied initialization values.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit RenderGraphRenderPassBuilder(const ustr &label = {});

        /// Adds color attachment using the supplied arguments and current state.
        ///
        /// @param attachment `attachment` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphRenderPassBuilder &add_color_attachment(const RenderGraphColorAttachmentDesc &attachment);

        /// Sets the depth stencil attachment for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param attachment `attachment` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphRenderPassBuilder &set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment);

        /// Adds sampled texture using the supplied arguments and current state.
        ///
        /// @param read `read` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphRenderPassBuilder &add_sampled_texture(const RenderGraphSampledTextureReadDesc &read);

        /// Adds buffer using the supplied arguments and current state.
        ///
        /// @param access `access` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RenderGraphRenderPassBuilder &add_buffer(const RenderGraphBufferAccessDesc &access);

        /// Sets the render area for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param render_area `render_area` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphRenderPassBuilder &set_render_area(const RHI::Rect2D &render_area) noexcept;

        /// Sets the view mask for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param view_mask `view_mask` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphRenderPassBuilder &set_view_mask(u32 view_mask) noexcept;


        /// Sets the allow bundles for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param allow_bundles `allow_bundles` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphRenderPassBuilder &set_allow_bundles(bool allow_bundles) noexcept;


        /// Sets the side effect for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param side_effect `side_effect` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphRenderPassBuilder &set_side_effect(bool side_effect = true) noexcept;

        /// Sets the execute for this `RenderGraphRenderPassBuilder`.
        ///
        /// @param execute `execute` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        RenderGraphRenderPassBuilder &set_execute(RenderGraphExecuteFn execute) noexcept;

      private:
        friend class RenderGraph;

        UString label_;
        vector<RenderGraphColorAttachmentDesc> color_attachments_;
        RenderGraphDepthStencilAttachmentDesc depth_stencil_attachment_{};
        bool has_depth_stencil_attachment_ = false;
        vector<RenderGraphSampledTextureReadDesc> sampled_texture_reads_;
        vector<RenderGraphBufferAccessDesc> buffers_;
        RHI::Rect2D render_area_{};
        u32 view_mask_ = 0;
        bool allow_bundles_ = false;
        bool side_effect_ = false;
        RenderGraphExecuteFn execute_;
    };


    enum class RenderGraphCompileErrorCode : u8 {
        UnknownTextureHandle,
        UnknownBufferHandle,
        InvalidBufferAccess,
        IncompatibleTextureCopy,
        MissingProducer,
    };

    struct RenderGraphCompileError {
        RenderGraphCompileErrorCode code = RenderGraphCompileErrorCode::UnknownTextureHandle;
        UString message;
    };

    class RenderGraph {
      public:


        enum class PassKind : u8 {
            Render,
            Blit,
            Compute,
            Copy,
        };

        struct OrderedPass {
            PassKind kind = PassKind::Render;
            u32 index = 0;
        };


        struct CompiledPlan {
            vector<OrderedPass> order;


            vector<u32> levels;
        };

        using CompileResult = expected<CompiledPlan, RenderGraphCompileError>;


        struct GpuPassTiming {
            UString label;
            u32 begin_query_index = 0;
            u32 end_query_index = 0;
        };


        struct CpuPassTiming {
            UString label;
            f64 duration_ms = 0.0;
        };

        /// Imports texture using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle import_texture(const RenderGraphImportedTextureDesc &desc);

        /// Imports buffer using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphBufferHandle import_buffer(const RenderGraphImportedBufferDesc &desc);

        /// Creates a texture from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphTextureHandle create_texture(const RenderGraphTextureDesc &desc);

        /// Adds render pass using the supplied arguments and current state.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphRenderPassBuilder &add_render_pass(const ustr &label);

        /// Adds compute pass using the supplied arguments and current state.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] RenderGraphComputePassBuilder &add_compute_pass(const ustr &label);

        /// Adds blit pass using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void add_blit_pass(const RenderGraphBlitDesc &desc);

        /// Adds copy pass using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void add_copy_pass(const RenderGraphCopyDesc &desc);


        /// Marks output using the supplied arguments and current state.
        ///
        /// @param texture Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void mark_output(RenderGraphTextureHandle texture);

        /// Performs the texture access operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphTextureAccess texture_access(RenderGraphTextureHandle handle) const noexcept;
        /// Performs the buffer access operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RenderGraphBufferAccess buffer_access(RenderGraphBufferHandle handle) const noexcept;


        /// Compiles the supplied source or pipeline state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RenderGraphCompileErrorCode::UnknownTextureHandle`, `RenderGraphCompileErrorCode::UnknownBufferHandle`, `RenderGraphCompileErrorCode::InvalidBufferAccess`, `RenderGraphCompileErrorCode::IncompatibleTextureCopy`, `RenderGraphCompileErrorCode::MissingProducer`.
        [[nodiscard]] CompileResult compile() const;


        /// Executes the requested work.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param timestamp_query_set `timestamp_query_set` value used by the operation.
        /// @param out_pass_timings `out_pass_timings` value used by the operation.
        /// @param out_cpu_pass_timings `out_cpu_pass_timings` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    RHI::QuerySetHandle timestamp_query_set = {},
                                                    vector<GpuPassTiming> *out_pass_timings = nullptr,
                                                    vector<CpuPassTiming> *out_cpu_pass_timings = nullptr);


        /// Executes parallel.
        ///
        /// @param device Device used or affected by the operation.
        /// @param primary_encoder `primary_encoder` value used by the operation.
        /// @param queue Queue used or affected by the operation.
        /// @param out_command_buffers Buffer used or affected by the operation.
        /// @param timestamp_query_set `timestamp_query_set` value used by the operation.
        /// @param out_pass_timings `out_pass_timings` value used by the operation.
        /// @param out_cpu_pass_timings `out_cpu_pass_timings` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute_parallel(RHI::RhiDevice &device,
                                                             unique_ptr<RHI::CommandEncoder> primary_encoder,
                                                             RHI::QueueLane queue,
                                                             vector<RHI::CommandBufferHandle> &out_command_buffers,
                                                             RHI::QuerySetHandle timestamp_query_set = {},
                                                             vector<GpuPassTiming> *out_pass_timings = nullptr,
                                                             vector<CpuPassTiming> *out_cpu_pass_timings = nullptr);

        /// Destroys the transient resources identified by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_transient_resources(RHI::RhiDevice &device) noexcept;


        /// Performs the take transient resources operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param textures Texture used or affected by the operation.
        /// @param views `views` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views);

        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;


        struct TextureLifetime {
            i32 first_use = -1;
            i32 last_use = -1;
        };
        /// Computes transient lifetimes using the supplied arguments and current state.
        ///
        /// @param execution_order `execution_order` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<TextureLifetime> compute_transient_lifetimes(const vector<OrderedPass> &execution_order) const;


        /// Computes execution levels using the supplied arguments and current state.
        ///
        /// @param execution_order `execution_order` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<u32> compute_execution_levels(const vector<OrderedPass> &execution_order) const;

      private:


        struct TextureState {
            RHI::TextureLayout layout = RHI::TextureLayout::Undefined;
            RHI::PipelineStage stage = RHI::PipelineStage::None;
            RHI::AccessFlags access = RHI::AccessFlags::None;
        };

        struct PhysicalSlot {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle default_view{};
            vector<TextureState> mip_states;
            bool owns_resource = false;
        };


        struct BufferRecord {
            RenderGraphImportedBufferDesc imported{};
            RHI::PipelineStage stage = RHI::PipelineStage::None;
            RHI::AccessFlags access = RHI::AccessFlags::None;
            UString label;
        };

        struct TextureRecord {
            RenderGraphImportedTextureDesc imported{};
            RenderGraphTextureDesc transient{};
            bool is_transient = false;


            u32 physical_slot = ~0u;
            RHI::Format format = RHI::Format::Undefined;
            RHI::Extent3D extent{};
            u32 mip_levels = 1;
            RHI::SampleCount samples = RHI::SampleCount::X1;
            RHI::TextureUsage usage = RHI::TextureUsage::None;
            RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
            RHI::PipelineStage final_stage = RHI::PipelineStage::None;
            RHI::AccessFlags final_access = RHI::AccessFlags::None;
            UString label;
        };

        /// Performs the texture record operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextureRecord *texture_record(RenderGraphTextureHandle handle) noexcept;

        /// Performs the texture record operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TextureRecord *texture_record(RenderGraphTextureHandle handle) const noexcept;

        /// Resolves the physical slot associated with the supplied key, handle, or resource.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) noexcept;

        /// Resolves the physical slot associated with the supplied key, handle, or resource.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) const noexcept;

        /// Performs the buffer record operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] BufferRecord *buffer_record(RenderGraphBufferHandle handle) noexcept;
        /// Performs the buffer record operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const BufferRecord *buffer_record(RenderGraphBufferHandle handle) const noexcept;

        /// Performs the transition buffer operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param access `access` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult transition_buffer(RHI::CommandEncoder &encoder,
                                                             const RenderGraphBufferAccessDesc &access);

        /// Performs the transition texture operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param handle Handle identifying the target object or resource.
        /// @param next_layout `next_layout` value used by the operation.
        /// @param next_stage `next_stage` value used by the operation.
        /// @param next_access `next_access` value used by the operation.
        /// @param subresources `subresources` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access,
                                                              RHI::TextureSubresourceRange subresources = {});


        /// Executes one pass.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param ordered `ordered` value used by the operation.
        /// @param begin_query_index Zero-based index of the target element or entry.
        /// @param timestamp_query_set `timestamp_query_set` value used by the operation.
        /// @param timing_enabled `timing_enabled` value used by the operation.
        /// @param cpu_timing_enabled `cpu_timing_enabled` value used by the operation.
        /// @param out_gpu_timing `out_gpu_timing` value used by the operation.
        /// @param out_cpu_timing `out_cpu_timing` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult execute_one_pass(RHI::CommandEncoder &encoder, const OrderedPass &ordered,
                                                             u32 begin_query_index, RHI::QuerySetHandle timestamp_query_set,
                                                             bool timing_enabled, bool cpu_timing_enabled,
                                                             GpuPassTiming *out_gpu_timing, CpuPassTiming *out_cpu_timing);

        /// Executes render pass.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass);

        /// Returns a copy or derived value with debug group applied.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <typename Fn>
        [[nodiscard]] Core::RendererResult with_debug_group(RHI::CommandEncoder &encoder, const UString &label, Fn &&fn) {
            if (!label.empty()) {
                encoder.push_debug_group(label.c_str());
            }
            Core::RendererResult result = fn();
            if (!label.empty()) {
                encoder.pop_debug_group();
            }
            return result;
        }

        /// Executes blit pass.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass);

        /// Executes compute pass.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute_compute_pass(RHI::CommandEncoder &encoder,
                                                                RenderGraphComputePassBuilder &pass);

        /// Executes copy pass.
        ///
        /// @param encoder `encoder` value used by the operation.
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult execute_copy_pass(RHI::CommandEncoder &encoder, const RenderGraphCopyDesc &pass);


        /// Creates a transient resources from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param execution_order `execution_order` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult create_transient_resources(RHI::RhiDevice &device,
                                                                      const vector<OrderedPass> &execution_order);

        /// Performs the transition to final states operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param encoder `encoder` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult transition_to_final_states(RHI::CommandEncoder &encoder);


        struct PassUsage {
            vector<RenderGraphTextureHandle> writes;
            vector<RenderGraphTextureHandle> reads;
            vector<RenderGraphBufferHandle> buffer_writes;
            vector<RenderGraphBufferHandle> buffer_reads;
            bool always_live = false;
        };
        /// Performs the pass usage of operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphRenderPassBuilder &pass);
        /// Performs the pass usage of operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphBlitDesc &pass);
        /// Performs the pass usage of operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphComputePassBuilder &pass);
        /// Performs the pass usage of operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphCopyDesc &pass);
        /// Performs the usage of ordered operation for `RenderGraph` using the supplied arguments.
        ///
        /// @param ordered `ordered` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] PassUsage usage_of_ordered(const OrderedPass &ordered) const;


        /// Computes levels from usage using the supplied arguments and current state.
        ///
        /// @param usage_by_position `usage_by_position` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<u32> compute_levels_from_usage(const vector<PassUsage> &usage_by_position) const;

        vector<TextureRecord> textures_;
        vector<PhysicalSlot> physical_slots_;
        vector<BufferRecord> buffers_;


        vector<OrderedPass> ordered_passes_;
        vector<RenderGraphRenderPassBuilder> render_passes_;
        vector<RenderGraphBlitDesc> blit_passes_;
        vector<RenderGraphComputePassBuilder> compute_passes_;
        vector<RenderGraphCopyDesc> copy_passes_;
        vector<RenderGraphTextureHandle> outputs_;
    };


} // namespace SFT::Renderer
