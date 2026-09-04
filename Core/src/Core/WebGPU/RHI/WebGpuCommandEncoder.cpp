#include <Core/WebGPU/RHI/WebGpuCommandEncoder.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>
#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <cstring>
#include <optional>
#include <vector>

namespace SFT::Core::WebGpu {

    namespace {

        /// Logs, once per call site, that a command has no WebGPU equivalent.
        ///
        /// The RHI's recording entry points return void, so an unsupported command cannot be
        /// reported through the normal error channel. Silently ignoring one would produce wrong
        /// output with nothing to explain it, so it is logged instead — loudly enough to find, but
        /// without aborting a frame that may still be mostly correct.
        ///
        /// @param what The command that was requested.
        ///
        /// @note This function does not throw exceptions.
        void report_unsupported_command(const char *what) noexcept {
            Foundation::log_error("WebGPU backend: '{}' has no WebGPU equivalent and was ignored.", what);
        }

        /// Builds the buffer-side layout of a buffer/texture copy.
        ///
        /// The RHI follows Vulkan here and measures `buffer_row_length` in texels, with zero
        /// meaning "tightly packed"; WebGPU wants a byte count, and rejects a zero outright rather
        /// than reading it as tightly packed. The two also count differently for a block-compressed
        /// format: `buffer_image_height` is a texel row count, where WebGPU's `rowsPerImage` counts
        /// rows of blocks.
        ///
        /// WebGPU further requires `bytesPerRow` to be a multiple of 256 whenever the copy spans
        /// more than one row, exactly as D3D12 does. That is not enforced here -- Dawn reports it
        /// with the offending pitch, which is more useful than anything this could say -- but it is
        /// why `Renderer::upload_texture_rgba` pads its staging rows for this backend too.
        ///
        /// @param region The copy region as the RHI describes it.
        /// @param texture Format and extent of the texture side of the copy.
        ///
        /// @return Returns the value converted to the WebGPU representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUTexelCopyBufferLayout texel_copy_layout(
            const rhi::BufferTextureCopy &region, const WebGpuDevice::TextureLayout &texture) noexcept {
            const u32 block = format_block_extent(texture.format);
            const u32 element_bytes = format_element_bytes(texture.format);
            const u32 width =
                region.texture_extent.width != 0 ? region.texture_extent.width : texture.extent.width;
            const u32 height =
                region.texture_extent.height != 0 ? region.texture_extent.height : texture.extent.height;
            const u32 layers = region.array_layer_count != 0 ? region.array_layer_count : 1u;
            const u32 row_length = region.buffer_row_length != 0 ? region.buffer_row_length : width;
            const u32 image_height = region.buffer_image_height != 0 ? region.buffer_image_height : height;
            const u32 block_rows = (height + block - 1u) / block;

            WGPUTexelCopyBufferLayout layout{};
            layout.offset = region.buffer_offset;
            // A copy of a single row of a single layer has no stride to speak of, and supplying one
            // would only invite the 256-byte rule to reject a pitch nothing reads across.
            layout.bytesPerRow = block_rows > 1u || layers > 1u
                                     ? ((row_length + block - 1u) / block) * element_bytes
                                     : WGPU_COPY_STRIDE_UNDEFINED;
            layout.rowsPerImage =
                layers > 1u ? (image_height + block - 1u) / block : WGPU_COPY_STRIDE_UNDEFINED;
            return layout;
        }

