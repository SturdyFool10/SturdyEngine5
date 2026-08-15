#pragma once

// Command recording. One `D3D12CommandEncoder` owns an ID3D12GraphicsCommandList and hands out
// render/compute pass encoders that record into that same list — D3D12 has no separate pass object,
// so a pass is a scope in this backend rather than an API object.
//
// ─── Binding state, and why it is tracked here ───────────────────────────────────────────────────
//
// The RHI's set_bind_group() is "bind this group at set index N". Realizing that on D3D12 takes three
// steps: copy the group's authoritative CPU descriptors into the command list's shader-visible heap,
// point the matching root descriptor table at the copy, and write any dynamic-offset bindings as root
// descriptors. All three depend on the *currently bound pipeline layout*, which the caller may set
// after the bind group (the RHI permits either order). So binds are recorded into `BindingState` and
// flushed at draw/dispatch time, once both the layout and the groups are known — the same
// deferred-flush shape every D3D12 backend converges on, and the only one that makes both orders work.
//
// It also makes shader-visible heap exhaustion recoverable: if the heap runs out mid-list, the
// encoder swaps in a fresh one and re-uploads every currently bound group from its CPU staging copy
// (a table's GPU handle is only valid while its heap is the bound one), which is possible precisely
// because the state is retained rather than consumed at bind time.

#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <array>
#include <optional>
#include <vector>
#pragma endregion

namespace SFT::D3D12 {

    // The maximum number of bind group indices an encoder tracks. Matches the `max_bind_groups` limit
    // the adapter reports, so a caller respecting the limit can never overflow this.
    inline constexpr u32 max_tracked_bind_groups = 8;

    // One set index's pending binding, retained until the next draw/dispatch flushes it.
    struct PendingBindGroup {
        rhi::BindGroupHandle handle{};
        std::vector<u32> dynamic_offsets;
        bool dirty = false;
    };

    // Everything a flush needs, shared by the render and compute paths (they differ only in which
    // SetGraphicsRoot*/SetComputeRoot* family they call, which is why this is one struct and one
    // flush function parameterized by a bool rather than two of each).
    struct BindingState {
        rhi::PipelineLayoutHandle layout{};
        std::array<PendingBindGroup, max_tracked_bind_groups> groups{};
        std::vector<std::byte> push_constants;
        bool push_constants_dirty = false;
        bool layout_dirty = false;

        void reset() noexcept;
    };

