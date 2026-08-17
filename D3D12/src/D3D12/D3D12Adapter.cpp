#include <D3D12/D3D12Adapter.hpp>
#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <directx/d3d12sdklayers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#pragma endregion

namespace SFT::D3D12 {

    namespace {

        // The lowest feature level this backend targets. 11_0 is the D3D12 floor, but every capability
        // the RHI treats as *base* (dynamic-rendering-equivalent rendering, compute, timeline sync via
        // fences, the full common format set) is available there, so nothing is gained by demanding
        // 12_0 and it would exclude otherwise-working hardware.
        constexpr D3D_FEATURE_LEVEL minimum_feature_level = D3D_FEATURE_LEVEL_11_0;

        [[nodiscard]] const char *vendor_name(u32 vendor_id) noexcept {
            switch (vendor_id) {
                case 0x1002:
                    return "AMD";
                case 0x10DE:
                    return "NVIDIA";
                case 0x8086:
                    return "Intel";
                case 0x1414:
                    return "Microsoft"; // WARP / the software adapter
                case 0x13B5:
                    return "ARM";
                case 0x5143:
                    return "Qualcomm";
                default:
                    return "Unknown";
            }
        }

        [[nodiscard]] std::string narrow(const wchar_t *wide) {
            std::string result;
            for (const wchar_t *cursor = wide; cursor != nullptr && *cursor != L'\0'; ++cursor) {
                result.push_back(*cursor < 128 ? static_cast<char>(*cursor) : '?');
            }
            return result;
        }