        /// Applies one `set_push_constants` call to an encoder's shadow block and obtains the ring
        /// slice to bind.
        ///
        /// The stage mask is dropped: the emulation's bind group layout makes the block visible to
        /// every stage, because WebGPU has no way to vary a binding's visibility per draw and a
        /// block visible to more stages than a shader reads costs nothing.
        ///
        /// @param device Device owning the ring.
        /// @param shadow The encoder's mirror of the whole block, updated in place.
        /// @param offset Byte offset within the block the caller is writing at.
        /// @param data Bytes to write there.
        ///
        /// @return Returns the group and dynamic offset to bind, or `std::nullopt` when the write
        ///         was out of range or no slice could be obtained.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<WebGpuDevice::PushConstantBinding> stage_push_constants(
            WebGpuDevice &device, std::array<std::byte, push_constant_shadow_size> &shadow, u32 offset,
            span<const std::byte> data) noexcept {
            static_assert(push_constant_shadow_size == WebGpuDevice::push_constant_block_size,
                          "the encoder shadow block and the device's ring slice must be the same size");
            if (static_cast<usize>(offset) + data.size() > shadow.size()) {
                Foundation::log_error(
                    "WebGPU backend: a push-constant write of {} byte(s) at offset {} does not fit the "
                    "{}-byte block this backend emulates.",
                    data.size(), offset, shadow.size());
                return std::nullopt;
            }
            std::memcpy(shadow.data() + offset, data.data(), data.size());
            return device.allocate_push_constant_slice(span<const std::byte>{shadow.data(), shadow.size()});
        }

    } // namespace

    // ─── Render pass ─────────────────────────────────────────────────────────────

    WebGpuRenderPassEncoder::WebGpuRenderPassEncoder(WebGpuDevice &device, WGPURenderPassEncoder pass) noexcept
        : device_(device), pass_(pass) {}

    WebGpuRenderPassEncoder::~WebGpuRenderPassEncoder() {
        if (pass_ != nullptr) {
            wgpuRenderPassEncoderRelease(pass_);
        }
    }

    void WebGpuRenderPassEncoder::set_pipeline(rhi::RenderPipelineHandle pipeline) {
        if (WGPURenderPipeline p = device_.lookup_render_pipeline(pipeline); p != nullptr) {
            wgpuRenderPassEncoderSetPipeline(pass_, p);
        }
    }

    void WebGpuRenderPassEncoder::set_bind_group(u32 index, rhi::BindGroupHandle bind_group,
                                                 span<const u32> dynamic_offsets) {
        if (WGPUBindGroup group = device_.lookup_bind_group(bind_group); group != nullptr) {
            wgpuRenderPassEncoderSetBindGroup(pass_, index, group, dynamic_offsets.size(),
                                              dynamic_offsets.data());
        }
    }

    void WebGpuRenderPassEncoder::set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(buffer); b != nullptr) {
            wgpuRenderPassEncoderSetVertexBuffer(pass_, slot, b, offset, WGPU_WHOLE_SIZE);
        }
    }

    void WebGpuRenderPassEncoder::set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format,
                                                   u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(buffer); b != nullptr) {
            wgpuRenderPassEncoderSetIndexBuffer(pass_, b, to_wgpu(format), offset, WGPU_WHOLE_SIZE);
        }
    }

    void WebGpuRenderPassEncoder::set_push_constants(rhi::ShaderStage stages, u32 offset,
                                                     span<const std::byte> data) {
        (void)stages;
        if (const auto binding = stage_push_constants(device_, push_constants_, offset, data)) {
            wgpuRenderPassEncoderSetBindGroup(pass_, WebGpuDevice::push_constant_group_index,
                                              binding->group, 1, &binding->dynamic_offset);
        }
    }

    void WebGpuRenderPassEncoder::set_viewport(const rhi::Viewport &viewport) {
        wgpuRenderPassEncoderSetViewport(pass_, viewport.x, viewport.y, viewport.width, viewport.height,
                                         viewport.min_depth, viewport.max_depth);
    }

    void WebGpuRenderPassEncoder::set_scissor(const rhi::Rect2D &scissor) {
        wgpuRenderPassEncoderSetScissorRect(pass_, static_cast<u32>(scissor.x),
                                            static_cast<u32>(scissor.y), scissor.width, scissor.height);
    }

    void WebGpuRenderPassEncoder::set_blend_constant(const rhi::ClearColor &color) {
        const WGPUColor wgpu_color{color.r, color.g, color.b, color.a};
        wgpuRenderPassEncoderSetBlendConstant(pass_, &wgpu_color);
    }

    void WebGpuRenderPassEncoder::set_stencil_reference(u32 reference) {
        wgpuRenderPassEncoderSetStencilReference(pass_, reference);
    }

    void WebGpuRenderPassEncoder::set_depth_bounds(f32 min_depth, f32 max_depth) {
        (void)min_depth;
        (void)max_depth;
        report_unsupported_command("set_depth_bounds");
    }

    void WebGpuRenderPassEncoder::set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size,
                                                       span<const rhi::SampleLocation> locations) {
        (void)samples_per_pixel;
        (void)grid_size;
        (void)locations;
        report_unsupported_command("set_sample_locations");
    }

    void WebGpuRenderPassEncoder::set_shading_rate(rhi::ShadingRate rate,
                                                   rhi::ShadingRateCombiner primitive_combiner,
                                                   rhi::ShadingRateCombiner attachment_combiner) {
        (void)rate;
        (void)primitive_combiner;
        (void)attachment_combiner;
        report_unsupported_command("set_shading_rate");
    }

    void WebGpuRenderPassEncoder::draw(const rhi::DrawArgs &args) {
        wgpuRenderPassEncoderDraw(pass_, args.vertex_count, args.instance_count, args.first_vertex,
                                  args.first_instance);
    }

    void WebGpuRenderPassEncoder::draw_indexed(const rhi::DrawIndexedArgs &args) {
        wgpuRenderPassEncoderDrawIndexed(pass_, args.index_count, args.instance_count, args.first_index,
                                         args.base_vertex, args.first_instance);
    }

    void WebGpuRenderPassEncoder::draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) {
        (void)args;
        report_unsupported_command("draw_mesh_tasks");
    }

    void WebGpuRenderPassEncoder::draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(indirect_buffer); b != nullptr) {
            wgpuRenderPassEncoderDrawIndirect(pass_, b, offset);
        }
    }

    void WebGpuRenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(indirect_buffer); b != nullptr) {
            wgpuRenderPassEncoderDrawIndexedIndirect(pass_, b, offset);
        }
    }

    void WebGpuRenderPassEncoder::draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset,
                                                u32 draw_count, u32 stride) {
        // WebGPU has no multi-draw indirect. Issuing the draws one at a time is exactly equivalent
        // apart from the per-call overhead, so this is emulated rather than refused.
        WGPUBuffer b = device_.lookup_buffer(indirect_buffer);
        if (b == nullptr) {
            return;
        }
        for (u32 i = 0; i < draw_count; ++i) {
            wgpuRenderPassEncoderDrawIndirect(pass_, b, offset + static_cast<u64>(i) * stride);
        }
    }

    void WebGpuRenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset,
                                                        u32 draw_count, u32 stride) {
        WGPUBuffer b = device_.lookup_buffer(indirect_buffer);
        if (b == nullptr) {
            return;
        }
        for (u32 i = 0; i < draw_count; ++i) {
            wgpuRenderPassEncoderDrawIndexedIndirect(pass_, b, offset + static_cast<u64>(i) * stride);
        }
    }

    void WebGpuRenderPassEncoder::draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        (void)indirect_buffer;
        (void)offset;
        report_unsupported_command("draw_mesh_tasks_indirect");
    }

    void WebGpuRenderPassEncoder::execute_bundles(span<const rhi::RenderBundleHandle> bundles) {
        std::vector<WGPURenderBundle> resolved;
        resolved.reserve(bundles.size());
        for (rhi::RenderBundleHandle handle : bundles) {
            if (WGPURenderBundle bundle = device_.lookup_render_bundle(handle); bundle != nullptr) {
                resolved.push_back(bundle);
            }
        }
        if (!resolved.empty()) {
            wgpuRenderPassEncoderExecuteBundles(pass_, resolved.size(), resolved.data());
        }
    }

    void WebGpuRenderPassEncoder::begin_occlusion_query(rhi::QuerySetHandle query_set, u32 index) {
        // WebGPU names the occlusion query set on the render pass itself rather than per query, so
        // the set is already bound by the time this runs; only the index is needed here.
        (void)query_set;
        wgpuRenderPassEncoderBeginOcclusionQuery(pass_, index);
    }

    void WebGpuRenderPassEncoder::end_occlusion_query() {
        wgpuRenderPassEncoderEndOcclusionQuery(pass_);
    }

    void WebGpuRenderPassEncoder::end() {
        if (pass_ != nullptr) {
            wgpuRenderPassEncoderEnd(pass_);
        }
    }

    // ─── Compute pass ────────────────────────────────────────────────────────────

    WebGpuComputePassEncoder::WebGpuComputePassEncoder(WebGpuDevice &device,
                                                       WGPUComputePassEncoder pass) noexcept
        : device_(device), pass_(pass) {}

    WebGpuComputePassEncoder::~WebGpuComputePassEncoder() {
        if (pass_ != nullptr) {
            wgpuComputePassEncoderRelease(pass_);
        }
    }

    void WebGpuComputePassEncoder::set_pipeline(rhi::ComputePipelineHandle pipeline) {
        if (WGPUComputePipeline p = device_.lookup_compute_pipeline(pipeline); p != nullptr) {
            wgpuComputePassEncoderSetPipeline(pass_, p);
        }
    }

    void WebGpuComputePassEncoder::set_bind_group(u32 index, rhi::BindGroupHandle bind_group,
                                                  span<const u32> dynamic_offsets) {
        if (WGPUBindGroup group = device_.lookup_bind_group(bind_group); group != nullptr) {
            wgpuComputePassEncoderSetBindGroup(pass_, index, group, dynamic_offsets.size(),
                                               dynamic_offsets.data());
        }
    }

    void WebGpuComputePassEncoder::set_push_constants(rhi::ShaderStage stages, u32 offset,
                                                      span<const std::byte> data) {
        (void)stages;
        if (const auto binding = stage_push_constants(device_, push_constants_, offset, data)) {
            wgpuComputePassEncoderSetBindGroup(pass_, WebGpuDevice::push_constant_group_index,
                                               binding->group, 1, &binding->dynamic_offset);
        }
    }

    void WebGpuComputePassEncoder::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) {
        wgpuComputePassEncoderDispatchWorkgroups(pass_, group_count_x, group_count_y, group_count_z);
    }

    void WebGpuComputePassEncoder::dispatch_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(indirect_buffer); b != nullptr) {
            wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass_, b, offset);
        }
    }

    void WebGpuComputePassEncoder::end() {
        if (pass_ != nullptr) {
            wgpuComputePassEncoderEnd(pass_);
        }
    }

    // ─── Render bundle ───────────────────────────────────────────────────────────

    WebGpuRenderBundleEncoder::WebGpuRenderBundleEncoder(WebGpuDevice &device,
                                                         WGPURenderBundleEncoder encoder) noexcept
        : device_(device), encoder_(encoder) {}

    WebGpuRenderBundleEncoder::~WebGpuRenderBundleEncoder() {
        if (encoder_ != nullptr) {
            wgpuRenderBundleEncoderRelease(encoder_);
        }
    }

    void WebGpuRenderBundleEncoder::set_pipeline(rhi::RenderPipelineHandle pipeline) {
        if (WGPURenderPipeline p = device_.lookup_render_pipeline(pipeline); p != nullptr) {
            wgpuRenderBundleEncoderSetPipeline(encoder_, p);
        }
    }

    void WebGpuRenderBundleEncoder::set_bind_group(u32 index, rhi::BindGroupHandle bind_group,
                                                   span<const u32> dynamic_offsets) {
        if (WGPUBindGroup group = device_.lookup_bind_group(bind_group); group != nullptr) {
            wgpuRenderBundleEncoderSetBindGroup(encoder_, index, group, dynamic_offsets.size(),
                                                dynamic_offsets.data());
        }
    }

    void WebGpuRenderBundleEncoder::set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(buffer); b != nullptr) {
            wgpuRenderBundleEncoderSetVertexBuffer(encoder_, slot, b, offset, WGPU_WHOLE_SIZE);
        }
    }

    void WebGpuRenderBundleEncoder::set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format,
                                                     u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(buffer); b != nullptr) {
            wgpuRenderBundleEncoderSetIndexBuffer(encoder_, b, to_wgpu(format), offset, WGPU_WHOLE_SIZE);
        }
    }

    void WebGpuRenderBundleEncoder::set_push_constants(rhi::ShaderStage stages, u32 offset,
                                                       span<const std::byte> data) {
        (void)stages;
        if (const auto binding = stage_push_constants(device_, push_constants_, offset, data)) {
            wgpuRenderBundleEncoderSetBindGroup(encoder_, WebGpuDevice::push_constant_group_index,
                                                binding->group, 1, &binding->dynamic_offset);
        }
    }

    // Viewport, scissor, blend constant, stencil reference, depth bounds, sample locations and
    // shading rate are all *render pass* state in WebGPU, not bundle state: a bundle inherits
    // whatever the pass executing it has set. The RHI declares them on the bundle encoder because
    // Vulkan and D3D12 allow them in a secondary command buffer, so they are accepted and ignored
    // here rather than reported — the pass has already established the correct values.

    void WebGpuRenderBundleEncoder::set_viewport(const rhi::Viewport &viewport) { (void)viewport; }

    void WebGpuRenderBundleEncoder::set_scissor(const rhi::Rect2D &scissor) { (void)scissor; }

    void WebGpuRenderBundleEncoder::set_blend_constant(const rhi::ClearColor &color) { (void)color; }

    void WebGpuRenderBundleEncoder::set_stencil_reference(u32 reference) { (void)reference; }

    void WebGpuRenderBundleEncoder::set_depth_bounds(f32 min_depth, f32 max_depth) {
        (void)min_depth;
        (void)max_depth;
        report_unsupported_command("set_depth_bounds");
    }

    void WebGpuRenderBundleEncoder::set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size,
                                                         span<const rhi::SampleLocation> locations) {
        (void)samples_per_pixel;
        (void)grid_size;
        (void)locations;
        report_unsupported_command("set_sample_locations");
    }

    void WebGpuRenderBundleEncoder::draw(const rhi::DrawArgs &args) {
        wgpuRenderBundleEncoderDraw(encoder_, args.vertex_count, args.instance_count, args.first_vertex,
                                    args.first_instance);
    }

    void WebGpuRenderBundleEncoder::draw_indexed(const rhi::DrawIndexedArgs &args) {
        wgpuRenderBundleEncoderDrawIndexed(encoder_, args.index_count, args.instance_count,
                                           args.first_index, args.base_vertex, args.first_instance);
    }

    void WebGpuRenderBundleEncoder::draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) {
        (void)args;
        report_unsupported_command("draw_mesh_tasks");
    }

    void WebGpuRenderBundleEncoder::draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(indirect_buffer); b != nullptr) {
            wgpuRenderBundleEncoderDrawIndirect(encoder_, b, offset);
        }
    }

    void WebGpuRenderBundleEncoder::draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        if (WGPUBuffer b = device_.lookup_buffer(indirect_buffer); b != nullptr) {
            wgpuRenderBundleEncoderDrawIndexedIndirect(encoder_, b, offset);
        }
    }

    void WebGpuRenderBundleEncoder::draw_indirect(rhi::BufferHandle indirect_buffer, u64 offset,
                                                  u32 draw_count, u32 stride) {
        WGPUBuffer b = device_.lookup_buffer(indirect_buffer);
        if (b == nullptr) {
            return;
        }
        for (u32 i = 0; i < draw_count; ++i) {
            wgpuRenderBundleEncoderDrawIndirect(encoder_, b, offset + static_cast<u64>(i) * stride);
        }
    }

    void WebGpuRenderBundleEncoder::draw_indexed_indirect(rhi::BufferHandle indirect_buffer, u64 offset,
                                                          u32 draw_count, u32 stride) {
        WGPUBuffer b = device_.lookup_buffer(indirect_buffer);
        if (b == nullptr) {
            return;
        }
        for (u32 i = 0; i < draw_count; ++i) {
            wgpuRenderBundleEncoderDrawIndexedIndirect(encoder_, b, offset + static_cast<u64>(i) * stride);
        }
    }

    void WebGpuRenderBundleEncoder::draw_mesh_tasks_indirect(rhi::BufferHandle indirect_buffer, u64 offset) {
        (void)indirect_buffer;
        (void)offset;
        report_unsupported_command("draw_mesh_tasks_indirect");
    }

    rhi::RhiExpected<rhi::RenderBundleHandle> WebGpuRenderBundleEncoder::finish() {
        if (encoder_ == nullptr) {
            return std::unexpected(webgpu_error("finish", "the bundle encoder was already finished"));
        }
        WGPURenderBundleDescriptor bundle_desc{};
        WGPURenderBundle bundle = wgpuRenderBundleEncoderFinish(encoder_, &bundle_desc);
        wgpuRenderBundleEncoderRelease(encoder_);
        encoder_ = nullptr;
        if (bundle == nullptr) {
            return std::unexpected(webgpu_error("finish"));
        }
        return device_.store_render_bundle(bundle);
    }

    // ─── Command encoder ─────────────────────────────────────────────────────────

    WebGpuCommandEncoder::WebGpuCommandEncoder(WebGpuDevice &device, WGPUCommandEncoder encoder) noexcept
        : device_(device), encoder_(encoder) {}

    WebGpuCommandEncoder::~WebGpuCommandEncoder() {
        if (encoder_ != nullptr) {
            wgpuCommandEncoderRelease(encoder_);
        }
    }

    rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> WebGpuCommandEncoder::begin_render_pass(
        const rhi::RenderPassDesc &desc) {
        std::vector<WGPURenderPassColorAttachment> color_attachments;
        color_attachments.reserve(desc.color_attachments.size());
        for (const rhi::ColorAttachment &attachment : desc.color_attachments) {
            WGPUTextureView view = device_.lookup_texture_view(attachment.view);
            if (view == nullptr) {
                return std::unexpected(webgpu_error("begin_render_pass", "unknown color attachment view"));
            }
            WGPURenderPassColorAttachment out{};
            out.view = view;
            out.resolveTarget = attachment.resolve_view.value != 0
                                    ? device_.lookup_texture_view(attachment.resolve_view)
                                    : nullptr;
            out.loadOp = to_wgpu(attachment.load_op);
            out.storeOp = to_wgpu(attachment.store_op);
            out.clearValue = WGPUColor{attachment.clear_color.r, attachment.clear_color.g,
                                       attachment.clear_color.b, attachment.clear_color.a};
            // WebGPU uses this sentinel to mean "not an array layer selection"; leaving it zero
            // would ask for slice 0 of a 3D texture instead.
            out.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            color_attachments.push_back(out);
        }

        WGPURenderPassDepthStencilAttachment depth_stencil{};
        const bool has_depth = desc.depth_stencil.view.value != 0;
        if (has_depth) {
            WGPUTextureView view = device_.lookup_texture_view(desc.depth_stencil.view);
            if (view == nullptr) {
                return std::unexpected(webgpu_error("begin_render_pass", "unknown depth/stencil view"));
            }
            depth_stencil.view = view;
            depth_stencil.depthLoadOp = to_wgpu(desc.depth_stencil.depth_load_op);
            depth_stencil.depthStoreOp = to_wgpu(desc.depth_stencil.depth_store_op);
            depth_stencil.depthClearValue = desc.depth_stencil.clear_value.depth;
            depth_stencil.stencilLoadOp = to_wgpu(desc.depth_stencil.stencil_load_op);
            depth_stencil.stencilStoreOp = to_wgpu(desc.depth_stencil.stencil_store_op);
            depth_stencil.stencilClearValue = desc.depth_stencil.clear_value.stencil;
        }

        WGPURenderPassDescriptor pass_desc{};
        pass_desc.label = wgpu_string(desc.label);
        pass_desc.colorAttachmentCount = color_attachments.size();
        pass_desc.colorAttachments = color_attachments.data();
        pass_desc.depthStencilAttachment = has_depth ? &depth_stencil : nullptr;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder_, &pass_desc);
        if (pass == nullptr) {
            return std::unexpected(webgpu_error("begin_render_pass"));
        }
        return std::make_unique<WebGpuRenderPassEncoder>(device_, pass);
    }

    rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> WebGpuCommandEncoder::begin_compute_pass(
        const rhi::ComputePassDesc &desc) {
        WGPUComputePassDescriptor pass_desc{};
        pass_desc.label = wgpu_string(desc.label);
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder_, &pass_desc);
        if (pass == nullptr) {
            return std::unexpected(webgpu_error("begin_compute_pass"));
        }
        return std::make_unique<WebGpuComputePassEncoder>(device_, pass);
    }

    void WebGpuCommandEncoder::copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst,
                                                     const rhi::BufferCopy &region) {
        WGPUBuffer source = device_.lookup_buffer(src);
        WGPUBuffer destination = device_.lookup_buffer(dst);
        if (source != nullptr && destination != nullptr) {
            wgpuCommandEncoderCopyBufferToBuffer(encoder_, source, region.src_offset, destination,
                                                 region.dst_offset, region.size);
        }
    }

    void WebGpuCommandEncoder::copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst,
                                                      const rhi::BufferTextureCopy &region) {
        WGPUBuffer source = device_.lookup_buffer(src);
        WGPUTexture destination = device_.lookup_texture(dst);
        WebGpuDevice::TextureLayout layout{};
        if (source == nullptr || destination == nullptr || !device_.lookup_texture_layout(dst, layout)) {
            return;
        }
        WGPUTexelCopyBufferInfo buffer_info{};
        buffer_info.buffer = source;
        buffer_info.layout = texel_copy_layout(region, layout);

        WGPUTexelCopyTextureInfo texture_info{};
        texture_info.texture = destination;
        texture_info.mipLevel = region.mip_level;
        texture_info.origin = WGPUOrigin3D{static_cast<u32>(region.texture_offset.x),
                                           static_cast<u32>(region.texture_offset.y),
                                           region.base_array_layer};
        texture_info.aspect = WGPUTextureAspect_All;

        const WGPUExtent3D extent{region.texture_extent.width, region.texture_extent.height,
                                  region.array_layer_count};
        wgpuCommandEncoderCopyBufferToTexture(encoder_, &buffer_info, &texture_info, &extent);
    }

    void WebGpuCommandEncoder::copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst,
                                                      const rhi::BufferTextureCopy &region) {
        WGPUTexture source = device_.lookup_texture(src);
        WGPUBuffer destination = device_.lookup_buffer(dst);
        WebGpuDevice::TextureLayout layout{};
        if (source == nullptr || destination == nullptr || !device_.lookup_texture_layout(src, layout)) {
            return;
        }
        WGPUTexelCopyTextureInfo texture_info{};
        texture_info.texture = source;
        texture_info.mipLevel = region.mip_level;
        texture_info.origin = WGPUOrigin3D{static_cast<u32>(region.texture_offset.x),
                                           static_cast<u32>(region.texture_offset.y),
                                           region.base_array_layer};
        texture_info.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferInfo buffer_info{};
        buffer_info.buffer = destination;
        buffer_info.layout = texel_copy_layout(region, layout);

        const WGPUExtent3D extent{region.texture_extent.width, region.texture_extent.height,
                                  region.array_layer_count};
        wgpuCommandEncoderCopyTextureToBuffer(encoder_, &texture_info, &buffer_info, &extent);
    }

    void WebGpuCommandEncoder::copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst,
                                                       const rhi::TextureCopy &region) {
        WGPUTexture source = device_.lookup_texture(src);
        WGPUTexture destination = device_.lookup_texture(dst);
        if (source == nullptr || destination == nullptr) {
            return;
        }
        WGPUTexelCopyTextureInfo source_info{};
        source_info.texture = source;
        source_info.mipLevel = region.src_subresource.mip_level;
        source_info.origin = WGPUOrigin3D{static_cast<u32>(region.src_offset.x),
                                          static_cast<u32>(region.src_offset.y),
                                          region.src_subresource.base_array_layer};
        source_info.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyTextureInfo destination_info{};
        destination_info.texture = destination;
        destination_info.mipLevel = region.dst_subresource.mip_level;
        destination_info.origin = WGPUOrigin3D{static_cast<u32>(region.dst_offset.x),
                                               static_cast<u32>(region.dst_offset.y),
                                               region.dst_subresource.base_array_layer};
        destination_info.aspect = WGPUTextureAspect_All;

        const WGPUExtent3D extent{region.extent.width, region.extent.height, region.extent.depth_or_layers};
        wgpuCommandEncoderCopyTextureToTexture(encoder_, &source_info, &destination_info, &extent);
    }

    void WebGpuCommandEncoder::blit_texture(rhi::TextureHandle src, rhi::TextureHandle dst,
                                            const rhi::TextureBlit &region, rhi::Filter filter) {
        (void)src;
        (void)dst;
        (void)region;
        (void)filter;
        // WebGPU has no scaling blit. The equivalent is a full-screen draw sampling the source,
        // which is a render pass the caller has to author -- not something this layer can
        // manufacture without owning a pipeline and shader of its own.
        report_unsupported_command("blit_texture");
    }

    void WebGpuCommandEncoder::fill_buffer(rhi::BufferHandle buffer, u64 offset, u64 size, u32 value) {
        WGPUBuffer b = device_.lookup_buffer(buffer);
        if (b == nullptr) {
            return;
        }
        if (value != 0) {
            // WebGPU can only clear a buffer to zero. A non-zero fill would need a compute shader.
            report_unsupported_command("fill_buffer with a non-zero value");
            return;
        }
        wgpuCommandEncoderClearBuffer(encoder_, b, offset, size);
    }

    void WebGpuCommandEncoder::update_buffer(rhi::BufferHandle buffer, u64 offset,
                                             span<const std::byte> data) {
        // WebGPU has no inline buffer update inside a command encoder; queue writes are ordered
        // against submitted work, which is the same guarantee the RHI's update_buffer makes.
        (void)device_.write_buffer(buffer, offset, data);
    }

    void WebGpuCommandEncoder::clear_color_texture(rhi::TextureHandle texture, const rhi::ClearColor &color,
                                                   const rhi::TextureSubresourceRange &range) {
        (void)texture;
        (void)color;
        (void)range;
        // Clearing outside a render pass does not exist in WebGPU: a colour attachment is cleared
        // by beginning a pass with LoadOp::Clear, which is how the render graph already expresses
        // it on every backend.
        report_unsupported_command("clear_color_texture (use a render pass with LoadOp::Clear)");
    }

    void WebGpuCommandEncoder::clear_depth_stencil_texture(rhi::TextureHandle texture,
                                                           const rhi::ClearDepthStencil &value,
                                                           const rhi::TextureSubresourceRange &range) {
        (void)texture;
        (void)value;
        (void)range;
        report_unsupported_command("clear_depth_stencil_texture (use a render pass with LoadOp::Clear)");
    }

    void WebGpuCommandEncoder::build_acceleration_structures(
        span<const rhi::AccelerationStructureBuildDesc> builds) {
        (void)builds;
        report_unsupported_command("build_acceleration_structures");
    }

    void WebGpuCommandEncoder::build_opacity_micromaps(span<const rhi::OpacityMicromapBuildDesc> builds) {
        (void)builds;
        report_unsupported_command("build_opacity_micromaps");
    }

    void WebGpuCommandEncoder::copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) {
        (void)copy;
        report_unsupported_command("copy_acceleration_structure");
    }

    void WebGpuCommandEncoder::set_ray_tracing_pipeline(rhi::RayTracingPipelineHandle pipeline) {
        (void)pipeline;
        report_unsupported_command("set_ray_tracing_pipeline");
    }

    void WebGpuCommandEncoder::trace_rays(const rhi::TraceRaysDesc &desc) {
        (void)desc;
        report_unsupported_command("trace_rays");
    }

    void WebGpuCommandEncoder::barrier(span<const rhi::GlobalBarrier> global_barriers,
                                       span<const rhi::BufferBarrier> buffer_barriers,
                                       span<const rhi::TextureBarrier> texture_barriers) {
        (void)global_barriers;
        (void)buffer_barriers;
        (void)texture_barriers;
        // Deliberately a no-op, and *not* an unsupported-command report. WebGPU has no barriers
        // because it does not need them: the implementation tracks every resource's usage across
        // passes and inserts whatever transitions and cache flushes the underlying driver requires.
        // A caller issuing correct barriers for Vulkan is issuing redundant information here, not
        // losing anything.
    }

    void WebGpuCommandEncoder::reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) {
        (void)query_set;
        (void)first;
        (void)count;
        // Nothing to do: see WebGpuDevice::reset_query_set.
    }

    void WebGpuCommandEncoder::write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set,
                                               u32 index) {
        // WebGPU timestamps are not staged: the write happens between passes rather than at a
        // nominated pipeline stage, so `stage` has nothing to map to.
        (void)stage;
        if (WGPUQuerySet set = device_.lookup_query_set(query_set); set != nullptr) {
            wgpuCommandEncoderWriteTimestamp(encoder_, set, index);
        }
    }

    void WebGpuCommandEncoder::begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) {
        (void)query_set;
        (void)index;
        report_unsupported_command("begin_pipeline_statistics_query");
    }

    void WebGpuCommandEncoder::end_pipeline_statistics_query() {
        report_unsupported_command("end_pipeline_statistics_query");
    }

    void WebGpuCommandEncoder::resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count,
                                                 rhi::BufferHandle dst, u64 dst_offset, u64 stride,
                                                 rhi::QueryResultFlags flags) {
        // WebGPU always resolves to tightly packed 64-bit values, so an explicit stride or a
        // 32-bit result request cannot be honoured.
        (void)stride;
        (void)flags;
        WGPUQuerySet set = device_.lookup_query_set(query_set);
        WGPUBuffer destination = device_.lookup_buffer(dst);
        if (set != nullptr && destination != nullptr) {
            wgpuCommandEncoderResolveQuerySet(encoder_, set, first, count, destination, dst_offset);
        }
    }

    void WebGpuCommandEncoder::push_debug_group(const char *label) {
        wgpuCommandEncoderPushDebugGroup(encoder_, wgpu_string(label));
    }

    void WebGpuCommandEncoder::pop_debug_group() { wgpuCommandEncoderPopDebugGroup(encoder_); }

    rhi::RhiExpected<rhi::CommandBufferHandle> WebGpuCommandEncoder::finish() {
        if (encoder_ == nullptr) {
            return std::unexpected(webgpu_error("finish", "the command encoder was already finished"));
        }
        WGPUCommandBufferDescriptor buffer_desc{};
        WGPUCommandBuffer buffer = wgpuCommandEncoderFinish(encoder_, &buffer_desc);
        wgpuCommandEncoderRelease(encoder_);
        encoder_ = nullptr;
        if (buffer == nullptr) {
            return std::unexpected(webgpu_error("finish"));
        }
        return device_.store_command_buffer(buffer);
    }

    // ─── Encoder creation (WebGpuDevice) ─────────────────────────────────────────
    //
    // Defined here rather than in the device's own translation units because they are the only
    // device entry points that need the encoder class definitions above.

    /// Creates a command encoder.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> WebGpuDevice::create_command_encoder(
        const rhi::CommandEncoderDesc &desc) {
        WGPUCommandEncoderDescriptor encoder_desc{};
        encoder_desc.label = wgpu_string(desc.label);
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encoder_desc);
        if (encoder == nullptr) {
            return std::unexpected(webgpu_error("create_command_encoder"));
        }
        return std::make_unique<WebGpuCommandEncoder>(*this, encoder);
    }

    /// Creates a render bundle encoder.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> WebGpuDevice::create_render_bundle_encoder(
        const rhi::RenderBundleDesc &desc) {
        if (desc.view_mask != 0) {
            return std::unexpected(unsupported_by_webgpu("Multiview render bundles"));
        }

        std::vector<WGPUTextureFormat> color_formats;
        color_formats.reserve(desc.color_formats.size());
        for (rhi::Format format : desc.color_formats) {
            const WGPUTextureFormat converted = to_wgpu(format);
            if (converted == WGPUTextureFormat_Undefined) {
                return std::unexpected(webgpu_error(
                    "create_render_bundle_encoder", "a color format has no WebGPU equivalent"));
            }
            color_formats.push_back(converted);
        }

        WGPURenderBundleEncoderDescriptor encoder_desc{};
        encoder_desc.label = wgpu_string(desc.label);
        encoder_desc.colorFormatCount = color_formats.size();
        encoder_desc.colorFormats = color_formats.data();
        encoder_desc.depthStencilFormat = to_wgpu(desc.depth_stencil_format);
        encoder_desc.sampleCount = static_cast<u32>(desc.samples);

        WGPURenderBundleEncoder encoder = wgpuDeviceCreateRenderBundleEncoder(device_, &encoder_desc);
        if (encoder == nullptr) {
            return std::unexpected(webgpu_error("create_render_bundle_encoder"));
        }
        return std::make_unique<WebGpuRenderBundleEncoder>(*this, encoder);
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderPassEncoder::draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_indirect_count");
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderPassEncoder::draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_indexed_indirect_count");
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderPassEncoder::draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_mesh_tasks_indirect_count");
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderBundleEncoder::draw_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_indirect_count");
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderBundleEncoder::draw_indexed_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_indexed_indirect_count");
    }

    // WebGPU has no count-buffer indirect draw: the draw count must be known on the CPU. Emulating
    // one would mean reading the count buffer back, which turns a GPU-driven submission into a
    // stall -- a different performance model, not the same feature.
    void WebGpuRenderBundleEncoder::draw_mesh_tasks_indirect_count(rhi::BufferHandle indirect_buffer, u64 indirect_offset,
                                          rhi::BufferHandle count_buffer, u64 count_offset, u32 max_draws,
                                          u32 stride) {
        (void)indirect_buffer;
        (void)indirect_offset;
        (void)count_buffer;
        (void)count_offset;
        (void)max_draws;
        (void)stride;
        report_unsupported_command("draw_mesh_tasks_indirect_count");
    }

} // namespace SFT::Core::WebGpu
