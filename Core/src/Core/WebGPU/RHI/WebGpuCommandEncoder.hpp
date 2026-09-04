#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <memory>
#include <span>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/WebGPU/RHI/WebGpuCommon.hpp>

namespace SFT::Core::WebGpu {

    namespace rhi = ::SFT::RHI;

    using std::span;
    using std::unique_ptr;

    class WebGpuDevice;

    /// Size of each encoder's push-constant shadow block, matching
    /// `WebGpuDevice::push_constant_block_size`. Declared here rather than taken from the device so
    /// this header does not have to include WebGpuDevice.hpp; a static_assert in the .cpp keeps the
    /// two from drifting apart.
    inline constexpr usize push_constant_shadow_size = 256;

    /// Records draw calls into a WebGPU render pass.
    ///
    /// WebGPU's render pass is closer to the RHI's than Vulkan's is: attachments, load/store
    /// actions, and clear values are all given up front, and there are no subpasses. What it does
    /// not have is per-draw state the RHI treats as dynamic on the other backends — push constants
    /// and depth bounds have no WebGPU form at all. Those report an error through the log rather
    /// than silently doing nothing, because the interface returns void and a caller relying on them
    /// would otherwise render wrong output with no indication why.
    class WebGpuRenderPassEncoder final : public rhi::RenderPassEncoder {
      public:
        /// Constructs an encoder around a live WebGPU render pass.
        ///
        /// @param device The device the pass belongs to, used to resolve handles.
        /// @param pass The WebGPU render pass encoder, whose ownership passes to this object.
        ///
        /// @note This function does not throw exceptions.
        WebGpuRenderPassEncoder(WebGpuDevice &device, WGPURenderPassEncoder pass) noexcept;

        /// Destroys the encoder, releasing the pass if `end` was never called.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuRenderPassEncoder() override;

