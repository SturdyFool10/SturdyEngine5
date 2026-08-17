#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <memory>
#include <span>
#pragma endregion

#include "Error.hpp"
#include "Types.hpp"
#include "Handles.hpp"
#include "Shader.hpp"
#include "Resources.hpp"
#include "Queues.hpp"
#include "Barrier.hpp"
#include "Queries.hpp"
#include "RayTracing.hpp"

using std::span;
using std::unique_ptr;

namespace SFT::RHI {


    enum class LoadOp : u32 {
        Load,
        Clear,
        DontCare,
    };


    enum class StoreOp : u32 {
        Store,
        DontCare,
    };

    struct ColorAttachment {
        TextureViewHandle view{};


        TextureViewHandle resolve_view{};
        LoadOp load_op = LoadOp::Clear;
        StoreOp store_op = StoreOp::Store;
        ClearColor clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct DepthStencilAttachment {
        TextureViewHandle view{};


        TextureViewHandle resolve_view{};
        ResolveMode depth_resolve_mode = ResolveMode::SampleZero;
        LoadOp depth_load_op = LoadOp::Clear;
        StoreOp depth_store_op = StoreOp::Store;
        LoadOp stencil_load_op = LoadOp::DontCare;
        StoreOp stencil_store_op = StoreOp::DontCare;
        ClearDepthStencil clear_value{};
    };


    struct RenderPassDesc {
        span<const ColorAttachment> color_attachments;
        DepthStencilAttachment depth_stencil{};

        Rect2D render_area{};


        u32 view_mask = 0;


        bool allow_bundles = false;
        const char *label = nullptr;
    };


    struct RenderBundleDesc {
        span<const Format> color_formats;
        Format depth_stencil_format = Format::Undefined;
        SampleCount samples = SampleCount::X1;


        u32 view_mask = 0;
        const char *label = nullptr;
    };

    struct ComputePassDesc {
        const char *label = nullptr;
    };


    struct BufferCopy {
        u64 src_offset = 0;
        u64 dst_offset = 0;
        u64 size = 0;
    };


    struct BufferTextureCopy {
        u64 buffer_offset = 0;
        u32 buffer_row_length = 0;
        u32 buffer_image_height = 0;
        u32 mip_level = 0;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;
        Offset3D texture_offset{};
        Extent3D texture_extent{};
    };


    struct TextureSubresourceLayers {
        u32 mip_level = 0;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;
    };


    struct TextureCopy {
        TextureSubresourceLayers src_subresource{};
        Offset3D src_offset{};
        TextureSubresourceLayers dst_subresource{};
        Offset3D dst_offset{};
        Extent3D extent{};
    };


    struct TextureBlit {
        TextureSubresourceLayers src_subresource{};
        Offset3D src_min{};
        Offset3D src_max{};
        TextureSubresourceLayers dst_subresource{};
        Offset3D dst_min{};
        Offset3D dst_max{};
    };


    struct DrawArgs {
        u32 vertex_count = 0;
        u32 instance_count = 1;
        u32 first_vertex = 0;
        u32 first_instance = 0;
    };

    struct DrawIndexedArgs {
        u32 index_count = 0;
        u32 instance_count = 1;
        u32 first_index = 0;
        i32 base_vertex = 0;
        u32 first_instance = 0;
    };

    struct DrawMeshTasksArgs {
        u32 group_count_x = 1;
        u32 group_count_y = 1;
        u32 group_count_z = 1;
    };


    class RenderPassEncoder {
      public:
        /// Destroys the `RenderPassEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~RenderPassEncoder() = default;

        /// Sets the pipeline for this `RenderPassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_pipeline(RenderPipelineHandle pipeline) = 0;


        /// Sets the bind group for this `RenderPassEncoder`.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;

        /// Sets the vertex buffer for this `RenderPassEncoder`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_vertex_buffer(u32 slot, BufferHandle buffer, u64 offset = 0) = 0;
        /// Sets the index buffer for this `RenderPassEncoder`.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset = 0) = 0;


        /// Sets the push constants for this `RenderPassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;

        /// Sets the viewport for this `RenderPassEncoder`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_viewport(const Viewport &viewport) = 0;
        /// Sets the scissor for this `RenderPassEncoder`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_scissor(const Rect2D &scissor) = 0;

        /// Sets the blend constant for this `RenderPassEncoder`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_blend_constant(const ClearColor &color) = 0;
        /// Sets the stencil reference for this `RenderPassEncoder`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_stencil_reference(u32 reference) = 0;

        /// Draws the requested content using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw(const DrawArgs &args) = 0;
        /// Draws indexed using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed(const DrawIndexedArgs &args) = 0;


        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks(const DrawMeshTasksArgs &args) = 0;


        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset) = 0;


        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;


