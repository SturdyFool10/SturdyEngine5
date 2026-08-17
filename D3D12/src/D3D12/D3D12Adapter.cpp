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


        constexpr D3D_FEATURE_LEVEL minimum_feature_level = D3D_FEATURE_LEVEL_11_0;

        /// Returns a human-readable name for the supplied vendor value.
        ///
        /// @param vendor_id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *vendor_name(u32 vendor_id) noexcept {
            switch (vendor_id) {
                case 0x1002:
                    return "AMD";
                case 0x10DE:
                    return "NVIDIA";
                case 0x8086:
                    return "Intel";
                case 0x1414:
                    return "Microsoft";
                case 0x13B5:
                    return "ARM";
                case 0x5143:
                    return "Qualcomm";
                default:
                    return "Unknown";
            }
        }

        /// Performs the narrow operation for `D3D12` using the supplied arguments.
        ///
        /// @param wide `wide` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string narrow(const wchar_t *wide) {
            std::string result;
            for (const wchar_t *cursor = wide; cursor != nullptr && *cursor != L'\0'; ++cursor) {
                result.push_back(*cursor < 128 ? static_cast<char>(*cursor) : '?');
            }
            return result;
        }

        /// Formats driver version using the supplied arguments and current state.
        ///
        /// @param umd `umd` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string format_driver_version(LARGE_INTEGER umd) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(umd.HighPart)), static_cast<unsigned>(LOWORD(umd.HighPart)), static_cast<unsigned>(HIWORD(umd.LowPart)), static_cast<unsigned>(LOWORD(umd.LowPart)));
            return buffer;
        }

        /// Performs the physical device ID operation for `D3D12` using the supplied arguments.
        ///
        /// @param luid `luid` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string physical_device_id(LUID luid) {
            static_assert(sizeof(luid) == sizeof(u64));
            u64 bits = 0;
            std::memcpy(&bits, &luid, sizeof(bits));
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "windows-luid:%016llx", static_cast<unsigned long long>(bits));
            return buffer;
        }

        /// Returns a human-readable name for the supplied shader model value.
        ///
        /// @param model `model` value used by the operation.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note This function does not throw exceptions.
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


        /// Performs the check feature operation for `D3D12` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        /// @param feature `feature` value used by the operation.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        template <typename Data>
        [[nodiscard]] bool check_feature(ID3D12Device *device, D3D12_FEATURE feature, Data &data) noexcept {
            return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
        }


        /// Performs the highest shader model operation for `D3D12` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Fills sample counts using the supplied arguments and current state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param limits `limits` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void fill_sample_counts(ID3D12Device *device, rhi::DeviceLimits &limits) noexcept {


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

    /// Performs the probe capabilities operation for `D3D12` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DeviceCapabilities probe_capabilities(ID3D12Device *device) {
        DeviceCapabilities caps{};
        rhi::FeatureSet &features = caps.features;


        for (rhi::Feature feature : {
                 rhi::Feature::TimelineSynchronization,
                 rhi::Feature::DynamicRendering,
                 rhi::Feature::TextureCompressionBC,
                 rhi::Feature::ImageCubeArray,
                 rhi::Feature::FullDrawIndexUint32,


                 rhi::Feature::MultiDrawIndirect,
                 rhi::Feature::DrawIndirectFirstInstance,
                 rhi::Feature::DrawIndirectCount,
                 rhi::Feature::OcclusionQueries,
                 rhi::Feature::PreciseOcclusionQueries,
                 rhi::Feature::TimestampQueries,
                 rhi::Feature::PipelineStatisticsQueries,


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


        rhi::DeviceLimits &limits = caps.limits;
        limits.max_texture_dimension_2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        limits.max_texture_array_layers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;


        limits.max_bind_groups = 8;


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


        limits.min_storage_buffer_offset_alignment = 16;
        limits.timestamp_valid_bits = 64;
        fill_sample_counts(device, limits);


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


    /// Performs the d3 d12 adapter operation for `D3D12` using the supplied arguments.
    ///
    /// @param factory `factory` value used by the operation.
    /// @param adapter `adapter` value used by the operation.
    /// @param device Device used or affected by the operation.
    /// @param info Description of the resource or operation to perform.
    /// @param capabilities `capabilities` value used by the operation.
    /// @param debug_layer_enabled `debug_layer_enabled` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12Adapter::D3D12Adapter(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter4> adapter, ComPtr<ID3D12Device> device, rhi::AdapterInfo info, DeviceCapabilities capabilities, bool debug_layer_enabled)
        : factory_(std::move(factory)), adapter_(std::move(adapter)), device_(std::move(device)),
          info_(std::move(info)), capabilities_(std::move(capabilities)),
          debug_layer_enabled_(debug_layer_enabled) {}

    /// Returns the current or globally available info value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::AdapterInfo &D3D12Adapter::info() const noexcept { return info_; }

    /// Returns the current or globally available supported features value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::FeatureSet &D3D12Adapter::supported_features() const noexcept { return capabilities_.features; }

    /// Returns the current or globally available feature properties value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::FeatureProperties &D3D12Adapter::feature_properties() const noexcept { return capabilities_.properties; }

    /// Returns the current or globally available supported extensions value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    span<const rhi::ExtensionId> D3D12Adapter::supported_extensions() const noexcept { return supported_extensions_; }

    /// Returns the current or globally available queue infos value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    span<const rhi::QueueInfo> D3D12Adapter::queue_infos() const noexcept { return capabilities_.queue_infos; }

    /// Returns the current or globally available limits value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::DeviceLimits &D3D12Adapter::limits() const noexcept { return capabilities_.limits; }

    /// Creates a device from the supplied parameters.
    ///
    /// @param request `request` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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


    /// Performs the d3 d12 instance operation for `D3D12` using the supplied arguments.
    ///
    /// @param factory `factory` value used by the operation.
    /// @param debug_layer_enabled `debug_layer_enabled` value used by the operation.
    /// @param allow_tearing `allow_tearing` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12Instance::D3D12Instance(ComPtr<IDXGIFactory6> factory, bool debug_layer_enabled, bool allow_tearing)
        : factory_(std::move(factory)), debug_layer_enabled_(debug_layer_enabled), allow_tearing_(allow_tearing) {}

    /// Returns the current or globally available backend type value.
    ///
    /// @return Returns the current backend type value.
    /// @note This function does not throw exceptions.
    rhi::BackendType D3D12Instance::backend_type() const noexcept { return rhi::BackendType::D3D12; }

    /// Enumerates adapters using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<vector<unique_ptr<rhi::RhiAdapter>>> D3D12Instance::enumerate_adapters() {
        vector<unique_ptr<rhi::RhiAdapter>> adapters;

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter1;


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

    /// Creates a D3D12 instance from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::RhiInstance>> create_d3d12_instance(const rhi::InstanceDesc &desc) {
        UINT factory_flags = 0;
        bool debug_layer_enabled = false;

        if (desc.enable_validation) {


            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                debug_layer_enabled = true;
                factory_flags |= DXGI_CREATE_FACTORY_DEBUG;


                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
            } else {
                Foundation::log_warn(
                    "D3D12: validation was requested but the debug layer is unavailable (the Graphics "
                    "Tools optional feature is probably not installed); continuing without it.");
            }


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


        BOOL allow_tearing = FALSE;
        if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            allow_tearing = FALSE;
        }

        (void)desc.headless;

        return unique_ptr<rhi::RhiInstance>(
            std::make_unique<D3D12Instance>(std::move(factory), debug_layer_enabled, allow_tearing != FALSE));
    }

    /// Returns the current or globally available D3D12 backend registration value.
    ///
    /// @return Returns the current D3D12 backend registration value.
    /// @note This function does not throw exceptions.
    rhi::BackendRegistration d3d12_backend_registration() noexcept {
        return {
            .backend = rhi::BackendType::D3D12,
            .name = "D3D12",
            .create_instance = create_d3d12_instance,
        };
    }

} // namespace SFT::D3D12
