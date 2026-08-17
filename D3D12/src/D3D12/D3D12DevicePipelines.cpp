// Render (vertex-input and mesh-shader) and compute pipeline state objects.
//
// ─── Vertex input semantic ABI ───────────────────────────────────────────────────────────────────
//
// D3D12's input assembler matches vertex attributes to shader inputs by HLSL *semantic name and
// index*, where the RHI (like Vulkan/SPIR-V) identifies them by a numeric `shader_location`. The
// bridge between the two has to be a stated convention, and the one used here is the same one every
// other portable D3D12 backend settled on: location N becomes semantic `TEXCOORD` index N. Shaders
// compiled for this backend must therefore declare their vertex inputs as `TEXCOORDn` — which is what
// Slang emits for a `[[vk::location(n)]]`-annotated input when targeting HLSL/DXIL, so in practice
// nothing at the shader level has to change between the two backends.
#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {

        [[nodiscard]] D3D12_SHADER_BYTECODE to_bytecode(const ShaderModuleRecord *module) noexcept {
            if (module == nullptr || module->bytecode.empty()) {
                return D3D12_SHADER_BYTECODE{nullptr, 0};
            }
            return D3D12_SHADER_BYTECODE{module->bytecode.data(), module->bytecode.size()};
        }

        [[nodiscard]] D3D12_RENDER_TARGET_BLEND_DESC to_blend_desc(const rhi::ColorTargetState &target) noexcept {
            return D3D12_RENDER_TARGET_BLEND_DESC{
                .BlendEnable = target.blend_enable ? TRUE : FALSE,
                // No RHI vocabulary for a logic op per target (LogicOp is a device feature with no
                // descriptor field), so it stays off; blending and logic ops are mutually exclusive in
                // D3D12 anyway.
                .LogicOpEnable = FALSE,
                .SrcBlend = to_d3d12_blend(target.color.src_factor, false),
                .DestBlend = to_d3d12_blend(target.color.dst_factor, false),
                .BlendOp = to_d3d12(target.color.op),
                .SrcBlendAlpha = to_d3d12_blend(target.alpha.src_factor, true),
                .DestBlendAlpha = to_d3d12_blend(target.alpha.dst_factor, true),
                .BlendOpAlpha = to_d3d12(target.alpha.op),
                .LogicOp = D3D12_LOGIC_OP_NOOP,
                .RenderTargetWriteMask = to_d3d12_write_mask(target.write_mask),
            };
        }

        // Drains everything the debug layer stored since the matching arm_info_queue() call. Read from
        // index 0 rather than from a pre-call high-water mark: the info queue's message store is
        // bounded (1024 by default) and, once full, discards the oldest instead of growing. A
        // high-water mark taken before a failing call therefore ends up >= the post-call count, and
        // the diagnostics silently come back empty — which is exactly how a pipeline failure reduces
        // to a bare "E_INVALIDARG" with no reason attached. Clearing up front keeps indices stable
        // and scopes the messages to the one operation being reported.
        [[nodiscard]] std::string info_queue_messages(ID3D12InfoQueue *queue) {
            if (queue == nullptr) return {};
            std::string result;
            const u64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (u64 index = 0; index < count; ++index) {
                SIZE_T bytes = 0;
                if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || bytes == 0) continue;
                vector<std::byte> storage(bytes);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                if (FAILED(queue->GetMessage(index, message, &bytes)) || message->pDescription == nullptr) continue;
                if (!result.empty()) result += " ";
                result.append(message->pDescription, message->DescriptionByteLength);
            }
            return result;
        }

        // Clears the debug layer's stored messages so a following failure reports only its own.
        void arm_info_queue(ID3D12InfoQueue *queue) noexcept {
            if (queue != nullptr) queue->ClearStoredMessages();
        }

        [[nodiscard]] D3D12_DEPTH_STENCILOP_DESC to_stencil_desc(const rhi::StencilFaceState &face) noexcept {
            return D3D12_DEPTH_STENCILOP_DESC{
                .StencilFailOp = to_d3d12(face.fail_op),
                .StencilDepthFailOp = to_d3d12(face.depth_fail_op),
                .StencilPassOp = to_d3d12(face.pass_op),
                .StencilFunc = to_d3d12(face.compare),
            };
        }

    } // namespace

    rhi::RhiExpected<rhi::RenderPipelineHandle> D3D12Device::create_render_pipeline(
        const rhi::RenderPipelineDesc &desc) {
        ZoneScopedN("D3D12Device::create_render_pipeline");

        const PipelineLayoutRecord *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return invalid_argument("create_render_pipeline: unknown pipeline layout handle.");
        }
        ComPtr<ID3D12Device2> device2;
        if (FAILED(device_.As(&device2))) {
            return unsupported("create_render_pipeline: this runtime does not provide ID3D12Device2::CreatePipelineState.");
        }

        const bool is_mesh_pipeline = desc.mesh.module.is_valid();
        if (is_mesh_pipeline && !enabled_features_.has(rhi::Feature::MeshShader)) {
            return unsupported("create_render_pipeline: a mesh-shader pipeline was requested but Feature::MeshShader "
                               "is not enabled on this device.");
        }
        if (!is_mesh_pipeline && !desc.vertex.module.is_valid()) {
            return invalid_argument("create_render_pipeline: neither a vertex nor a mesh entry point was supplied.");
        }

        // Every ShaderEntry's module must still exist; a missing one is a caller error rather than
        // something to silently compile a stage-less pipeline from.
        const auto module_for = [&](const rhi::ShaderEntry &entry) -> const ShaderModuleRecord * {
            return entry.module.is_valid() ? shader_modules_.find(entry.module) : nullptr;
        };
        const ShaderModuleRecord *vertex = module_for(desc.vertex);
        const ShaderModuleRecord *task = module_for(desc.task);
        const ShaderModuleRecord *mesh = module_for(desc.mesh);
        const ShaderModuleRecord *fragment = module_for(desc.fragment);
        if ((desc.vertex.module.is_valid() && vertex == nullptr) || (desc.task.module.is_valid() && task == nullptr) ||
            (desc.mesh.module.is_valid() && mesh == nullptr) ||
            (desc.fragment.module.is_valid() && fragment == nullptr)) {
            return invalid_argument("create_render_pipeline: a shader entry names an unknown shader module.");
        }

        // A DXIL library carries its own entry-point name, and D3D12 has no per-PSO entry point
        // selector the way Vulkan's VkPipelineShaderStageCreateInfo::pName does. Core/Slang must
        // therefore emit one module per entry point for this backend; `entry_point` is retained in the
        // descriptor for the Vulkan path and is not consulted here.
        (void)desc.vertex.entry_point;

        vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
        if (!is_mesh_pipeline) {
            for (u32 slot = 0; slot < desc.vertex_buffers.size(); ++slot) {
                const rhi::VertexBufferLayout &buffer_layout = desc.vertex_buffers[slot];
                for (const rhi::VertexAttribute &attribute : buffer_layout.attributes) {
                    input_elements.push_back(D3D12_INPUT_ELEMENT_DESC{
                        .SemanticName = "TEXCOORD", // see this file's header comment
                        .SemanticIndex = attribute.shader_location,
                        .Format = to_dxgi(attribute.format),
                        .InputSlot = slot,
                        .AlignedByteOffset = attribute.offset,
                        .InputSlotClass = buffer_layout.step_mode == rhi::VertexStepMode::Instance
                                              ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                              : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                        // D3D12 folds Vulkan's per-instance "divisor" into this field: 0 for per-vertex
                        // data, 1 for the ordinary once-per-instance step the RHI's VertexStepMode
                        // describes.
                        .InstanceDataStepRate =
                            buffer_layout.step_mode == rhi::VertexStepMode::Instance ? 1u : 0u,
                    });
                }
            }
        }

        D3D12_RT_FORMAT_ARRAY rtv_formats{};
        rtv_formats.NumRenderTargets = static_cast<UINT>(
            std::min<usize>(desc.color_targets.size(), D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT));
        CD3DX12_BLEND_DESC blend{D3D12_DEFAULT};
        blend.AlphaToCoverageEnable = desc.multisample.alpha_to_coverage_enable ? TRUE : FALSE;
        // Set whenever there is more than one target: without it D3D12 applies RenderTarget[0]'s blend
        // state to every attachment, silently discarding per-target state the RHI does let a caller
        // specify.
        blend.IndependentBlendEnable = desc.color_targets.size() > 1 ? TRUE : FALSE;
        for (u32 index = 0; index < rtv_formats.NumRenderTargets; ++index) {
            rtv_formats.RTFormats[index] = to_dxgi_view_format(desc.color_targets[index].format);
            blend.RenderTarget[index] = to_blend_desc(desc.color_targets[index]);
        }

        CD3DX12_RASTERIZER_DESC rasterizer{D3D12_DEFAULT};
        rasterizer.FillMode = to_d3d12(desc.rasterization.polygon_mode);
        rasterizer.CullMode = to_d3d12(desc.rasterization.cull_mode);
        // Inverted on purpose — do not "fix" this to the obvious 1:1 mapping.
        //
        // The RHI's Vulkan-style clip space is reconciled on D3D12 by negating clip-space Y. For the
        // main render path (gbuffer/forward geometry, UI) that negation is now baked into C++-side
        // matrices/constants before they ever reach a shader (RHI::gpu_clip_flip /
        // RHI::gpu_clip_y_sign, see Renderer::render_frame and Renderer::prepare_scene_gpu_data);
        // shadow-pass and spectral-path-tracer shaders still do it themselves via
        // sturdy_clip_position() in sturdy_common.slang (pending a follow-up migration). Either way,
        // the effective clip-space Y negation is the same mirror, and it reverses how the D3D12
        // rasterizer classifies a triangle's winding — measured, not assumed: the same triangle that
        // Vulkan reports as clockwise is only drawn here when FrontCounterClockwise is TRUE. Passing
        // front_face through unchanged silently culls exactly the faces that should be visible, which
        // looks like correct geometry with random pieces missing.
        rasterizer.FrontCounterClockwise =
            desc.rasterization.front_face == rhi::FrontFace::Clockwise ? TRUE : FALSE;
        rasterizer.DepthBias = static_cast<INT>(desc.rasterization.depth_bias_constant);
        rasterizer.DepthBiasClamp = desc.rasterization.depth_bias_clamp;
        rasterizer.SlopeScaledDepthBias = desc.rasterization.depth_bias_slope_scale;
        // D3D12 spells depth clamping as the *absence* of depth clipping; there is no separate clamp
        // enable, so the two are inverses of each other rather than independent knobs.
        rasterizer.DepthClipEnable = desc.rasterization.depth_clamp_enable ? FALSE : TRUE;
        rasterizer.MultisampleEnable = desc.multisample.samples != rhi::SampleCount::X1 ? TRUE : FALSE;
        // RasterizationState::line_width has no D3D12 equivalent at all (line width is fixed at 1.0);
        // Feature::WideLines is correspondingly never reported supported.
        (void)desc.rasterization.line_width;

        CD3DX12_DEPTH_STENCIL_DESC1 depth_stencil{D3D12_DEFAULT};
        depth_stencil.DepthEnable = desc.depth_stencil.depth_test_enable ? TRUE : FALSE;
        depth_stencil.DepthWriteMask = desc.depth_stencil.depth_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                             : D3D12_DEPTH_WRITE_MASK_ZERO;
        depth_stencil.DepthFunc = to_d3d12(desc.depth_stencil.depth_compare);
        depth_stencil.StencilEnable = desc.depth_stencil.stencil_test_enable ? TRUE : FALSE;
        depth_stencil.StencilReadMask = desc.depth_stencil.stencil_read_mask;
        depth_stencil.StencilWriteMask = desc.depth_stencil.stencil_write_mask;
        depth_stencil.FrontFace = to_stencil_desc(desc.depth_stencil.stencil_front);
        depth_stencil.BackFace = to_stencil_desc(desc.depth_stencil.stencil_back);
        depth_stencil.DepthBoundsTestEnable = FALSE;

        // View instancing is D3D12's multiview: each set view-mask bit becomes one view rendered into
        // the matching render-target array slice, which is precisely what RenderPipelineDesc::view_mask
        // describes.
        vector<D3D12_VIEW_INSTANCE_LOCATION> view_locations;
        if (desc.view_mask != 0) {
            if (!enabled_features_.has(rhi::Feature::Multiview)) {
                return unsupported("create_render_pipeline: a view mask was supplied but Feature::Multiview is not "
                                   "enabled on this device.");
            }
            for (u32 bit = 0; bit < 32; ++bit) {
                if ((desc.view_mask & (1u << bit)) != 0) {
                    view_locations.push_back(D3D12_VIEW_INSTANCE_LOCATION{.ViewportArrayIndex = 0,
                                                                          .RenderTargetArrayIndex = bit});
                }
            }
        }
        const D3D12_VIEW_INSTANCING_DESC view_instancing{
            .ViewInstanceCount = static_cast<UINT>(view_locations.size()),
            .pViewInstanceLocations = view_locations.empty() ? nullptr : view_locations.data(),
            .Flags = D3D12_VIEW_INSTANCING_FLAG_NONE,
        };

        CD3DX12_PIPELINE_STATE_STREAM2 stream{};
        stream.pRootSignature = layout->root_signature.Get();
        stream.PrimitiveTopologyType = to_d3d12_topology_type(desc.topology);
        stream.BlendState = blend;
        stream.RasterizerState = rasterizer;
        stream.DepthStencilState = depth_stencil;
        stream.DSVFormat = to_dxgi_view_format(desc.depth_stencil.format);
        stream.RTVFormats = rtv_formats;
        stream.SampleDesc = DXGI_SAMPLE_DESC{static_cast<UINT>(desc.multisample.samples), 0};
        stream.SampleMask = desc.multisample.sample_mask;
        stream.PS = to_bytecode(fragment);
        stream.ViewInstancingDesc = CD3DX12_VIEW_INSTANCING_DESC(view_instancing);

        if (is_mesh_pipeline) {
            stream.MS = to_bytecode(mesh);
            if (task != nullptr) {
                if (!enabled_features_.has(rhi::Feature::TaskShader)) {
                    return unsupported("create_render_pipeline: a task/amplification stage was supplied but "
                                       "Feature::TaskShader is not enabled on this device.");
                }
                stream.AS = to_bytecode(task);
            }
        } else {
            stream.VS = to_bytecode(vertex);
            stream.InputLayout = D3D12_INPUT_LAYOUT_DESC{
                input_elements.empty() ? nullptr : input_elements.data(),
                static_cast<UINT>(input_elements.size()),
            };
            // Left disabled deliberately. D3D12 bakes the primitive-restart cut value into the PSO
            // (0xFFFF vs 0xFFFFFFFF), but the index format is dynamic state set per draw by
            // set_index_buffer(), so the correct value is not knowable here. Rather than guess — and
            // silently cut strips at the wrong index for one of the two formats — restart stays off,
            // and Feature::PrimitiveRestart is correspondingly not reported supported by this backend.
            stream.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        }

        const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        RenderPipelineRecord record{};
        record.layout = desc.layout;
        record.topology = to_d3d12_topology(desc.topology);
        record.vertex_strides.reserve(desc.vertex_buffers.size());
        for (const rhi::VertexBufferLayout &buffer_layout : desc.vertex_buffers) {
            if (buffer_layout.stride == 0 || buffer_layout.stride > std::numeric_limits<UINT>::max()) {
                return invalid_argument("create_render_pipeline: every vertex-buffer stride must fit in a non-zero D3D12 UINT.");
            }
            record.vertex_strides.push_back(static_cast<u32>(buffer_layout.stride));
        }
        record.is_mesh_pipeline = is_mesh_pipeline;
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(device_.As(&info_queue))) arm_info_queue(info_queue.Get());
        if (const HRESULT hr = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&record.pipeline));
            FAILED(hr)) {
            std::string operation = "create_render_pipeline (CreatePipelineState)";
            const std::string diagnostics = info_queue_messages(info_queue.Get());
            if (!diagnostics.empty()) operation += ": " + diagnostics;
            return hresult_error(hr, operation);
        }
        set_debug_name(record.pipeline.Get(), desc.label);
        return render_pipelines_.insert(std::move(record));
    }

    void D3D12Device::destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept {
        render_pipelines_.erase(handle);
    }

    rhi::RhiExpected<rhi::ComputePipelineHandle> D3D12Device::create_compute_pipeline(
        const rhi::ComputePipelineDesc &desc) {
        ZoneScopedN("D3D12Device::create_compute_pipeline");

        const PipelineLayoutRecord *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return invalid_argument("create_compute_pipeline: unknown pipeline layout handle.");
        }
        const ShaderModuleRecord *compute = shader_modules_.find(desc.compute.module);
        if (compute == nullptr) {
            return invalid_argument("create_compute_pipeline: unknown compute shader module.");
        }

        const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
            .pRootSignature = layout->root_signature.Get(),
            .CS = to_bytecode(compute),
            .NodeMask = 0,
            .CachedPSO = {nullptr, 0},
            .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
        };

        ComputePipelineRecord record{};
        record.layout = desc.layout;
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(device_.As(&info_queue))) arm_info_queue(info_queue.Get());
        if (const HRESULT hr = device_->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&record.pipeline));
            FAILED(hr)) {
            std::string operation = "create_compute_pipeline (CreateComputePipelineState)";
            const std::string diagnostics = info_queue_messages(info_queue.Get());
            if (!diagnostics.empty()) operation += ": " + diagnostics;
            return hresult_error(hr, operation);
        }
        set_debug_name(record.pipeline.Get(), desc.label);
        return compute_pipelines_.insert(std::move(record));
    }

    void D3D12Device::destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept {
        compute_pipelines_.erase(handle);
    }

} // namespace SFT::D3D12