        [[nodiscard]] std::string format_driver_version(LARGE_INTEGER umd) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(umd.HighPart)), static_cast<unsigned>(LOWORD(umd.HighPart)), static_cast<unsigned>(HIWORD(umd.LowPart)), static_cast<unsigned>(LOWORD(umd.LowPart)));
            return buffer;
        }

        [[nodiscard]] std::string physical_device_id(LUID luid) {
            static_assert(sizeof(luid) == sizeof(u64));
            u64 bits = 0;
            std::memcpy(&bits, &luid, sizeof(bits));
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "windows-luid:%016llx", static_cast<unsigned long long>(bits));
            return buffer;
        }

        [[nodiscard]] const char *shader_model_name(D3D_SHADER_MODEL model) noexcept {
            switch (model) {
                case D3D_SHADER_MODEL_6_9:
                    return "Shader Model 6.9";
                case D3D_SHADER_MODEL_6_8:
                    return "Shader Model 6.8";
                case D3D_SHADER_MODEL_6_7:
                    return "Shader Model 6.7";
                case D3D_SHADER_MODEL_6_6:
                    return "Shader Model 6.6";
                case D3D_SHADER_MODEL_6_5:
                    return "Shader Model 6.5";
                case D3D_SHADER_MODEL_6_4:
                    return "Shader Model 6.4";
                case D3D_SHADER_MODEL_6_3:
                    return "Shader Model 6.3";
                case D3D_SHADER_MODEL_6_2:
                    return "Shader Model 6.2";
                case D3D_SHADER_MODEL_6_1:
                    return "Shader Model 6.1";
                default:
                    return "Shader Model 6.0";
            }
        }

        // CheckFeatureSupport with the boilerplate removed. Returns false (leaving `data` at whatever
        // the caller zero-initialized it to) when the runtime doesn't know the feature at all, which is
        // exactly how an older runtime reports "this options structure postdates me".
        template <typename Data>
        [[nodiscard]] bool check_feature(ID3D12Device *device, D3D12_FEATURE feature, Data &data) noexcept {
            return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
        }

        // The highest shader model the runtime *and* driver both support. The query is documented to be
        // asked highest-first and to fail down to a supported value, which is why this walks a list
        // rather than asking once.
        [[nodiscard]] D3D_SHADER_MODEL highest_shader_model(ID3D12Device *device) noexcept {
            constexpr D3D_SHADER_MODEL candidates[] = {
                D3D_SHADER_MODEL_6_9,
                D3D_SHADER_MODEL_6_8,
                D3D_SHADER_MODEL_6_7,
                D3D_SHADER_MODEL_6_6,
                D3D_SHADER_MODEL_6_5,
                D3D_SHADER_MODEL_6_4,
                D3D_SHADER_MODEL_6_3,
                D3D_SHADER_MODEL_6_2,
                D3D_SHADER_MODEL_6_1,
                D3D_SHADER_MODEL_6_0,
            };
            for (D3D_SHADER_MODEL candidate : candidates) {
                D3D12_FEATURE_DATA_SHADER_MODEL data{candidate};
                if (check_feature(device, D3D12_FEATURE_SHADER_MODEL, data) && data.HighestShaderModel >= candidate) {
                    return data.HighestShaderModel;
                }
            }
            return D3D_SHADER_MODEL_6_0;
        }

        void fill_sample_counts(ID3D12Device *device, rhi::DeviceLimits &limits) noexcept {
            // Probed against RGBA8Unorm: the RHI's limit is a single device-wide figure, and this is
            // the format every backend's "how much MSAA can I ask for" answer is conventionally
            // quoted against. Per-format support still has to be queried per-format by anyone who
            // needs it — which is exactly why DeviceLimits documents this as a hint, not a guarantee.
            u32 mask = 0;
            u32 highest = 1;
            for (u32 count : {1u, 2u, 4u, 8u, 16u}) {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data{
                    .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                    .SampleCount = count,
                    .Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,
                    .NumQualityLevels = 0,
                };
                if (check_feature(device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, data) &&
                    data.NumQualityLevels > 0) {
                    mask |= count;
                    highest = std::max(highest, count);
                }
            }
            limits.framebuffer_sample_counts = mask == 0 ? 1u : mask;
            limits.max_framebuffer_sample_count = highest;
        }

    } // namespace

    DeviceCapabilities probe_capabilities(ID3D12Device *device) {
        DeviceCapabilities caps{};
        rhi::FeatureSet &features = caps.features;

        // ── Baseline: capabilities D3D12 guarantees on every conformant device ──
        //
        // These are not assumptions. Timeline synchronization is what an ID3D12Fence *is*; there is no
        // render-pass object to opt out of, so the RHI's dynamic-rendering model is the only model;
        // and BC texture compression, cube arrays, 32-bit indices, indirect draws, and occlusion/
        // timestamp queries are all core D3D12 with no capability bit to query.
        for (rhi::Feature feature : {
                 rhi::Feature::TimelineSynchronization,
                 rhi::Feature::DynamicRendering,
                 rhi::Feature::TextureCompressionBC,
                 rhi::Feature::ImageCubeArray,
                 rhi::Feature::FullDrawIndexUint32,
                 // Feature::PrimitiveRestart is deliberately absent: D3D12 bakes the strip-cut index
                 // into the PSO while the index format is per-draw dynamic state, so the correct cut
                 // value is unknowable at pipeline creation. See create_render_pipeline()'s own
                 // comment on IBStripCutValue.
                 rhi::Feature::MultiDrawIndirect,
                 rhi::Feature::DrawIndirectFirstInstance,
                 rhi::Feature::DrawIndirectCount,
                 rhi::Feature::OcclusionQueries,
                 rhi::Feature::PreciseOcclusionQueries,
                 rhi::Feature::TimestampQueries,
                 rhi::Feature::PipelineStatisticsQueries,
                 // D3D12 query heaps have no "reset" step at all: a slot is overwritten by the next
                 // Begin/EndQuery. Reporting HostQueryReset supported is accurate — the host-side reset
                 // the feature names is trivially available (and a no-op) — and lets a caller skip the
                 // per-frame reset command the Vulkan path needs.
                 rhi::Feature::HostQueryReset,
                 rhi::Feature::AnisotropicFiltering,
                 rhi::Feature::IndependentBlend,
                 rhi::Feature::SampleRateShading,
                 rhi::Feature::MultiViewport,
                 rhi::Feature::DepthClamp,
                 rhi::Feature::DepthBiasClamp,
                 rhi::Feature::WireframeFill,
                 rhi::Feature::ShaderDrawParameters,
                 rhi::Feature::ShaderClipDistance,
                 rhi::Feature::ShaderCullDistance,
                 rhi::Feature::ShaderImageGatherExtended,
                 rhi::Feature::VertexPipelineStoresAndAtomics,
                 rhi::Feature::FragmentStoresAndAtomics,
                 rhi::Feature::StorageImageExtendedFormats,
                 rhi::Feature::StorageImageWriteWithoutFormat,
                 rhi::Feature::SeparateDepthStencilLayouts,
                 rhi::Feature::BufferDeviceAddress,
                 rhi::Feature::CopyCommands2,
                 rhi::Feature::ExtendedDynamicState,
                 rhi::Feature::AsyncCompute,
                 rhi::Feature::AsyncTransfer,
                 rhi::Feature::SwapchainColorspace,
                 rhi::Feature::HdrMetadata,
                 rhi::Feature::HdrOutput,
                 rhi::Feature::DriverProperties,
             }) {
            features.set(feature);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options0{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS, options0)) {
            if (options0.DoublePrecisionFloatShaderOps != FALSE) {
                features.set(rhi::Feature::ShaderFloat64);
            }
            if (options0.PSSpecifiedStencilRefSupported != FALSE) {
                features.set(rhi::Feature::ShaderStencilExport);
            }
            if (options0.ROVsSupported != FALSE) {
                features.set(rhi::Feature::FragmentShaderInterlock);
            }
            if (options0.TypedUAVLoadAdditionalFormats != FALSE) {
                features.set(rhi::Feature::StorageImageReadWithoutFormat);
            }

            // Tier 2 supports the table-backed descriptor arrays and shader indexing this backend
            // exposes. Bind groups are immutable after creation, so update-after-bind is deliberately
            // not included. The scalar RHI limit must also be safe for sampler-bearing bindings.
            if (options0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2) {
                for (rhi::Feature feature : {rhi::Feature::DescriptorIndexing,
                                             rhi::Feature::RuntimeDescriptorArrays,
                                             rhi::Feature::DescriptorBindingPartiallyBound,
                                             rhi::Feature::DescriptorBindingVariableCount,
                                             rhi::Feature::SampledImageArrayDynamicIndexing,
                                             rhi::Feature::UniformBufferArrayDynamicIndexing,
                                             rhi::Feature::StorageBufferArrayDynamicIndexing,
                                             rhi::Feature::StorageImageArrayDynamicIndexing}) {
                    features.set(feature);
                }
                caps.properties.descriptor_indexing.max_variable_descriptor_count =
                    D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
            }
            if (options0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3) {
                for (rhi::Feature feature : {rhi::Feature::BindlessResources,
                                             rhi::Feature::NonUniformResourceIndexing,
                                             rhi::Feature::SampledImageArrayNonUniformIndexing,
                                             rhi::Feature::UniformBufferArrayNonUniformIndexing,
                                             rhi::Feature::StorageBufferArrayNonUniformIndexing,
                                             rhi::Feature::StorageImageArrayNonUniformIndexing}) {
                    features.set(feature);
                }
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS1, options1)) {
            if (options1.WaveOps != FALSE) {
                features.set(rhi::Feature::SubgroupOperations);
                features.set(rhi::Feature::SubgroupExtendedTypes);
                caps.properties.subgroup.min_subgroup_size = options1.WaveLaneCountMin;
                caps.properties.subgroup.max_subgroup_size = options1.WaveLaneCountMax;
            }
            if (options1.Int64ShaderOps != FALSE) {
                features.set(rhi::Feature::ShaderInt64);
            }
        }


        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS3, options3)) {
            if (options3.BarycentricsSupported != FALSE) {
                features.set(rhi::Feature::FragmentShaderBarycentric);
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS4, options4)) {
            if (options4.Native16BitShaderOpsSupported != FALSE) {
                for (rhi::Feature feature : {rhi::Feature::ShaderFloat16, rhi::Feature::ShaderInt16, rhi::Feature::Shader16BitStorage}) {
                    features.set(feature);
                }
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS5, options5)) {
            if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
                features.set(rhi::Feature::AccelerationStructures);
                features.set(rhi::Feature::RayTracingPipeline);
                // DXR's fixed limits — the spec's, not a per-device query, because D3D12 exposes no
                // equivalent of VkPhysicalDeviceRayTracingPipelinePropertiesKHR. The portable base
                // alignment applies to the shader-table start, not merely each shader record's stride.
                caps.properties.ray_tracing.max_ray_recursion_depth = D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH;
                caps.properties.ray_tracing.shader_group_handle_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
                caps.properties.ray_tracing.shader_group_base_alignment =
                    D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
                caps.properties.ray_tracing.max_ray_hit_attribute_size =
                    D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;
                caps.properties.ray_tracing.min_acceleration_structure_scratch_offset_alignment =
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
            }
            if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) {
                features.set(rhi::Feature::RayQuery);
                features.set(rhi::Feature::RayTraversalPrimitiveCulling);
            }
        }


        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS7, options7) &&
            options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1) {
            features.set(rhi::Feature::MeshShader);
            features.set(rhi::Feature::TaskShader);
            // DX12 mesh/amplification shader model limits are fixed by the API specification.
            caps.properties.mesh_shader.max_task_work_group_invocations = 128;
            caps.properties.mesh_shader.max_mesh_work_group_invocations = 128;
            caps.properties.mesh_shader.max_mesh_output_vertices = 256;
            caps.properties.mesh_shader.max_mesh_output_primitives = 256;
            caps.properties.mesh_shader.max_mesh_payload_size = 16 * 1024;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS9, options9)) {

            if (options9.AtomicInt64OnTypedResourceSupported != FALSE) {
                features.set(rhi::Feature::ShaderImageInt64Atomics);
                features.set(rhi::Feature::ShaderBufferInt64Atomics);
            }
            if (options9.AtomicInt64OnGroupSharedSupported != FALSE) {
                features.set(rhi::Feature::ShaderSharedInt64Atomics);
            }
            if (options9.DerivativesInMeshAndAmplificationShadersSupported != FALSE) {
                features.set(rhi::Feature::ComputeShaderDerivatives);
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS12, options12)) {
            caps.enhanced_barriers = options12.EnhancedBarriersSupported != FALSE;
            if (caps.enhanced_barriers) {
                // Only with enhanced barriers does this backend actually implement the
                // (stage, access) -> (stage, access) + layout model :Barrier specifies. On the legacy
                // fallback the barriers still work, but the fine-grained scoping is collapsed, so
                // reporting Synchronization2 there would be a claim the backend cannot honor.
                features.set(rhi::Feature::Synchronization2);
                features.set(rhi::Feature::UnifiedImageLayouts);
            }
            if (options12.RelaxedFormatCastingSupported != FALSE) {
                features.set(rhi::Feature::SwapchainMutableFormat);
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS14, options14)) {
            if (options14.WriteableMSAATexturesSupported != FALSE) {
                features.set(rhi::Feature::StorageImageMultisample);
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
        if (check_feature(device, D3D12_FEATURE_D3D12_OPTIONS16, options16)) {
            if (options16.DynamicDepthBiasSupported != FALSE) {
                features.set(rhi::Feature::DepthBiasControl);
                features.set(rhi::Feature::ExtendedDynamicState2);
            }
            // ReBAR-backed heap: CPU-mappable *and* GPU-local. Only reported true on hardware/OS
            // combinations that actually back it with Resizable BAR, so this is a real capability
            // probe, not just an SDK-header check. create_buffer() is the consumer.
            caps.gpu_upload_heap_supported = options16.GPUUploadHeapSupported != FALSE;
        }

        D3D12_FEATURE_DATA_SHADER_CACHE shader_cache{};
        if (check_feature(device, D3D12_FEATURE_SHADER_CACHE, shader_cache)) {
            caps.pipeline_library_supported =
                (shader_cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) != 0;
        }

        const D3D_SHADER_MODEL shader_model = highest_shader_model(device);
        if (shader_model >= D3D_SHADER_MODEL_6_2) {
            features.set(rhi::Feature::ShaderFloat16);
        }
        if (shader_model >= D3D_SHADER_MODEL_6_5) {
            features.set(rhi::Feature::SubgroupPartitioned);
        }
        if (shader_model >= D3D_SHADER_MODEL_6_6) {
            features.set(rhi::Feature::ShaderClock);
        }
        if (shader_model >= D3D_SHADER_MODEL_6_7) {
            features.set(rhi::Feature::ShaderQuadControl);
        }

        // ── Limits ──
        rhi::DeviceLimits &limits = caps.limits;
        limits.max_texture_dimension_2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        limits.max_texture_array_layers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        // Bind groups are register spaces, of which D3D12 has no hard cap — the real constraint is the
        // 64-DWORD root signature, and 8 sets' worth of descriptor tables (1 DWORD each) plus root
        // constants fits comfortably. Reported as the portable budget rather than a hardware limit.
        limits.max_bind_groups = 8;
        // Root constants share the 64-DWORD root signature budget with every descriptor table and root
        // descriptor, so the whole budget can never be spent on constants. 128 bytes (32 DWORDs) leaves
        // half the signature for bindings, and matches the push-constant size the common Vulkan
        // baseline guarantees, so a renderer sized for one fits the other.
        limits.max_push_constants_size = 128;
        limits.max_vertex_buffers = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        limits.max_vertex_attributes = D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT;
        limits.max_color_attachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
        limits.supports_bc_texture_compression = true;
        limits.supports_minimum_depth_resolve = false;
        limits.max_compute_workgroup_size_x = D3D12_CS_THREAD_GROUP_MAX_X;
        limits.max_compute_workgroup_size_y = D3D12_CS_THREAD_GROUP_MAX_Y;
        limits.max_compute_workgroup_size_z = D3D12_CS_THREAD_GROUP_MAX_Z;
        limits.min_uniform_buffer_offset_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        // Structured/raw buffer views are addressed in elements, and the strictest element alignment
        // D3D12 imposes on a raw (ByteAddress) view is 16 bytes.
        limits.min_storage_buffer_offset_alignment = 16;
        limits.timestamp_valid_bits = 64;
        fill_sample_counts(device, limits);

        // ── Queue topology ──
        // D3D12's engines are fixed and always present: DIRECT, COMPUTE, and COPY are separate
        // hardware engines on every conformant device, so async compute/transfer are topology facts
        // here rather than the negotiated maybe they are under Vulkan.
        caps.queue_infos.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Graphics,
            .capabilities = rhi::QueueCapability::Graphics | rhi::QueueCapability::Compute |
                            rhi::QueueCapability::Transfer | rhi::QueueCapability::Present,
            .lane_count = 1,
            .physical_group = 0,
            .likely_parallel_with_graphics = false,
            .dedicated = true,
            .label = "D3D12 direct queue",
        });
        caps.queue_infos.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Compute,
            .capabilities = rhi::QueueCapability::Compute | rhi::QueueCapability::Transfer,
            .lane_count = 1,
            .physical_group = 1,
            .likely_parallel_with_graphics = true,
            .dedicated = true,
            .label = "D3D12 compute queue",
        });
        caps.queue_infos.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Transfer,
            .capabilities = rhi::QueueCapability::Transfer,
            .lane_count = 1,
            .physical_group = 2,
            .likely_parallel_with_graphics = true,
            .dedicated = true,
            .label = "D3D12 copy queue",
        });

        return caps;
    }

    // ─── D3D12Adapter ────────────────────────────────────────────────────────────

    D3D12Adapter::D3D12Adapter(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter4> adapter, ComPtr<ID3D12Device> device, rhi::AdapterInfo info, DeviceCapabilities capabilities, bool debug_layer_enabled)
        : factory_(std::move(factory)), adapter_(std::move(adapter)), device_(std::move(device)),
          info_(std::move(info)), capabilities_(std::move(capabilities)),
          debug_layer_enabled_(debug_layer_enabled) {}

    const rhi::AdapterInfo &D3D12Adapter::info() const noexcept { return info_; }

    const rhi::FeatureSet &D3D12Adapter::supported_features() const noexcept { return capabilities_.features; }

    const rhi::FeatureProperties &D3D12Adapter::feature_properties() const noexcept { return capabilities_.properties; }

    span<const rhi::ExtensionId> D3D12Adapter::supported_extensions() const noexcept { return supported_extensions_; }

    span<const rhi::QueueInfo> D3D12Adapter::queue_infos() const noexcept { return capabilities_.queue_infos; }

    const rhi::DeviceLimits &D3D12Adapter::limits() const noexcept { return capabilities_.limits; }

    rhi::RhiExpected<unique_ptr<rhi::RhiDevice>> D3D12Adapter::create_device(const rhi::DeviceRequest &request) {
        const rhi::FeatureNegotiationReport report = rhi::negotiate_features(
            capabilities_.features,
            request.required_features,
            request.optional_features);
        if (!report.required_satisfied()) {
            std::string message = "D3D12 adapter cannot satisfy required features:";
            bool first = true;
            report.missing_required_features.for_each([&](rhi::Feature feature) {
                message += first ? " " : ", ";
                message += rhi::feature_name(feature);
                first = false;
            });
            return unsupported(std::move(message) + ".");
        }
        for (rhi::ExtensionId extension : request.required_extensions) {
            if (!rhi::contains_extension(supported_extensions_, extension)) {
                return unsupported(std::string("D3D12 adapter does not support required extension '") +
                                   std::string(extension.name_space) + "." + std::string(extension.name) + "'.");
            }
        }
        // Queue requests are checked against the fixed D3D12 engine topology rather than used to build
        // queues: unlike Vulkan, queues are not created at device-creation time, so a request can only
        // ever be validated here, never honored differently.
        for (const rhi::QueueRequest &queue_request : request.queue_requests) {
            const auto it = std::ranges::find_if(capabilities_.queue_infos, [&](const rhi::QueueInfo &info) {
                return info.queue == queue_request.queue;
            });
            if (it == capabilities_.queue_infos.end()) {
                return unsupported("D3D12 adapter exposes no queue for a requested queue class.");
            }
            if (queue_request.min_lanes > it->lane_count) {
                return unsupported("D3D12 adapter cannot provide the requested number of queue lanes.");
            }
            if (queue_request.require_dedicated && !it->dedicated) {
                return unsupported("D3D12 adapter cannot provide a dedicated queue for a requested queue class.");
            }
        }

        D3D12DeviceCreateInfo info{};
        info.factory = factory_;
        info.adapter = adapter_;
        info.device = device_;
        info.adapter_info = info_;
        info.limits = capabilities_.limits;
        info.feature_report = report;
        info.feature_properties = capabilities_.properties;
        info.queue_infos = capabilities_.queue_infos;
        info.enhanced_barriers = capabilities_.enhanced_barriers;
        info.debug_layer_enabled = debug_layer_enabled_;
        info.allow_tearing = capabilities_.allow_tearing;
        info.pipeline_library_supported = capabilities_.pipeline_library_supported;
        info.gpu_upload_heap_supported = capabilities_.gpu_upload_heap_supported;

        auto device = std::make_unique<D3D12Device>(std::move(info));
        if (auto initialized = device->initialize(); !initialized) {
            return std::unexpected(initialized.error());
        }
        return unique_ptr<rhi::RhiDevice>(std::move(device));
    }

    // ─── D3D12Instance ───────────────────────────────────────────────────────────

    D3D12Instance::D3D12Instance(ComPtr<IDXGIFactory6> factory, bool debug_layer_enabled, bool allow_tearing)
        : factory_(std::move(factory)), debug_layer_enabled_(debug_layer_enabled), allow_tearing_(allow_tearing) {}

    rhi::BackendType D3D12Instance::backend_type() const noexcept { return rhi::BackendType::D3D12; }

    rhi::RhiExpected<vector<unique_ptr<rhi::RhiAdapter>>> D3D12Instance::enumerate_adapters() {
        vector<unique_ptr<rhi::RhiAdapter>> adapters;

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter1;
            // EnumAdapterByGpuPreference orders by real GPU preference (the OS's own
            // discrete-vs-integrated policy, which also honors per-application overrides in Windows
            // graphics settings) rather than by bus order, so the caller's own scoring starts from a
            // sensible ordering instead of "whatever was plugged in first".
            if (factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ComPtr<IDXGIAdapter4> adapter;
            if (FAILED(adapter1.As(&adapter))) {
                continue;
            }

            DXGI_ADAPTER_DESC3 desc{};
            if (FAILED(adapter->GetDesc3(&desc))) {
                continue;
            }

            // A device has to exist to answer any capability question at all (see this file's header
            // comment), so an adapter that cannot produce one is not an adapter this backend can
            // report — skipped rather than surfaced as a broken entry.
            ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(adapter.Get(), minimum_feature_level, IID_PPV_ARGS(&device)))) {
                continue;
            }

            rhi::AdapterInfo info{};
            info.name = narrow(desc.Description);
            info.vendor = vendor_name(desc.VendorId);
            info.backend = rhi::BackendType::D3D12;
            info.vendor_id = desc.VendorId;
            info.device_id = desc.DeviceId;
            info.physical_device_id = physical_device_id(desc.AdapterLuid);

            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
            D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
            const bool unified_memory = check_feature(device.Get(), D3D12_FEATURE_ARCHITECTURE1, architecture) &&
                                        architecture.UMA != FALSE;
            info.device_type = software         ? rhi::DeviceType::Cpu
                               : unified_memory ? rhi::DeviceType::IntegratedGpu
                                                : rhi::DeviceType::DiscreteGpu;
            info.is_discrete = info.device_type == rhi::DeviceType::DiscreteGpu;

            LARGE_INTEGER umd_version{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_version))) {
                info.driver_version = format_driver_version(umd_version);
            }

            DeviceCapabilities capabilities = probe_capabilities(device.Get());
            capabilities.allow_tearing = allow_tearing_;
            info.api_version = std::string("Direct3D 12 (") +
                               shader_model_name(highest_shader_model(device.Get())) + ")";

            adapters.push_back(std::make_unique<D3D12Adapter>(factory_, std::move(adapter), std::move(device), std::move(info), std::move(capabilities), debug_layer_enabled_));
        }

        if (adapters.empty()) {
            return operation_failed("D3D12 instance enumerated no adapters capable of creating a device.");
        }
        return adapters;
    }

    rhi::RhiExpected<unique_ptr<rhi::RhiInstance>> create_d3d12_instance(const rhi::InstanceDesc &desc) {
        UINT factory_flags = 0;
        bool debug_layer_enabled = false;

        if (desc.enable_validation) {
            // Must precede every D3D12CreateDevice call in the process — the debug layer is opted into
            // globally before device creation, not per device, so this cannot be deferred to
            // create_device() the way a Vulkan validation-layer enable could be.
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                debug_layer_enabled = true;
                factory_flags |= DXGI_CREATE_FACTORY_DEBUG;

                // GPU-based validation catches the class of errors the CPU-side layer structurally
                // cannot (out-of-bounds descriptor-table indexing, uninitialized descriptor reads) —
                // exactly the errors this backend's bindless/descriptor-table binding model can
                // produce. Best-effort: it needs ID3D12Debug1, which some older runtimes lack.
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
            } else {
                Foundation::log_warn(
                    "D3D12: validation was requested but the debug layer is unavailable (the Graphics "
                    "Tools optional feature is probably not installed); continuing without it.");
            }

            // Device Removed Extended Data: turns a bare DXGI_ERROR_DEVICE_REMOVED into an auto-
            // breadcrumb trail naming the last commands the GPU executed. Costs nothing until a
            // removal actually happens, and a TDR without it is close to undiagnosable.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
        }

        ComPtr<IDXGIFactory6> factory;
        if (const HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)); FAILED(hr)) {
            return hresult_error(hr, "create_d3d12_instance (CreateDXGIFactory2)");
        }

        // Tearing support is a factory-level DXGI capability, not a device one, and it is what any
        // real "VSync off" present mode depends on — queried once here and carried into every device.
        BOOL allow_tearing = FALSE;
        if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            allow_tearing = FALSE;
        }

        (void)desc.headless; // DXGI needs no presentation opt-in at instance scope; a headless device
                             // simply never creates a surface.
        return unique_ptr<rhi::RhiInstance>(
            std::make_unique<D3D12Instance>(std::move(factory), debug_layer_enabled, allow_tearing != FALSE));
    }

    rhi::BackendRegistration d3d12_backend_registration() noexcept {
        return {
            .backend = rhi::BackendType::D3D12,
            .name = "D3D12",
            .create_instance = create_d3d12_instance,
        };
    }

} // namespace SFT::D3D12