        void set_pipeline(rhi::RenderPipelineHandle pipeline) override;
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override;
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset = 0) override;
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset = 0) override;
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        void set_viewport(const rhi::Viewport &viewport) override;
        void set_scissor(const rhi::Rect2D &scissor) override;
        void set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size, span<const rhi::SampleLocation> locations) override;
        void set_blend_constant(const rhi::ClearColor &color) override;
        void set_stencil_reference(u32 reference) override;
        void set_depth_bounds(f32 min_depth, f32 max_depth) override;
        void set_shading_rate(rhi::ShadingRate rate, rhi::ShadingRateCombiner primitive_combiner, rhi::ShadingRateCombiner attachment_combiner) override;
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
        WebGpuDevice &device_;
        WGPURenderPassEncoder pass_ = nullptr;
        // Push constants have no WebGPU form; they are emulated as a dynamic-offset uniform buffer
        // bound at a reserved group (see WebGpuDevicePushConstants.cpp). Because a caller may set
        // only part of the block and expect the rest to persist -- which is exactly what a push
        // constant does on the other backends -- the whole block is mirrored here and uploaded in
        // full on each set.
        std::array<std::byte, push_constant_shadow_size> push_constants_{};
    };

    /// Records draw calls into a reusable WebGPU render bundle.
    class WebGpuRenderBundleEncoder final : public rhi::RenderBundleEncoder {
      public:
        /// Constructs an encoder around a live WebGPU render bundle encoder.
        ///
        /// @param device The device the bundle belongs to, used to resolve handles.
        /// @param encoder The WebGPU bundle encoder, whose ownership passes to this object.
        ///
        /// @note This function does not throw exceptions.
        WebGpuRenderBundleEncoder(WebGpuDevice &device, WGPURenderBundleEncoder encoder) noexcept;

        /// Destroys the encoder, releasing it if `finish` was never called.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuRenderBundleEncoder() override;

        void set_pipeline(rhi::RenderPipelineHandle pipeline) override;
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override;
        void set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset = 0) override;
        void set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset = 0) override;
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        void set_viewport(const rhi::Viewport &viewport) override;
        void set_scissor(const rhi::Rect2D &scissor) override;
        void set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size, span<const rhi::SampleLocation> locations) override;
        void set_blend_constant(const rhi::ClearColor &color) override;
        void set_stencil_reference(u32 reference) override;
        void set_depth_bounds(f32 min_depth, f32 max_depth) override;
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
        [[nodiscard]] rhi::RhiExpected<rhi::RenderBundleHandle> finish() override;

      private:
        WebGpuDevice &device_;
        WGPURenderBundleEncoder encoder_ = nullptr;
        // Push constants have no WebGPU form; they are emulated as a dynamic-offset uniform buffer
        // bound at a reserved group (see WebGpuDevicePushConstants.cpp). Because a caller may set
        // only part of the block and expect the rest to persist -- which is exactly what a push
        // constant does on the other backends -- the whole block is mirrored here and uploaded in
        // full on each set.
        std::array<std::byte, push_constant_shadow_size> push_constants_{};
    };

    /// Records dispatches into a WebGPU compute pass.
    class WebGpuComputePassEncoder final : public rhi::ComputePassEncoder {
      public:
        /// Constructs an encoder around a live WebGPU compute pass.
        ///
        /// @param device The device the pass belongs to, used to resolve handles.
        /// @param pass The WebGPU compute pass encoder, whose ownership passes to this object.
        ///
        /// @note This function does not throw exceptions.
        WebGpuComputePassEncoder(WebGpuDevice &device, WGPUComputePassEncoder pass) noexcept;

        /// Destroys the encoder, releasing the pass if `end` was never called.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuComputePassEncoder() override;

        void set_pipeline(rhi::ComputePipelineHandle pipeline) override;
        void set_bind_group(u32 index, rhi::BindGroupHandle bind_group, span<const u32> dynamic_offsets = {}) override;
        void set_push_constants(rhi::ShaderStage stages, u32 offset, span<const std::byte> data) override;
        void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) override;
        void dispatch_indirect(rhi::BufferHandle indirect_buffer, u64 offset) override;
        void end() override;

      private:
        WebGpuDevice &device_;
        WGPUComputePassEncoder pass_ = nullptr;
        // Push constants have no WebGPU form; they are emulated as a dynamic-offset uniform buffer
        // bound at a reserved group (see WebGpuDevicePushConstants.cpp). Because a caller may set
        // only part of the block and expect the rest to persist -- which is exactly what a push
        // constant does on the other backends -- the whole block is mirrored here and uploaded in
        // full on each set.
        std::array<std::byte, push_constant_shadow_size> push_constants_{};
    };

    /// Records copies, passes, and queries into a WebGPU command encoder.
    class WebGpuCommandEncoder final : public rhi::CommandEncoder {
      public:
        /// Constructs an encoder around a live WebGPU command encoder.
        ///
        /// @param device The device the encoder belongs to, used to resolve handles.
        /// @param encoder The WebGPU command encoder, whose ownership passes to this object.
        ///
        /// @note This function does not throw exceptions.
        WebGpuCommandEncoder(WebGpuDevice &device, WGPUCommandEncoder encoder) noexcept;

        /// Destroys the encoder, releasing it if `finish` was never called.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuCommandEncoder() override;

        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> begin_render_pass(const rhi::RenderPassDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> begin_compute_pass(const rhi::ComputePassDesc &desc) override;
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
        void build_opacity_micromaps(span<const rhi::OpacityMicromapBuildDesc> builds) override;
        void copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) override;
        void set_ray_tracing_pipeline(rhi::RayTracingPipelineHandle pipeline) override;
        void trace_rays(const rhi::TraceRaysDesc &desc) override;
        void barrier(span<const rhi::GlobalBarrier> global_barriers, span<const rhi::BufferBarrier> buffer_barriers, span<const rhi::TextureBarrier> texture_barriers) override;
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) override;
        void write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set, u32 index) override;
        void begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) override;
        void end_pipeline_statistics_query() override;
        void resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count, rhi::BufferHandle dst, u64 dst_offset, u64 stride, rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
        void push_debug_group(const char *label) override;
        void pop_debug_group() override;
        [[nodiscard]] rhi::RhiExpected<rhi::CommandBufferHandle> finish() override;

      private:
        /// Runs `body` once per mip level and array layer named by `range`, handing it a view of
        /// that one subresource.
        ///
        /// WebGPU clears through a render pass, and a render pass attaches exactly one subresource,
        /// so a range clear is a loop rather than a single command.
        ///
        /// @param target The texture being cleared.
        /// @param format Format the subresource views are created with.
        /// @param mip_levels Total mip levels the texture has.
        /// @param layer_total Total array layers the texture has.
        /// @param range Subresource range the caller asked for.
        /// @param body Invoked with each subresource's view, which is released afterwards.
        ///
        /// @note This function does not throw exceptions.
        template <typename Body>
        void for_each_subresource(WGPUTexture target, rhi::Format format, u32 mip_levels, u32 layer_total,
                                  const rhi::TextureSubresourceRange &range, Body &&body) noexcept;

        /// Begins and immediately ends a render pass, which is how a clear-only pass is expressed:
        /// the load op does the work and there is nothing to draw.
        ///
        /// @param pass_desc The pass to open and close.
        ///
        /// @note This function does not throw exceptions.
        void end_empty_render_pass(const WGPURenderPassDescriptor &pass_desc) noexcept;

        WebGpuDevice &device_;
        WGPUCommandEncoder encoder_ = nullptr;
    };

} // namespace SFT::Core::WebGpu
