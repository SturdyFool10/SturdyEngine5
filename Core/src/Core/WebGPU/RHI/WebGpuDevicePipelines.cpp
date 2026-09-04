#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

#include <vector>

namespace SFT::Core::WebGpu {

    namespace {

        /// Translates one RHI blend component into WebGPU's.
        ///
        /// @param component `component` value used by the operation.
        ///
        /// @return Returns the value converted to the WebGPU representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBlendComponent to_wgpu_blend(const rhi::BlendComponent &component) noexcept {
            return WGPUBlendComponent{
                .operation = to_wgpu(component.op),
                .srcFactor = to_wgpu(component.src_factor),
                .dstFactor = to_wgpu(component.dst_factor),
            };
        }

        /// Translates an RHI stencil face state into WebGPU's.
        ///
        /// @param face `face` value used by the operation.
        ///
        /// @return Returns the value converted to the WebGPU representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUStencilFaceState to_wgpu_stencil(const rhi::StencilFaceState &face) noexcept {
            return WGPUStencilFaceState{
                .compare = to_wgpu(face.compare),
                .failOp = to_wgpu(face.fail_op),
                .depthFailOp = to_wgpu(face.depth_fail_op),
                .passOp = to_wgpu(face.pass_op),
            };
        }

    } // namespace

    /// Creates a render pipeline.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::RenderPipelineHandle> WebGpuDevice::create_render_pipeline(
        const rhi::RenderPipelineDesc &desc) {
        if (desc.mesh.module.value != 0 || desc.task.module.value != 0) {
            return std::unexpected(unsupported_by_webgpu("Mesh and task shader stages"));
        }
        if (desc.view_mask != 0) {
            return std::unexpected(unsupported_by_webgpu("Multiview rendering"));
        }
        if (desc.depth_stencil.depth_bounds_test_enable) {
            return std::unexpected(unsupported_by_webgpu("Depth bounds testing"));
        }
        if (desc.rasterization.conservative_rasterization_enable) {
            return std::unexpected(unsupported_by_webgpu("Conservative rasterization"));
        }

        WGPUPipelineLayout *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return std::unexpected(webgpu_error("create_render_pipeline", "unknown pipeline layout handle"));
        }
        WGPUShaderModule *vertex_module = shader_modules_.find(desc.vertex.module);
        if (vertex_module == nullptr) {
            return std::unexpected(webgpu_error("create_render_pipeline", "unknown vertex shader module"));
        }

        // Vertex layouts and their attributes are flattened into contiguous storage first: WebGPU
        // takes pointers into arrays that must outlive the create call.
        std::vector<std::vector<WGPUVertexAttribute>> attribute_storage;
        std::vector<WGPUVertexBufferLayout> vertex_layouts;
        attribute_storage.reserve(desc.vertex_buffers.size());
        vertex_layouts.reserve(desc.vertex_buffers.size());
        for (const rhi::VertexBufferLayout &buffer : desc.vertex_buffers) {
            std::vector<WGPUVertexAttribute> attributes;
            attributes.reserve(buffer.attributes.size());
            for (const rhi::VertexAttribute &attribute : buffer.attributes) {
                attributes.push_back(WGPUVertexAttribute{
                    .nextInChain = nullptr,
                    .format = to_wgpu(attribute.format),
                    .offset = attribute.offset,
                    .shaderLocation = attribute.shader_location,
                });
            }
            attribute_storage.push_back(std::move(attributes));
            vertex_layouts.push_back(WGPUVertexBufferLayout{
                .nextInChain = nullptr,
                .stepMode = to_wgpu(buffer.step_mode),
                .arrayStride = buffer.stride,
                .attributeCount = attribute_storage.back().size(),
                .attributes = attribute_storage.back().data(),
            });
        }

        std::vector<WGPUColorTargetState> color_targets;
        std::vector<WGPUBlendState> blend_states;
        color_targets.reserve(desc.color_targets.size());
        blend_states.reserve(desc.color_targets.size());
        for (const rhi::ColorTargetState &target : desc.color_targets) {
            const WGPUTextureFormat format = to_wgpu(target.format);
            if (format == WGPUTextureFormat_Undefined) {
                return std::unexpected(webgpu_error(
                    "create_render_pipeline", "a color target uses a format with no WebGPU equivalent"));
            }
            blend_states.push_back(WGPUBlendState{
                .color = to_wgpu_blend(target.color),
                .alpha = to_wgpu_blend(target.alpha),
            });
            color_targets.push_back(WGPUColorTargetState{
                .nextInChain = nullptr,
                .format = format,
                // Left null when blending is off; WebGPU reads a present blend state as enabled.
                .blend = target.blend_enable ? &blend_states.back() : nullptr,
                .writeMask = static_cast<WGPUColorWriteMask>(target.write_mask),
            });
        }

        WGPUFragmentState fragment{};
        WGPUShaderModule *fragment_module = nullptr;
        if (desc.fragment.module.value != 0) {
            fragment_module = shader_modules_.find(desc.fragment.module);
            if (fragment_module == nullptr) {
                return std::unexpected(webgpu_error("create_render_pipeline", "unknown fragment shader module"));
            }
            fragment.module = *fragment_module;
            fragment.entryPoint = wgpu_string(desc.fragment.entry_point);
            fragment.targetCount = color_targets.size();
            fragment.targets = color_targets.data();
        }

        WGPUDepthStencilState depth_stencil{};
        const bool has_depth = desc.depth_stencil.format != rhi::Format::Undefined;
        if (has_depth) {
            depth_stencil.format = to_wgpu(desc.depth_stencil.format);
            depth_stencil.depthWriteEnabled =
                desc.depth_stencil.depth_write_enable ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            // WebGPU has no separate depth-test enable: a comparison of Always with writes off is
            // how "no depth test" is spelled.
            depth_stencil.depthCompare = desc.depth_stencil.depth_test_enable
                                             ? to_wgpu(desc.depth_stencil.depth_compare)
                                             : WGPUCompareFunction_Always;
            depth_stencil.stencilFront = to_wgpu_stencil(desc.depth_stencil.stencil_front);
            depth_stencil.stencilBack = to_wgpu_stencil(desc.depth_stencil.stencil_back);
            depth_stencil.stencilReadMask = desc.depth_stencil.stencil_read_mask;
            depth_stencil.stencilWriteMask = desc.depth_stencil.stencil_write_mask;
            depth_stencil.depthBias = static_cast<i32>(desc.rasterization.depth_bias_constant);
            depth_stencil.depthBiasSlopeScale = desc.rasterization.depth_bias_slope_scale;
            depth_stencil.depthBiasClamp = desc.rasterization.depth_bias_clamp;
        }

        WGPURenderPipelineDescriptor pipeline_desc{};
        pipeline_desc.label = wgpu_string(desc.label);
        pipeline_desc.layout = *layout;
        pipeline_desc.vertex.module = *vertex_module;
        pipeline_desc.vertex.entryPoint = wgpu_string(desc.vertex.entry_point);
        pipeline_desc.vertex.bufferCount = vertex_layouts.size();
        pipeline_desc.vertex.buffers = vertex_layouts.data();
        pipeline_desc.primitive.topology = to_wgpu(desc.topology);
        // Strip topologies need their index format up front so the driver knows what value restarts
        // a strip; list topologies must leave it Undefined.
        pipeline_desc.primitive.stripIndexFormat =
            (desc.topology == rhi::PrimitiveTopology::LineStrip ||
             desc.topology == rhi::PrimitiveTopology::TriangleStrip)
                ? WGPUIndexFormat_Uint32
                : WGPUIndexFormat_Undefined;
        pipeline_desc.primitive.frontFace = to_wgpu(desc.rasterization.front_face);
        pipeline_desc.primitive.cullMode = to_wgpu(desc.rasterization.cull_mode);
        pipeline_desc.primitive.unclippedDepth = desc.rasterization.depth_clamp_enable ? 1u : 0u;
        pipeline_desc.depthStencil = has_depth ? &depth_stencil : nullptr;
        pipeline_desc.multisample.count = static_cast<u32>(desc.multisample.samples);
        pipeline_desc.multisample.mask = desc.multisample.sample_mask;
        pipeline_desc.multisample.alphaToCoverageEnabled =
            desc.multisample.alpha_to_coverage_enable ? 1u : 0u;
        pipeline_desc.fragment = fragment_module != nullptr ? &fragment : nullptr;

        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &pipeline_desc);
        if (pipeline == nullptr) {
            return std::unexpected(webgpu_error("create_render_pipeline"));
        }
        return render_pipelines_.insert(std::move(pipeline));
    }

    /// Destroys a render pipeline.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept {
        render_pipelines_.erase(handle, [](WGPURenderPipeline &p) { wgpuRenderPipelineRelease(p); });
    }

    /// Resolves a render pipeline handle to the Dawn pipeline behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPURenderPipeline WebGpuDevice::lookup_render_pipeline(rhi::RenderPipelineHandle handle) noexcept {
        WGPURenderPipeline *pipeline = render_pipelines_.find(handle);
        return pipeline != nullptr ? *pipeline : nullptr;
    }

    /// Creates a compute pipeline.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::ComputePipelineHandle> WebGpuDevice::create_compute_pipeline(
        const rhi::ComputePipelineDesc &desc) {
        WGPUPipelineLayout *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return std::unexpected(webgpu_error("create_compute_pipeline", "unknown pipeline layout handle"));
        }
        WGPUShaderModule *module = shader_modules_.find(desc.compute.module);
        if (module == nullptr) {
            return std::unexpected(webgpu_error("create_compute_pipeline", "unknown compute shader module"));
        }

        WGPUComputePipelineDescriptor pipeline_desc{};
        pipeline_desc.label = wgpu_string(desc.label);
        pipeline_desc.layout = *layout;
        pipeline_desc.compute.module = *module;
        pipeline_desc.compute.entryPoint = wgpu_string(desc.compute.entry_point);

        WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device_, &pipeline_desc);
        if (pipeline == nullptr) {
            return std::unexpected(webgpu_error("create_compute_pipeline"));
        }
        return compute_pipelines_.insert(std::move(pipeline));
    }

    /// Destroys a compute pipeline.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept {
        compute_pipelines_.erase(handle, [](WGPUComputePipeline &p) { wgpuComputePipelineRelease(p); });
    }

    /// Resolves a compute pipeline handle to the Dawn pipeline behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUComputePipeline WebGpuDevice::lookup_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept {
        WGPUComputePipeline *pipeline = compute_pipelines_.find(handle);
        return pipeline != nullptr ? *pipeline : nullptr;
    }

} // namespace SFT::Core::WebGpu
