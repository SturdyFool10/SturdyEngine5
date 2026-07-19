#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
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
        u32 mip_levels = 1;

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
        RHI::TextureSubresourceRange subresources{};
        RHI::LoadOp load_op = RHI::LoadOp::Clear;
        RHI::StoreOp store_op = RHI::StoreOp::Store;
        RHI::ClearColor clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct RenderGraphDepthStencilAttachmentDesc {
        RenderGraphTextureHandle texture{};
        RHI::TextureViewHandle view{}; // null => imported texture default view
        RHI::TextureSubresourceRange subresources{};
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
        RHI::TextureSubresourceRange subresources{};
        RHI::PipelineStage stages = RHI::PipelineStage::FragmentShader;
        RHI::AccessFlags access = RHI::AccessFlags::ShaderRead;
    };

    struct RenderGraphTextureAccess {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        // Layout of mip 0 for legacy callers; pass execution tracks every mip independently.
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

    // Raw same-size, same-format texture->texture copy (no scaling/filtering) — distinct from
    // RenderGraphBlitDesc, which is the scaled/filtered path. History buffers / readback staging.
    struct RenderGraphCopyDesc {
        RenderGraphTextureHandle source{};
        RenderGraphTextureHandle destination{};
        const char *label = nullptr;
    };

    // A storage-image (RHI::TextureLayout::General) access declared by a compute pass. `read`/`write`
    // are independent so a pass can declare read-only, write-only, or read-modify-write access; at
    // least one must be true.
    struct RenderGraphStorageTextureAccessDesc {
        RenderGraphTextureHandle texture{};
        bool read = false;
        bool write = true;
    };

    class RenderGraphComputeContext {
      public:
        RenderGraphComputeContext(RenderGraph &graph, RHI::CommandEncoder &command_encoder,
                                  RHI::ComputePassEncoder &compute_pass) noexcept;

        [[nodiscard]] RHI::CommandEncoder &command_encoder() const noexcept;
        [[nodiscard]] RHI::ComputePassEncoder &compute_pass() const noexcept;
        [[nodiscard]] RenderGraphTextureAccess texture(RenderGraphTextureHandle handle) const noexcept;

      private:
        RenderGraph *graph_ = nullptr;
        RHI::CommandEncoder *command_encoder_ = nullptr;
        RHI::ComputePassEncoder *compute_pass_ = nullptr;
    };

    using RenderGraphComputeExecuteFn = function<Core::RendererResult(RenderGraphComputeContext &)>;

    class RenderGraphComputePassBuilder {
      public:
        explicit RenderGraphComputePassBuilder(string label = {});

        // Sampled (ShaderReadOnly) texture read, always at the compute-shader stage.
        RenderGraphComputePassBuilder &add_sampled_texture(RenderGraphTextureHandle texture);

        // Storage-image (RHI::TextureLayout::General) read/write/read-write access.
        RenderGraphComputePassBuilder &add_storage_texture(const RenderGraphStorageTextureAccessDesc &access);

        RenderGraphComputePassBuilder &set_execute(RenderGraphComputeExecuteFn execute) noexcept;

      private:
        friend class RenderGraph;

        string label_;
        vector<RenderGraphTextureHandle> sampled_texture_reads_;
        vector<RenderGraphStorageTextureAccessDesc> storage_textures_;
        RenderGraphComputeExecuteFn execute_;
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

    // Structured compile() failure. Every code here reflects a graph the caller actually built wrong
    // (not a transient GPU/allocation failure — those stay in execute()'s Core::RendererResult): a
    // pass declared a handle that was never created by this graph, or a pass reads a transient
    // texture no earlier pass produced (a genuinely uninitialized read — imported textures are
    // always valid to read since something outside the graph already gave them real content).
    enum class RenderGraphCompileErrorCode : u8 {
        UnknownTextureHandle,
        MissingProducer,
    };

    struct RenderGraphCompileError {
        RenderGraphCompileErrorCode code = RenderGraphCompileErrorCode::UnknownTextureHandle;
        string message;
    };

    class RenderGraph {
      public:
        // One kind + index pair identifying a declared pass; the compiled order is just these,
        // reordered and with dead entries dropped. Public (not just an implementation detail) so a
        // CPU-only test — or future tooling — can inspect a compiled plan without an RHI device.
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

        // The result of compile(): a dependency-ordered, dead-pass-culled pass list. A small wrapper
        // struct (rather than a bare vector) so the type can grow (e.g. per-pass diagnostics) without
        // changing compile()'s signature.
        struct CompiledPlan {
            vector<OrderedPass> order;
        };

        using CompileResult = std::expected<CompiledPlan, RenderGraphCompileError>;

        [[nodiscard]] RenderGraphTextureHandle import_texture(const RenderGraphImportedTextureDesc &desc);

        [[nodiscard]] RenderGraphTextureHandle create_texture(const RenderGraphTextureDesc &desc);

        [[nodiscard]] RenderGraphRenderPassBuilder &add_render_pass(string_view label);

        [[nodiscard]] RenderGraphComputePassBuilder &add_compute_pass(string_view label);

        void add_blit_pass(const RenderGraphBlitDesc &desc);

        void add_copy_pass(const RenderGraphCopyDesc &desc);

        [[nodiscard]] RenderGraphTextureAccess texture_access(RenderGraphTextureHandle handle) const noexcept;

        // Pure-CPU compile step: derives a dependency-ordered, dead-pass-culled CompiledPlan from every
        // pass/texture declared so far, or a structured RenderGraphCompileError if the graph itself is
        // malformed (an unknown texture handle, or a transient texture read with no earlier producer).
        // Needs no RHI device and performs no GPU work — safe to call from CPU-only tests/tooling, and
        // execute() below is just this plus resource allocation, barrier recording, and pass dispatch.
        [[nodiscard]] CompileResult compile() const;

        // Lazy compile/record boundary. Until this is called, passes are declarations and transient
        // textures are virtual: no GPU allocation or command recording occurs. Despite the historical
        // name, execute() only compiles the dependency graph and records commands into `encoder`; queue
        // submission remains asynchronous unless the high-level graph requests WaitForCompletion.
        [[nodiscard]] Core::RendererResult execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder);

        void destroy_transient_resources(RHI::RhiDevice &device) noexcept;

        // Hands the created transient textures/views to the caller (appending to its vectors) and clears
        // them from the graph, so a later destroy_transient_resources() is a no-op. This is the async
        // model's handoff: once a frame is submitted, its transient targets must outlive the graph object
        // and be destroyed only when the frame's fence retires — the caller owns that deferred cleanup.
        void take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views);

        void reset() noexcept;

        // First/last position a transient texture is read or written at, within a CompiledPlan's order
        // — the input to interval-graph aliasing (see create_transient_resources() in RenderGraph.cpp).
        // -1 means the texture was never used by any live pass, i.e. it gets no physical allocation at
        // all. Public (not just an implementation detail) so a CPU-only test can confirm an unused
        // create_texture() is correctly recognized as dead without needing an RHI device to observe
        // that create_transient_resources() then skips allocating it.
        struct TextureLifetime {
            i32 first_use = -1;
            i32 last_use = -1;
        };
        [[nodiscard]] vector<TextureLifetime> compute_transient_lifetimes(const vector<OrderedPass> &execution_order) const;

      private:
        // The actual GPU-visible backing for one or more virtual transient textures. Two virtual
        // textures whose lifetimes don't overlap (and whose creation desc matches exactly) are assigned
        // the same PhysicalSlot by create_transient_resources()'s aliasing pass, so layout/stage/access
        // state is tracked per mip of the *physical* slot, not per virtual TextureRecord: it reflects
        // real GPU state, which is shared whenever two virtual textures alias. Imported textures always
        // get a dedicated, non-owning slot (owns_resource = false — the graph never destroys it).
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

        struct TextureRecord {
            RenderGraphImportedTextureDesc imported{};
            RenderGraphTextureDesc transient{};
            bool is_transient = false;
            // Index into physical_slots_. Imported textures get one immediately in import_texture();
            // transient textures are only assigned one once create_transient_resources() runs its
            // aliasing pass, so this is ~0u (invalid) between create_texture() and execute().
            u32 physical_slot = ~0u;
            RHI::Format format = RHI::Format::Undefined;
            RHI::Extent3D extent{};
            u32 mip_levels = 1;
            RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
            RHI::PipelineStage final_stage = RHI::PipelineStage::None;
            RHI::AccessFlags final_access = RHI::AccessFlags::None;
            string label;
        };

        [[nodiscard]] TextureRecord *texture_record(RenderGraphTextureHandle handle) noexcept;

        [[nodiscard]] const TextureRecord *texture_record(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) noexcept;

        [[nodiscard]] const PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] Core::RendererResult transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access,
                                                              RHI::TextureSubresourceRange subresources = {});

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

        [[nodiscard]] Core::RendererResult execute_compute_pass(RHI::CommandEncoder &encoder,
                                                                RenderGraphComputePassBuilder &pass);

        [[nodiscard]] Core::RendererResult execute_copy_pass(RHI::CommandEncoder &encoder, const RenderGraphCopyDesc &pass);

        // Runs compile_execution_order()'s topo-sorted/culled order through interval-graph aliasing
        // (see RenderGraph.cpp for the algorithm) before creating one physical GPU texture per resulting
        // slot instead of one per virtual transient texture, then creates the GPU resources.
        [[nodiscard]] Core::RendererResult create_transient_resources(RHI::RhiDevice &device,
                                                                      const vector<OrderedPass> &execution_order);

        [[nodiscard]] Core::RendererResult transition_to_final_states(RHI::CommandEncoder &encoder);

        // What one pass reads from and writes to, in RenderGraphTextureHandle terms — the input to
        // compile()'s dependency analysis. `always_live` covers a pass with no
        // declared attachments at all (doesn't happen from any call site today, but nothing stops
        // one existing): the graph can't reason about a side effect it never declared, so such a
        // pass is never culled. Member functions (not free functions) purely so they can read
        // RenderGraphRenderPassBuilder's private fields via its existing `friend class RenderGraph`.
        struct PassUsage {
            vector<RenderGraphTextureHandle> writes;
            vector<RenderGraphTextureHandle> reads;
            bool always_live = false;
        };
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphRenderPassBuilder &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphBlitDesc &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphComputePassBuilder &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphCopyDesc &pass);
        [[nodiscard]] PassUsage usage_of_ordered(const OrderedPass &ordered) const;

        vector<TextureRecord> textures_;
        vector<PhysicalSlot> physical_slots_;
        vector<OrderedPass> ordered_passes_;
        vector<RenderGraphRenderPassBuilder> render_passes_;
        vector<RenderGraphBlitDesc> blit_passes_;
        vector<RenderGraphComputePassBuilder> compute_passes_;
        vector<RenderGraphCopyDesc> copy_passes_;
    };

    

    

    

    

} // namespace SFT::Renderer
