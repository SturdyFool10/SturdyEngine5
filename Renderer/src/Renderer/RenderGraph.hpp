#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#pragma endregion

#include <Async/src/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::expected;
using std::function;
using std::span;
using std::unique_ptr;
using std::vector;

namespace SFT::Renderer {

    /// Small, stable typed handle for graph-local textures. The graph deliberately does not expose raw
    /// vector indices so future graph compilation can move resources/passes without changing callers.
    struct RenderGraphTextureHandle {
        u32 index = ~0u;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != ~0u; }
        [[nodiscard]] friend constexpr bool operator==(RenderGraphTextureHandle, RenderGraphTextureHandle) noexcept = default;
    };

    struct RenderGraphBufferHandle {
        u32 index = ~0u;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return index != ~0u; }
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
        /// Range participates in validation and future interval-aware scheduling. Synchronization is
        /// currently conservatively emitted for the whole imported buffer because state is whole-buffer.
        u64 offset = 0;
        u64 size = 0;
        bool read = true;
        bool write = false;
    };

    struct RenderGraphBufferAccess {
        RHI::BufferHandle buffer{};
        u64 size = 0;
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
    };

    struct RenderGraphTextureDesc {
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        u32 mip_levels = 1;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        RHI::TextureUsage usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled;

        /// State at graph entry for the newly-created texture. Transients almost always start Undefined.
        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;

        /// Desired state after the final graph use. Undefined means no final transition. Since transient
        /// textures are destroyed after execute(), most internal targets can leave this Undefined unless a
        /// pass explicitly needs a terminal layout for debugging/capture consistency.
        RHI::TextureLayout final_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage final_stage = RHI::PipelineStage::None;
        RHI::AccessFlags final_access = RHI::AccessFlags::None;

        const char *label = nullptr;
    };

    /// A texture already owned outside the graph: swapchain images, persistent history buffers, cached
    /// post-process targets, etc.
    struct RenderGraphImportedTextureDesc {
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle default_view{};
        RHI::Format format = RHI::Format::Undefined;
        RHI::Extent3D extent{};
        u32 mip_levels = 1;
        RHI::SampleCount samples = RHI::SampleCount::X1;
        /// Capabilities of the externally-created image. Required for validating copy/storage uses;
        /// the graph never mutates the underlying texture's creation flags.
        RHI::TextureUsage usage = RHI::TextureUsage::None;

        /// State at graph entry. Swapchain acquisition commonly starts Undefined; persistent resources will
        /// usually enter in ShaderReadOnly/ColorAttachment/etc. The graph tracks from here.
        RHI::TextureLayout initial_layout = RHI::TextureLayout::Undefined;
        RHI::PipelineStage initial_stage = RHI::PipelineStage::None;
        RHI::AccessFlags initial_access = RHI::AccessFlags::None;

        /// Desired state at graph exit. Presentable swapchain images use Present; sampled history buffers
        /// use ShaderReadOnly. Undefined means "leave in last graph-written state".
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

    /// Declares that a pass samples a texture through shader resource bindings. The graph does not create
    /// the bind group for the shader — materials/post effects still own binding — but it does make the
    /// texture's layout and memory visibility correct before the pass callback records draws/dispatches.
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
        /// Layout of mip 0 for legacy callers; pass execution tracks every mip independently.
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

    /// Raw same-size, same-format texture->texture copy (no scaling/filtering) — distinct from
    /// RenderGraphBlitDesc, which is the scaled/filtered path. History buffers / readback staging.
    struct RenderGraphCopyDesc {
        RenderGraphTextureHandle source{};
        RenderGraphTextureHandle destination{};
        UString label;
    };

    /// A storage-image (RHI::TextureLayout::General) access declared by a compute pass. `read`/`write`
    /// are independent so a pass can declare read-only, write-only, or read-modify-write access; at
    /// least one must be true.
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
        [[nodiscard]] RenderGraphBufferAccess buffer(RenderGraphBufferHandle handle) const noexcept;

      private:
        RenderGraph *graph_ = nullptr;
        RHI::CommandEncoder *command_encoder_ = nullptr;
        RHI::ComputePassEncoder *compute_pass_ = nullptr;
    };

    using RenderGraphComputeExecuteFn = function<Core::RendererResult(RenderGraphComputeContext &)>;

    class RenderGraphComputePassBuilder {
      public:
        explicit RenderGraphComputePassBuilder(const ustr &label = {});

        /// Sampled (ShaderReadOnly) texture read, always at the compute-shader stage.
        RenderGraphComputePassBuilder &add_sampled_texture(RenderGraphTextureHandle texture);

        /// Storage-image (RHI::TextureLayout::General) read/write/read-write access.
        RenderGraphComputePassBuilder &add_storage_texture(const RenderGraphStorageTextureAccessDesc &access);

        RenderGraphComputePassBuilder &add_buffer(const RenderGraphBufferAccessDesc &access);

        /// Keeps this pass live even when none of its declared texture writes contribute to a marked
        /// graph output (for example, a compute pass whose externally visible result is a buffer).
        RenderGraphComputePassBuilder &set_side_effect(bool side_effect = true) noexcept;

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
        explicit RenderGraphRenderPassBuilder(const ustr &label = {});

        RenderGraphRenderPassBuilder &add_color_attachment(const RenderGraphColorAttachmentDesc &attachment);

        RenderGraphRenderPassBuilder &set_depth_stencil_attachment(const RenderGraphDepthStencilAttachmentDesc &attachment);

        RenderGraphRenderPassBuilder &add_sampled_texture(const RenderGraphSampledTextureReadDesc &read);

        RenderGraphRenderPassBuilder &add_buffer(const RenderGraphBufferAccessDesc &access);

        RenderGraphRenderPassBuilder &set_render_area(const RHI::Rect2D &render_area) noexcept;

        RenderGraphRenderPassBuilder &set_view_mask(u32 view_mask) noexcept;

        /// Opts this pass into recording via RHI::RenderBundleEncoder / RenderPassEncoder::execute_bundles
        /// (secondary command buffers) — Vulkan's vkCmdBeginRendering only permits vkCmdExecuteCommands
        /// inside a render pass instance opened with VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
        /// set (VUID-vkCmdExecuteCommands-flags-06024), and per the same dynamic-rendering model that bit
        /// and ordinary inline vkCmdDraw calls are mutually exclusive within one instance — so any pass
        /// whose execute_ callback might call execute_bundles (record_render_items_culled's own
        /// >kParallelRecordThreshold-items parallel path, or a hand-rolled bundle path like the shadow
        /// atlas pass) must set this to true, and must record *only* via bundles when it does (no direct
        /// inline pass.draw()-style calls in that same pass). False by default, matching every ordinary
        /// inline-recording pass.
        RenderGraphRenderPassBuilder &set_allow_bundles(bool allow_bundles) noexcept;

        /// Keeps this pass live even when its attachments do not contribute to a marked graph output.
        RenderGraphRenderPassBuilder &set_side_effect(bool side_effect = true) noexcept;

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

    /// Structured compile() failure. Every code here reflects a graph the caller actually built wrong
    /// (not a transient GPU/allocation failure — those stay in execute()'s Core::RendererResult): a
    /// pass declared a handle that was never created by this graph, or a pass reads a transient
    /// texture no earlier pass produced (a genuinely uninitialized read — imported textures are
    /// always valid to read since something outside the graph already gave them real content).
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
        /// One kind + index pair identifying a declared pass; the compiled order is just these,
        /// reordered and with dead entries dropped. Public (not just an implementation detail) so a
        /// CPU-only test — or future tooling — can inspect a compiled plan without an RHI device.
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

        /// The result of compile(): a dependency-ordered, dead-pass-culled pass list. A small wrapper
        /// struct (rather than a bare vector) so the type can grow (e.g. per-pass diagnostics) without
        /// changing compile()'s signature.
        struct CompiledPlan {
            vector<OrderedPass> order;
            /// Same length/indexing as `order` — compile() computes logical-resource levels for CPU-only
            /// inspection. execute_parallel() recomputes them after transient alias allocation so passes
            /// touching different logical textures backed by one physical image are serialized too.
            vector<u32> levels;
        };

        using CompileResult = expected<CompiledPlan, RenderGraphCompileError>;

        /// One executed pass's GPU timing, filled by execute() when a valid `timestamp_query_set` is
        /// passed in. `begin_query_index`/`end_query_index` are slots in that query set (both written
        /// at PipelineStage::AllCommands, immediately before/after the pass's own work — bracketing
        /// exactly that pass's GPU duration); the caller resolves them to nanoseconds via
        /// RHI::DeviceLimits::timestamp_period_ns once this frame's fence proves the GPU wrote them.
        struct GpuPassTiming {
            UString label;
            u32 begin_query_index = 0;
            u32 end_query_index = 0;
        };

        /// One executed pass's CPU recording cost — wall-clock time spent inside that pass's
        /// execute_ callback dispatch (barrier insertion + the callback itself). Unlike
        /// GpuPassTiming this needs no query set/fence round trip: it's ready the instant execute()
        /// returns, so the caller can log/display it immediately rather than one frame stale (though
        /// in practice most callers still stash it a frame, same as GpuPassTiming, simply because
        /// debug-overlay text for frame N is built before frame N's own execute() call runs).
        struct CpuPassTiming {
            UString label;
            f64 duration_ms = 0.0;
        };

        [[nodiscard]] RenderGraphTextureHandle import_texture(const RenderGraphImportedTextureDesc &desc);

        [[nodiscard]] RenderGraphBufferHandle import_buffer(const RenderGraphImportedBufferDesc &desc);

        [[nodiscard]] RenderGraphTextureHandle create_texture(const RenderGraphTextureDesc &desc);

        [[nodiscard]] RenderGraphRenderPassBuilder &add_render_pass(const ustr &label);

        [[nodiscard]] RenderGraphComputePassBuilder &add_compute_pass(const ustr &label);

        void add_blit_pass(const RenderGraphBlitDesc &desc);

        void add_copy_pass(const RenderGraphCopyDesc &desc);

        /// Marks a texture as a liveness root. Passes producing it and their ancestry execute even when
        /// disconnected from presentation; this does not export or preserve a transient after its final
        /// graph use. Importing a texture alone does not keep speculative history/cache writes live.
        void mark_output(RenderGraphTextureHandle texture);

        [[nodiscard]] RenderGraphTextureAccess texture_access(RenderGraphTextureHandle handle) const noexcept;
        [[nodiscard]] RenderGraphBufferAccess buffer_access(RenderGraphBufferHandle handle) const noexcept;

        /// Pure-CPU compile step: derives a dependency-ordered, dead-pass-culled CompiledPlan from every
        /// pass/resource declared so far, or a structured RenderGraphCompileError if the graph itself is
        /// malformed (unknown handles, invalid buffer ranges, or a transient read with no producer).
        /// Needs no RHI device and performs no GPU work — safe to call from CPU-only tests/tooling, and
        /// execute() below is just this plus resource allocation, barrier recording, and pass dispatch.
        [[nodiscard]] CompileResult compile() const;

        /// Lazy compile/record boundary. Until this is called, passes are declarations and transient
        /// textures are virtual: no GPU allocation or command recording occurs. Despite the historical
        /// name, execute() only compiles the dependency graph and records commands into `encoder`; queue
        /// submission remains asynchronous unless the high-level graph requests WaitForCompletion.
        /// `timestamp_query_set`/`out_pass_timings`: optional GPU per-pass timing. When
        /// `timestamp_query_set` is a valid Timestamp query set with at least `2 * compile()`'s pass
        /// count slots, execute() resets it and brackets every executed pass with a begin/end
        /// timestamp, appending a GpuPassTiming to `*out_pass_timings` per pass (cleared first). Pass
        /// an invalid handle (the default) to skip timing entirely — no queries written, no cost.
        /// `out_cpu_pass_timings`: optional CPU per-pass timing, independent of the GPU parameters
        /// above — pass a non-null pointer to have execute() clear it then append a CpuPassTiming
        /// (wall-clock steady_clock duration) per executed pass. No RHI cost either way; skip by
        /// leaving it null.
        [[nodiscard]] Core::RendererResult execute(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    RHI::QuerySetHandle timestamp_query_set = {},
                                                    vector<GpuPassTiming> *out_pass_timings = nullptr,
                                                    vector<CpuPassTiming> *out_cpu_pass_timings = nullptr);

        /// Parallel-recording counterpart to execute(). `primary_encoder` is a caller-created encoder
        /// that may already have work recorded into it (e.g. text-overlay/UI prep that a render-graph
        /// pass later reads) — this function takes ownership, finishes it as the first command buffer,
        /// then records every render-graph pass into its own fresh encoder — one per pass for a level
        /// with more than one mutually-independent pass (recorded concurrently via
        /// Async::Scheduler::spawn), a single encoder otherwise — appending every resulting handle to
        /// `out_command_buffers` in submission order (level by level, original declaration order within
        /// a level, primary_encoder always first). The caller passes the resulting span straight into
        /// RHI::SubmitDesc::command_buffers instead of calling finish() itself. `queue` picks which
        /// queue lane every newly created encoder targets (RHI::QueueLane{} — Graphics, lane 0 —
        /// matches what execute()'s caller-provided encoder is created with today). Every other
        /// parameter matches execute() exactly, including GPU/CPU per-pass timing semantics. On failure,
        /// every command buffer already appended to `out_command_buffers` is destroyed before returning
        /// (nothing orphaned in the RHI's command-buffer pool) and the vector is left empty.
        [[nodiscard]] Core::RendererResult execute_parallel(RHI::RhiDevice &device,
                                                             unique_ptr<RHI::CommandEncoder> primary_encoder,
                                                             RHI::QueueLane queue,
                                                             vector<RHI::CommandBufferHandle> &out_command_buffers,
                                                             RHI::QuerySetHandle timestamp_query_set = {},
                                                             vector<GpuPassTiming> *out_pass_timings = nullptr,
                                                             vector<CpuPassTiming> *out_cpu_pass_timings = nullptr);

        void destroy_transient_resources(RHI::RhiDevice &device) noexcept;

        /// Hands the created transient textures/views to the caller (appending to its vectors) and clears
        /// them from the graph, so a later destroy_transient_resources() is a no-op. This is the async
        /// model's handoff: once a frame is submitted, its transient targets must outlive the graph object
        /// and be destroyed only when the frame's fence retires — the caller owns that deferred cleanup.
        void take_transient_resources(vector<RHI::TextureHandle> &textures,
                                      vector<RHI::TextureViewHandle> &views);

        void reset() noexcept;

        /// First/last position a transient texture is read or written at, within a CompiledPlan's order
        /// — the input to interval-graph aliasing (see create_transient_resources() in RenderGraph.cpp).
        /// -1 means the texture was never used by any live pass, i.e. it gets no physical allocation at
        /// all. Public (not just an implementation detail) so a CPU-only test can confirm an unused
        /// create_texture() is correctly recognized as dead without needing an RHI device to observe
        /// that create_transient_resources() then skips allocating it.
        struct TextureLifetime {
            i32 first_use = -1;
            i32 last_use = -1;
        };
        [[nodiscard]] vector<TextureLifetime> compute_transient_lifetimes(const vector<OrderedPass> &execution_order) const;

        /// Level of each pass in `execution_order` (same length, same indexing — mirrors
        /// compute_transient_lifetimes' parallel-array style). Level N+1 passes depend, directly or
        /// transitively, on at least one level-N pass touching the same (physical-slot-aliased-aware)
        /// resource. Two passes never share a level if they touch the same texture or imported buffer at
        /// all — including a shared read — since transition_texture/transition_buffer
        /// decides on the fly which pass actually needs to emit a layout-transition barrier, and that
        /// decision isn't safe to race across command buffers whose relative submission order isn't
        /// known until after they're all recorded (see compute_execution_levels' own .cpp comment for
        /// the exact hazard this closes). Recording each level's passes into separate command buffers
        /// therefore needs no barrier between them, and finishing/submitting levels in order preserves
        /// whatever ordering the barriers within them depend on. Pure-CPU, same testability contract as
        /// compile()/compute_transient_lifetimes() above.
        ///
        /// This standalone entry point exists for testability (callable without re-running compile()).
        /// compile() computes logical-resource levels internally by reusing the PassUsage it already built.
        /// execute_parallel() calls this entry point again after create_transient_resources() so physical-
        /// slot alias conflicts, which do not exist during the CPU-only compile step, are included.
        [[nodiscard]] vector<u32> compute_execution_levels(const vector<OrderedPass> &execution_order) const;

      private:
        /// The actual GPU-visible backing for one or more virtual transient textures. Two virtual
        /// textures whose lifetimes don't overlap (and whose creation desc matches exactly) are assigned
        /// the same PhysicalSlot by create_transient_resources()'s aliasing pass, so layout/stage/access
        /// state is tracked per mip of the *physical* slot, not per virtual TextureRecord: it reflects
        /// real GPU state, which is shared whenever two virtual textures alias. Imported textures always
        /// get a dedicated, non-owning slot (owns_resource = false — the graph never destroys it).
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

        /// Imported buffers currently carry one conservative state for the whole allocation. Passes may
        /// declare ranges, but barriers remain whole-buffer until interval state tracking is added.
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
            /// Index into physical_slots_. Imported textures get one immediately in import_texture();
            /// transient textures are only assigned one once create_transient_resources() runs its
            /// aliasing pass, so this is ~0u (invalid) between create_texture() and execute().
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

        [[nodiscard]] TextureRecord *texture_record(RenderGraphTextureHandle handle) noexcept;

        [[nodiscard]] const TextureRecord *texture_record(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) noexcept;

        [[nodiscard]] const PhysicalSlot *physical_slot_for(RenderGraphTextureHandle handle) const noexcept;

        [[nodiscard]] BufferRecord *buffer_record(RenderGraphBufferHandle handle) noexcept;
        [[nodiscard]] const BufferRecord *buffer_record(RenderGraphBufferHandle handle) const noexcept;

        [[nodiscard]] Core::RendererResult transition_buffer(RHI::CommandEncoder &encoder,
                                                             const RenderGraphBufferAccessDesc &access);

        [[nodiscard]] Core::RendererResult transition_texture(RHI::CommandEncoder &encoder,
                                                              RenderGraphTextureHandle handle,
                                                              RHI::TextureLayout next_layout,
                                                              RHI::PipelineStage next_stage,
                                                              RHI::AccessFlags next_access,
                                                              RHI::TextureSubresourceRange subresources = {});

        /// Shared per-pass dispatch body for both execute() and execute_parallel(): writes the begin
        /// timestamp (if enabled), dispatches to the right execute_*_pass by PassKind, writes the end
        /// timestamp, and fills `out_gpu_timing`/`out_cpu_timing` when non-null. `begin_query_index` is
        /// precomputed by the caller (2 * the pass's position in execution_order) rather than a shared
        /// running counter, so it stays correct when passes execute out of order across threads.
        [[nodiscard]] Core::RendererResult execute_one_pass(RHI::CommandEncoder &encoder, const OrderedPass &ordered,
                                                             u32 begin_query_index, RHI::QuerySetHandle timestamp_query_set,
                                                             bool timing_enabled, bool cpu_timing_enabled,
                                                             GpuPassTiming *out_gpu_timing, CpuPassTiming *out_cpu_timing);

        [[nodiscard]] Core::RendererResult execute_render_pass(RHI::CommandEncoder &encoder,
                                                               RenderGraphRenderPassBuilder &pass);

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

        [[nodiscard]] Core::RendererResult execute_blit_pass(RHI::CommandEncoder &encoder, const RenderGraphBlitDesc &pass);

        [[nodiscard]] Core::RendererResult execute_compute_pass(RHI::CommandEncoder &encoder,
                                                                RenderGraphComputePassBuilder &pass);

        [[nodiscard]] Core::RendererResult execute_copy_pass(RHI::CommandEncoder &encoder, const RenderGraphCopyDesc &pass);

        /// Runs compile_execution_order()'s topo-sorted/culled order through interval-graph aliasing
        /// (see RenderGraph.cpp for the algorithm) before creating one physical GPU texture per resulting
        /// slot instead of one per virtual transient texture, then creates the GPU resources.
        [[nodiscard]] Core::RendererResult create_transient_resources(RHI::RhiDevice &device,
                                                                      const vector<OrderedPass> &execution_order);

        [[nodiscard]] Core::RendererResult transition_to_final_states(RHI::CommandEncoder &encoder);

        /// What one pass reads from and writes to, separated into texture and buffer handles — the input
        /// to compile()'s dependency analysis. `always_live` covers a pass with no
        /// declared attachments at all (doesn't happen from any call site today, but nothing stops
        /// one existing): the graph can't reason about a side effect it never declared, so such a
        /// pass is never culled. Member functions (not free functions) purely so they can read
        /// RenderGraphRenderPassBuilder's private fields via its existing `friend class RenderGraph`.
        struct PassUsage {
            vector<RenderGraphTextureHandle> writes;
            vector<RenderGraphTextureHandle> reads;
            vector<RenderGraphBufferHandle> buffer_writes;
            vector<RenderGraphBufferHandle> buffer_reads;
            bool always_live = false;
        };
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphRenderPassBuilder &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphBlitDesc &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphComputePassBuilder &pass);
        [[nodiscard]] static PassUsage pass_usage_of(const RenderGraphCopyDesc &pass);
        [[nodiscard]] PassUsage usage_of_ordered(const OrderedPass &ordered) const;

        /// Shared core of compute_execution_levels()/compile()'s internal level computation — see
        /// compute_execution_levels()'s own doc comment for the algorithm. Takes already-built usage
        /// (one entry per position in whatever pass sequence is being leveled) so the two callers can
        /// supply it however is cheapest for them: compute_execution_levels() builds it fresh via
        /// usage_of_ordered(), compile() moves its own already-computed PassUsage array into place.
        [[nodiscard]] vector<u32> compute_levels_from_usage(const vector<PassUsage> &usage_by_position) const;

        vector<TextureRecord> textures_;
        vector<PhysicalSlot> physical_slots_;
        vector<BufferRecord> buffers_;
        /// transition_texture()'s read-decide-barrier-update of a PhysicalSlot's mip_states needs no
        /// lock: compute_execution_levels() guarantees any two passes sharing a level never touch the
        /// same physical texture slot or imported buffer at all — read or write, an earlier version of this comment only accounted
        /// for writes, which was the actual bug that motivated adding (and, once the level algorithm
        /// was fixed to cover reads too, later removing) a transition_lock_ here. Also makes RenderGraph
        /// movable again, needed for it to live as a WindowSurfaceRecord member (reused frame-to-frame
        /// instead of a fresh stack-local — see that field's own doc comment) rather than a std::mutex-
        /// like type that would delete WindowSurfaceRecord's move constructor.
        vector<OrderedPass> ordered_passes_;
        vector<RenderGraphRenderPassBuilder> render_passes_;
        vector<RenderGraphBlitDesc> blit_passes_;
        vector<RenderGraphComputePassBuilder> compute_passes_;
        vector<RenderGraphCopyDesc> copy_passes_;
        vector<RenderGraphTextureHandle> outputs_;
    };









} // namespace SFT::Renderer