        /// Returns the draw indirect count for this `RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                         BufferHandle count_buffer, u64 count_offset,
                                         u32 max_draws, u32 stride) = 0;
        /// Returns the draw indexed indirect count for this `RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                 BufferHandle count_buffer, u64 count_offset,
                                                 u32 max_draws, u32 stride) = 0;
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        /// Returns the draw mesh tasks indirect count for this `RenderPassEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                    BufferHandle count_buffer, u64 count_offset,
                                                    u32 max_draws, u32 stride) = 0;


        /// Executes bundles.
        ///
        /// @param bundles `bundles` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void execute_bundles(span<const RenderBundleHandle> bundles) = 0;


        /// Performs the begin occlusion query operation for `RenderPassEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void begin_occlusion_query(QuerySetHandle query_set, u32 index) = 0;
        /// Performs the end occlusion query operation for `RenderPassEncoder` using the supplied arguments.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void end_occlusion_query() = 0;


        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void end() = 0;
    };


    class RenderBundleEncoder {
      public:
        /// Destroys the `RenderBundleEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~RenderBundleEncoder() = default;

        /// Sets the pipeline for this `RenderBundleEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_pipeline(RenderPipelineHandle pipeline) = 0;
        /// Sets the bind group for this `RenderBundleEncoder`.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;
        /// Sets the vertex buffer for this `RenderBundleEncoder`.
        ///
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_vertex_buffer(u32 slot, BufferHandle buffer, u64 offset = 0) = 0;
        /// Sets the index buffer for this `RenderBundleEncoder`.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset = 0) = 0;
        /// Sets the push constants for this `RenderBundleEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;
        /// Sets the viewport for this `RenderBundleEncoder`.
        ///
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_viewport(const Viewport &viewport) = 0;
        /// Sets the scissor for this `RenderBundleEncoder`.
        ///
        /// @param scissor `scissor` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_scissor(const Rect2D &scissor) = 0;
        /// Sets the blend constant for this `RenderBundleEncoder`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_blend_constant(const ClearColor &color) = 0;
        /// Sets the stencil reference for this `RenderBundleEncoder`.
        ///
        /// @param reference `reference` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_stencil_reference(u32 reference) = 0;

        /// Draws the requested content using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw(const DrawArgs &args) = 0;
        /// Draws indexed using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed(const DrawIndexedArgs &args) = 0;
        /// Draws mesh tasks using the current rendering state.
        ///
        /// @param args `args` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks(const DrawMeshTasksArgs &args) = 0;
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        /// Draws indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        /// Draws indexed indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param draw_count Number of elements or operations to process.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect(BufferHandle indirect_buffer, u64 offset, u32 draw_count, u32 stride) = 0;
        /// Returns the draw indirect count for this `RenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                         BufferHandle count_buffer, u64 count_offset,
                                         u32 max_draws, u32 stride) = 0;
        /// Returns the draw indexed indirect count for this `RenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_indexed_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                 BufferHandle count_buffer, u64 count_offset,
                                                 u32 max_draws, u32 stride) = 0;
        /// Draws mesh tasks indirect using the current rendering state.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks_indirect(BufferHandle indirect_buffer, u64 offset) = 0;
        /// Returns the draw mesh tasks indirect count for this `RenderBundleEncoder`.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param indirect_offset Offset from the beginning of the relevant range or buffer.
        /// @param count_buffer Buffer used or affected by the operation.
        /// @param count_offset Offset from the beginning of the relevant range or buffer.
        /// @param max_draws `max_draws` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void draw_mesh_tasks_indirect_count(BufferHandle indirect_buffer, u64 indirect_offset,
                                                    BufferHandle count_buffer, u64 count_offset,
                                                    u32 max_draws, u32 stride) = 0;

        /// Returns the current or globally available finish value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<RenderBundleHandle> finish() = 0;
    };


    class ComputePassEncoder {
      public:
        /// Destroys the `ComputePassEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~ComputePassEncoder() = default;

        /// Sets the pipeline for this `ComputePassEncoder`.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_pipeline(ComputePipelineHandle pipeline) = 0;
        /// Sets the bind group for this `ComputePassEncoder`.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param bind_group `bind_group` value used by the operation.
        /// @param dynamic_offsets Offset from the beginning of the relevant range or buffer.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_bind_group(u32 index, BindGroupHandle bind_group,
                                    span<const u32> dynamic_offsets = {}) = 0;
        /// Sets the push constants for this `ComputePassEncoder`.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void set_push_constants(ShaderStage stages, u32 offset, span<const std::byte> data) = 0;

        /// Dispatches the requested work.
        ///
        /// @param group_count_x `group_count_x` value used by the operation.
        /// @param group_count_y `group_count_y` value used by the operation.
        /// @param group_count_z `group_count_z` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void dispatch(u32 group_count_x, u32 group_count_y = 1, u32 group_count_z = 1) = 0;
        /// Dispatches indirect.
        ///
        /// @param indirect_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void dispatch_indirect(BufferHandle indirect_buffer, u64 offset) = 0;

        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void end() = 0;
    };

    enum class CommandBufferUsage : u32 {
        OneTimeSubmit,
        MultiSubmit,
    };

    struct CommandEncoderDesc {
        QueueLane queue{};
        CommandBufferUsage usage = CommandBufferUsage::OneTimeSubmit;
        const char *label = nullptr;
    };


    class CommandEncoder {
      public:
        /// Destroys the `CommandEncoder` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~CommandEncoder() = default;

        /// Performs the begin render pass operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<unique_ptr<RenderPassEncoder>> begin_render_pass(const RenderPassDesc &desc) = 0;
        /// Performs the begin compute pass operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<unique_ptr<ComputePassEncoder>> begin_compute_pass(const ComputePassDesc &desc) = 0;

        /// Copies buffer to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void copy_buffer_to_buffer(BufferHandle src, BufferHandle dst, const BufferCopy &region) = 0;
        /// Copies buffer to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void copy_buffer_to_texture(BufferHandle src, TextureHandle dst, const BufferTextureCopy &region) = 0;
        /// Copies texture to buffer to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy &region) = 0;


        /// Copies texture to texture to its destination.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void copy_texture_to_texture(TextureHandle src, TextureHandle dst, const TextureCopy &region) = 0;


        /// Performs the blit texture operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @param src Source value or resource.
        /// @param dst Destination value or resource.
        /// @param region `region` value used by the operation.
        /// @param filter `filter` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void blit_texture(TextureHandle src, TextureHandle dst, const TextureBlit &region, Filter filter) = 0;


        /// Fills buffer using the supplied arguments and current state.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param value Value consumed by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void fill_buffer(BufferHandle buffer, u64 offset, u64 size, u32 value) = 0;


        /// Updates buffer from the supplied values.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void update_buffer(BufferHandle buffer, u64 offset, span<const std::byte> data) = 0;


        /// Clears color texture.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param color `color` value used by the operation.
        /// @param range Range of values to process.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void clear_color_texture(TextureHandle texture, const ClearColor &color,
                                         const TextureSubresourceRange &range) = 0;
        /// Clears depth stencil texture.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param value Value consumed by the operation.
        /// @param range Range of values to process.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void clear_depth_stencil_texture(TextureHandle texture, const ClearDepthStencil &value,
                                                 const TextureSubresourceRange &range) = 0;


        /// Builds acceleration structures.
        ///
        /// @param builds `builds` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void build_acceleration_structures(span<const AccelerationStructureBuildDesc> builds) = 0;
        /// Copies acceleration structure to its destination.
        ///
        /// @param copy `copy` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void copy_acceleration_structure(const AccelerationStructureCopyDesc &copy) = 0;
        /// Traces rays using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void trace_rays(const TraceRaysDesc &desc) = 0;


        /// Performs the barrier operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @param global_barriers `global_barriers` value used by the operation.
        /// @param buffer_barriers Buffer used or affected by the operation.
        /// @param texture_barriers Texture used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void barrier(span<const GlobalBarrier> global_barriers,
                             span<const BufferBarrier> buffer_barriers,
                             span<const TextureBarrier> texture_barriers) = 0;


        /// Resets query set to its baseline state.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void reset_query_set(QuerySetHandle query_set, u32 first, u32 count) = 0;


        /// Writes timestamp to the associated destination.
        ///
        /// @param stage `stage` value used by the operation.
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void write_timestamp(PipelineStage stage, QuerySetHandle query_set, u32 index) = 0;


        /// Performs the begin pipeline statistics query operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void begin_pipeline_statistics_query(QuerySetHandle query_set, u32 index) = 0;
        /// Performs the end pipeline statistics query operation for `CommandEncoder` using the supplied arguments.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void end_pipeline_statistics_query() = 0;


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
        virtual void resolve_query_set(QuerySetHandle query_set, u32 first, u32 count,
                                       BufferHandle dst, u64 dst_offset, u64 stride,
                                       QueryResultFlags flags = QueryResultFlags::Result64Bit) = 0;


        /// Adds the supplied value to the end or work queue.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void push_debug_group(const char *label) = 0;
        /// Removes and returns or discards the next value from the container or queue.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        virtual void pop_debug_group() = 0;


        /// Returns the current or globally available finish value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<CommandBufferHandle> finish() = 0;
    };

} // namespace SFT::RHI