    class D3D12CommandEncoder final : public rhi::CommandEncoder {
      public:
        D3D12CommandEncoder(D3D12Device &device, CommandBufferRecord &&record);
        ~D3D12CommandEncoder() override;

        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> begin_render_pass(
            const rhi::RenderPassDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> begin_compute_pass(
            const rhi::ComputePassDesc &desc) override;

        void copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst, const rhi::BufferCopy &region) override;
        void copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst, const rhi::BufferTextureCopy &region) override;
        void copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst, const rhi::BufferTextureCopy &region) override;
        void copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureCopy &region) override;
        void blit_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureBlit &region, rhi::Filter filter) override;

        void fill_buffer(rhi::BufferHandle buffer, u64 offset, u64 size, u32 value) override;
        void update_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        void clear_color_texture(rhi::TextureHandle texture, const rhi::ClearColor &color, const rhi::TextureSubresourceRange &range) override;
        void clear_depth_stencil_texture(rhi::TextureHandle texture, const rhi::ClearDepthStencil &value, const rhi::TextureSubresourceRange &range) override;

        void build_acceleration_structures(span<const rhi::AccelerationStructureBuildDesc> builds) override;
        void copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) override;
        void trace_rays(const rhi::TraceRaysDesc &desc) override;

        void barrier(span<const rhi::GlobalBarrier> global_barriers, span<const rhi::BufferBarrier> buffer_barriers, span<const rhi::TextureBarrier> texture_barriers) override;

        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) override;
        void write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set, u32 index) override;
        void begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) override;
        void end_pipeline_statistics_query() override;
        void resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count, rhi::BufferHandle dst, u64 dst_offset, u64 stride, rhi::QueryResultFlags flags) override;

        void push_debug_group(const char *label) override;
        void pop_debug_group() override;

        [[nodiscard]] rhi::RhiExpected<rhi::CommandBufferHandle> finish() override;

      private:
        friend class D3D12RenderPassEncoder;
        friend class D3D12ComputePassEncoder;

        // Ensures the shader-visible heaps are bound to the list. Idempotent — called at the start of
        // every pass rather than once, because SetDescriptorHeaps has to be re-issued after a heap swap.
        void bind_descriptor_heaps();

        // Copies `group`'s descriptors into the list's shader-visible heaps and returns the resulting
        // GPU handles. `nullopt` on heap exhaustion after a swap has already been attempted.
        struct BoundTables {
            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> resource_table;
            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> sampler_table;
        };
        [[nodiscard]] std::optional<BoundTables> upload_bind_group(const BindGroupRecord &group,
                                                                   const BindGroupLayoutRecord &layout,
                                                                   bool allow_heap_swap);

        // Applies `state` to the list's root arguments. `graphics` selects the
        // SetGraphicsRoot*/SetComputeRoot* family; D3D12 keeps two entirely separate root-argument sets,
        // so a compute dispatch cannot see anything a graphics bind wrote and vice versa. False means
        // validation or descriptor upload failed and the caller must not emit its draw/dispatch.
        [[nodiscard]] bool flush_bindings(BindingState &state, bool graphics);

        [[nodiscard]] bool can_record_outside_pass(const char *operation);
        [[nodiscard]] rhi::RhiExpected<ComPtr<ID3D12Resource>> create_transient_upload(
            span<const std::byte> data, const char *operation);

        // Records the current legacy state of `subresource` and emits a transition when the caller's
        // stated old/new layouts disagree with it. Only reached on the pre-enhanced-barrier path.
        void legacy_transition(TextureRecord &texture, u32 subresource, D3D12_RESOURCE_STATES after);

        // Reports a recording-time error. Encoders return void from every record call (the RHI defers
        // failure to finish()), so an error is latched here and surfaced by finish().
        void fail(std::string message) noexcept;

        D3D12Device *device_ = nullptr;
        CommandBufferRecord record_{};
        ID3D12GraphicsCommandList *list_ = nullptr;
        // The enhanced-barrier interface, or null when the device reported no support — which is
        // exactly the condition that selects the legacy ResourceBarrier path.
        ComPtr<ID3D12GraphicsCommandList7> list7_;
        // ID3D12GraphicsCommandList4: BuildRaytracingAccelerationStructure/DispatchRays/BeginRenderPass.
        ComPtr<ID3D12GraphicsCommandList4> list4_;
        // ID3D12GraphicsCommandList6: DispatchMesh.
        ComPtr<ID3D12GraphicsCommandList6> list6_;

        BindingState graphics_bindings_{};
        BindingState compute_bindings_{};
        // Set while a render or compute pass is open, so a copy/barrier recorded inside one — which
        // D3D12 forbids inside a BeginRenderPass scope and the RHI forbids by contract — is reported
        // rather than silently mis-recorded.
        bool pass_open_ = false;
        rhi::QuerySetHandle statistics_query_set_{};
        u32 statistics_query_index_ = 0;
        bool finished_ = false;
        std::optional<rhi::RhiError> deferred_error_;
    };

    class D3D12RenderPassEncoder final : public rhi::RenderPassEncoder {
      public:
        struct ColorResolve {
            ID3D12Resource *source = nullptr;
            ID3D12Resource *destination = nullptr;
            u32 source_subresource = 0;
            u32 destination_subresource = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        };

        D3D12RenderPassEncoder(D3D12CommandEncoder &parent, bool bundles_only, std::vector<ColorResolve> color_resolves);
        ~D3D12RenderPassEncoder() override;

        void set_pipeline(rhi::RenderPipelineHandle pipeline) override;
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets) override;
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) override;
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset) override;
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        void set_viewport(const rhi::Viewport &viewport) override;
        void set_scissor(const rhi::Rect2D &scissor) override;
        void set_blend_constant(const rhi::ClearColor &color) override;
        void set_stencil_reference(u32 reference) override;

        void draw(const rhi::DrawArgs &args) override;
        void draw_indexed(const rhi::DrawIndexedArgs &args) override;
        void draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) override;
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override;
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override;
        void draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;
        void draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;
        void draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        void draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;

        void execute_bundles(span<const rhi::RenderBundleHandle> bundles) override;
        void begin_occlusion_query(rhi::QuerySetHandle query_set, u32 index) override;
        void end_occlusion_query() override;
        void end() override;

      private:
        struct PendingVertexBuffer {
            rhi::BufferHandle buffer{};
            u64 offset = 0;
        };

        void bind_vertex_buffer(u32 slot);
        void record_indirect(D3D12Device::IndirectKind kind, rhi::BufferHandle indirect_buffer, u64 offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride);

        D3D12CommandEncoder *parent_ = nullptr;
        std::array<PendingVertexBuffer, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_{};
        vector<u32> vertex_strides_;
        bool pipeline_bound_ = false;
        bool mesh_pipeline_bound_ = false;
        std::vector<ColorResolve> color_resolves_;
        // RenderPassDesc::allow_bundles — a pass opened this way records only through bundles, mirroring
        // Vulkan's INLINE-vs-SECONDARY split. Tracked so an inline draw in a bundles-only pass is caught.
        bool bundles_only_ = false;
        bool ended_ = false;
        // The query set an occlusion scope was opened on, for the matching EndQuery.
        rhi::QuerySetHandle occlusion_query_set_{};
        u32 occlusion_query_index_ = 0;
    };

    class D3D12ComputePassEncoder final : public rhi::ComputePassEncoder {
      public:
        explicit D3D12ComputePassEncoder(D3D12CommandEncoder &parent);
        ~D3D12ComputePassEncoder() override;

        void set_pipeline(rhi::ComputePipelineHandle pipeline) override;
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets) override;
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        void dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) override;
        void dispatch_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        void end() override;

      private:
        D3D12CommandEncoder *parent_ = nullptr;
        bool ended_ = false;
    };

} // namespace SFT::D3D12
