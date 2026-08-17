#pragma once


#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <array>
#include <optional>
#include <vector>
#pragma endregion

namespace SFT::D3D12 {


    inline constexpr u32 max_tracked_bind_groups = 8;


    struct PendingBindGroup {
        rhi::BindGroupHandle handle{};
        std::vector<u32> dynamic_offsets;
        bool dirty = false;
    };


    struct BindingState {
        rhi::PipelineLayoutHandle layout{};
        std::array<PendingBindGroup, max_tracked_bind_groups> groups{};
        std::vector<std::byte> push_constants;
        bool push_constants_dirty = false;
        bool layout_dirty = false;

        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;
    };

    class D3D12CommandEncoder final : public rhi::CommandEncoder {
      public:
        /// Constructs a `D3D12CommandEncoder` from the supplied initialization values.
        ///
        /// @param device Device used or affected by the operation.
        /// @param record `record` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        D3D12CommandEncoder(D3D12Device &device, CommandBufferRecord &&record);
        /// Destroys the `D3D12CommandEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~D3D12CommandEncoder() override;

        /// Performs the begin render pass operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> begin_render_pass(
            const rhi::RenderPassDesc &desc) override;
        /// Performs the begin compute pass operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> begin_compute_pass(
            const rhi::ComputePassDesc &desc) override;

        /// Copies buffer to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst, const rhi::BufferCopy &region) override;
        /// Copies buffer to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst, const rhi::BufferTextureCopy &region) override;
        /// Copies texture to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst, const rhi::BufferTextureCopy &region) override;
        /// Copies texture to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureCopy &region) override;
        /// Performs the blit texture operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        /// @param filter `filter` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void blit_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureBlit &region, rhi::Filter filter) override;

        /// Fills buffer using the supplied arguments and current state.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void fill_buffer(rhi::BufferHandle buffer, u64 offset, u64 size, u32 value) override;
        /// Updates buffer from the supplied values.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        /// Clears color texture.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param color `color` value used by the operation.
        /// @param range Range of values to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void clear_color_texture(rhi::TextureHandle texture, const rhi::ClearColor &color, const rhi::TextureSubresourceRange &range) override;
        /// Clears depth stencil texture.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param value Value consumed by the operation.
        /// @param range Range of values to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void clear_depth_stencil_texture(rhi::TextureHandle texture, const rhi::ClearDepthStencil &value, const rhi::TextureSubresourceRange &range) override;

        /// Builds acceleration structures.
        ///
        /// @param builds `builds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_acceleration_structures(span<const rhi::AccelerationStructureBuildDesc> builds) override;
        /// Copies acceleration structure to its destination.
        ///
        /// @param copy `copy` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) override;
        /// Traces rays using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void trace_rays(const rhi::TraceRaysDesc &desc) override;

        /// Performs the barrier operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param global_barriers `global_barriers` value used by the operation.
        /// @param buffer_barriers Buffer used or affected by the operation.
        /// @param texture_barriers Texture used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void barrier(span<const rhi::GlobalBarrier> global_barriers, span<const rhi::BufferBarrier> buffer_barriers, span<const rhi::TextureBarrier> texture_barriers) override;

        /// Resets query set to its baseline state.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) override;
        /// Writes timestamp to the associated destination.
        ///
        /// @param stage `stage` value used by the operation.
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set, u32 index) override;
        /// Performs the begin pipeline statistics query operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) override;
        /// Performs the end pipeline statistics query operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end_pipeline_statistics_query() override;
        /// Resolves query set into the concrete value used by the engine.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        /// @param dst Destination value or resource.
        /// @param dst_offset Offset from the beginning of the relevant range or buffer.
        /// @param stride `stride` value used by the operation.
        /// @param flags Flags controlling optional behavior.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        void resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count, rhi::BufferHandle dst, u64 dst_offset, u64 stride, rhi::QueryResultFlags flags) override;

        /// Adds the supplied value to the end or work queue.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void push_debug_group(const char *label) override;
        /// Removes and returns or discards the next value from the container or queue.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void pop_debug_group() override;

        /// Returns the current or globally available finish value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::CommandBufferHandle> finish() override;

      private:
        friend class D3D12RenderPassEncoder;
        friend class D3D12ComputePassEncoder;


        /// Binds descriptor heaps for subsequent operations.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void bind_descriptor_heaps();


        struct BoundTables {
            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> resource_table;
            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> sampler_table;
        };
        /// Uploads bind group using the supplied arguments and current state.
        ///
        /// @param group `group` value used by the operation.
        /// @param layout `layout` value used by the operation.
        /// @param allow_heap_swap `allow_heap_swap` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] std::optional<BoundTables> upload_bind_group(const BindGroupRecord &group,
                                                                   const BindGroupLayoutRecord &layout,
                                                                   bool allow_heap_swap);


        /// Flushes bindings.
        ///
        /// @param state `state` value used by the operation.
        /// @param graphics `graphics` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool flush_bindings(BindingState &state, bool graphics);

        /// Reports whether record outside pass holds for this `D3D12CommandEncoder`.
        ///
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool can_record_outside_pass(const char *operation);
        /// Creates a transient upload from the supplied parameters.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<ComPtr<ID3D12Resource>> create_transient_upload(
            span<const std::byte> data, const char *operation);


        /// Performs the legacy transition operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param subresource `subresource` value used by the operation.
        /// @param after `after` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void legacy_transition(TextureRecord &texture, u32 subresource, D3D12_RESOURCE_STATES after);


        /// Performs the fail operation for `D3D12CommandEncoder` using the supplied arguments.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        void fail(std::string message) noexcept;

        D3D12Device *device_ = nullptr;
        CommandBufferRecord record_{};
        ID3D12GraphicsCommandList *list_ = nullptr;


        ComPtr<ID3D12GraphicsCommandList7> list7_;

        ComPtr<ID3D12GraphicsCommandList4> list4_;

        ComPtr<ID3D12GraphicsCommandList6> list6_;

        BindingState graphics_bindings_{};
        BindingState compute_bindings_{};


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

        /// Constructs a `D3D12RenderPassEncoder` from the supplied initialization values.
        ///
        /// @param parent `parent` value used by the operation.
        /// @param bundles_only `bundles_only` value used by the operation.
        /// @param color_resolves `color_resolves` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        D3D12RenderPassEncoder(D3D12CommandEncoder &parent, bool bundles_only, std::vector<ColorResolve> color_resolves);
        /// Destroys the `D3D12RenderPassEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~D3D12RenderPassEncoder() override;

        /// Sets the pipeline for this `D3D12RenderPassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_pipeline(rhi::RenderPipelineHandle pipeline) override;
        /// Sets the bind group for this `D3D12RenderPassEncoder`.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets) override;
        /// Sets the vertex buffer for this `D3D12RenderPassEncoder`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) override;
        /// Sets the index buffer for this `D3D12RenderPassEncoder`.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset) override;
        /// Sets the push constants for this `D3D12RenderPassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        /// Sets the viewport for this `D3D12RenderPassEncoder`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_viewport(const rhi::Viewport &viewport) override;
        /// Sets the scissor for this `D3D12RenderPassEncoder`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_scissor(const rhi::Rect2D &scissor) override;
        /// Sets the blend constant for this `D3D12RenderPassEncoder`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_blend_constant(const rhi::ClearColor &color) override;
        /// Sets the stencil reference for this `D3D12RenderPassEncoder`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_stencil_reference(u32 reference) override;

        /// Draws the requested content using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw(const rhi::DrawArgs &args) override;
        /// Draws indexed using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed(const rhi::DrawIndexedArgs &args) override;
        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) override;
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) override;
        /// Returns the draw indirect count for this `D3D12RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;
        /// Returns the draw indexed indirect count for this `D3D12RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        /// Returns the draw mesh tasks indirect count for this `D3D12RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride) override;

        /// Executes bundles.
        ///
        /// @param bundles `bundles` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void execute_bundles(span<const rhi::RenderBundleHandle> bundles) override;
        /// Performs the begin occlusion query operation for `D3D12RenderPassEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_occlusion_query(rhi::QuerySetHandle query_set, u32 index) override;
        /// Performs the end occlusion query operation for `D3D12RenderPassEncoder` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end_occlusion_query() override;
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end() override;

      private:
        struct PendingVertexBuffer {
            rhi::BufferHandle buffer{};
            u64 offset = 0;
        };

        /// Binds vertex buffer for subsequent operations.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void bind_vertex_buffer(u32 slot);
        /// Records indirect using the supplied arguments and current state.
        ///
        /// @param kind `kind` value used by the operation.
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void record_indirect(D3D12Device::IndirectKind kind, rhi::BufferHandle indirect_buffer, u64 offset, rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws, u32 stride);

        D3D12CommandEncoder *parent_ = nullptr;
        std::array<PendingVertexBuffer, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_{};
        vector<u32> vertex_strides_;
        bool pipeline_bound_ = false;
        bool mesh_pipeline_bound_ = false;
        std::vector<ColorResolve> color_resolves_;


        bool bundles_only_ = false;
        bool ended_ = false;

        rhi::QuerySetHandle occlusion_query_set_{};
        u32 occlusion_query_index_ = 0;
    };

    class D3D12ComputePassEncoder final : public rhi::ComputePassEncoder {
      public:
        /// Constructs a `D3D12ComputePassEncoder` from the supplied initialization values.
        ///
        /// @param parent `parent` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit D3D12ComputePassEncoder(D3D12CommandEncoder &parent);
        /// Destroys the `D3D12ComputePassEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~D3D12ComputePassEncoder() override;

        /// Sets the pipeline for this `D3D12ComputePassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_pipeline(rhi::ComputePipelineHandle pipeline) override;
        /// Sets the bind group for this `D3D12ComputePassEncoder`.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets) override;
        /// Sets the push constants for this `D3D12ComputePassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        /// Dispatches the requested work.
        ///
        /// @param group_count_x `group_count_x` value used by the operation.
        /// @param group_count_y `group_count_y` value used by the operation.
        /// @param group_count_z `group_count_z` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) override;
        /// Dispatches indirect.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void dispatch_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void end() override;

      private:
        D3D12CommandEncoder *parent_ = nullptr;
        bool ended_ = false;
    };

} // namespace SFT::D3D12
