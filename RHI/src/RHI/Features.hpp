#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <bitset>
#include <initializer_list>
#pragma endregion

namespace SFT::RHI {

    // ─── Optional feature vocabulary ─────────────────────────────────────────────
    //
    // `Feature` is intentionally a broad, behavior-oriented, first-class capability catalog. The RHI is
    // a maximal-union API: if Vulkan/D3D12/Metal/WebGPU expose a stable, app-visible capability, prefer
    // a named feature here over an `ExtensionId` escape hatch. `ExtensionId` remains for truly
    // backend/vendor-specific interfaces, prototypes, or capabilities whose RHI descriptor vocabulary
    // has not been designed yet.
    //
    // Feature names should describe what renderer code can do, not which backend revision exposed it:
    // `RayTracingPipeline` is useful; `Maintenance11` is not. API version/maintenance bundles stay in
    // backend mapping tables until their concrete behaviors become RHI-visible features.
    //
    // A feature is a boolean: supported, requested, enabled. Graded values and limits live in
    // `FeatureProperties`, `DeviceLimits`, queue topology, or per-format queries. Vulkan feature bits
    // may map one-to-one to entries below; D3D12 tiers and Metal GPU families usually map to bundles
    // of entries.
    enum class Feature : u32 {
        // RHI baseline validators. The RHI is designed around these, but they are still named so a
        // backend can report a precise unsupported requirement on APIs/devices where they are not core.
        TimelineSynchronization,
        Synchronization2,
        DynamicRendering,

        // Device robustness, safety, and protected execution.
        RobustBufferAccess,
        RobustBufferAccess2,
        RobustImageAccess,
        NullDescriptors,
        PipelineRobustness,
        ProtectedMemory,
        PipelineProtectedAccess,

        // Shader stages and programmable pipeline front-ends.
        GeometryShader,
        TessellationShader,
        MeshShader,
        TaskShader,
        MeshShaderQueries,
        MultiviewMeshShader,
        ShaderObject,
        ShaderDrawParameters,

        // Ray tracing and acceleration structures.
        AccelerationStructures,
        AccelerationStructureCaptureReplay,
        AccelerationStructureIndirectBuild,
        RayQuery,
        RayTracingPipeline,
        RayTracingPipelineLibrary,
        RayTracingPipelineTraceRaysIndirect,
        RayTraversalPrimitiveCulling,
        RayTracingMaintenance1,
        RayTracingPositionFetch,
        RayTracingInvocationReorder,
        OpacityMicromap,
        DisplacementMicromap,
        RayTracingMotionBlur,
        DeferredHostOperations,
        GraphicsPipelineLibrary,
        PipelineLibrary,
        PipelineLibraryGroupHandles,

        // Descriptor/resource binding model. Keep `BindlessResources` as the high-level renderer path,
        // but expose fine-grained indexing/update capabilities individually for precise negotiation.
        BindlessResources,
        DescriptorIndexing,
        RuntimeDescriptorArrays,
        DescriptorBindingVariableCount,
        DescriptorBindingPartiallyBound,
        DescriptorBindingUpdateAfterBind,
        DescriptorBindingUpdateUnusedWhilePending,
        NonUniformResourceIndexing,
        UniformBufferArrayDynamicIndexing,
        SampledImageArrayDynamicIndexing,
        StorageBufferArrayDynamicIndexing,
        StorageImageArrayDynamicIndexing,
        InputAttachmentArrayDynamicIndexing,
        UniformTexelBufferArrayDynamicIndexing,
        StorageTexelBufferArrayDynamicIndexing,
        UniformBufferArrayNonUniformIndexing,
        SampledImageArrayNonUniformIndexing,
        StorageBufferArrayNonUniformIndexing,
        StorageImageArrayNonUniformIndexing,
        InputAttachmentArrayNonUniformIndexing,
        UniformTexelBufferArrayNonUniformIndexing,
        StorageTexelBufferArrayNonUniformIndexing,
        DescriptorBuffer,
        DescriptorHeap,
        DescriptorUpdateTemplate,
        PushDescriptors,
        InlineUniformBlocks,
        InlineUniformBlockUpdateAfterBind,
        MutableDescriptorTypes,

        // Shader numeric types, storage widths, atomics, and math instructions.
        ShaderInt8,
        ShaderInt16,
        ShaderInt64,
        ShaderFloat8,
        ShaderFloat16,
        ShaderFloat64,
        ShaderBFloat16,
        ShaderLongVector,
        ShaderReplicatedComposites,
        Shader16BitStorage,
        Shader8BitStorage,
        ShaderStorageInputOutput16,
        ShaderIntegerDotProduct,
        ShaderBufferInt64Atomics,
        ShaderSharedInt64Atomics,
        ShaderImageInt64Atomics,
        ShaderBufferFloat32Atomics,
        ShaderBufferFloat32AtomicAdd,
        ShaderBufferFloat64Atomics,
        ShaderBufferFloat64AtomicAdd,
        ShaderSharedFloat32Atomics,
        ShaderSharedFloat32AtomicAdd,
        ShaderSharedFloat64Atomics,
        ShaderSharedFloat64AtomicAdd,
        ShaderImageFloat32Atomics,
        ShaderImageFloat32AtomicAdd,
        CooperativeMatrix,
        CooperativeVector,
        ComputeShaderDerivatives,
        ShaderFloatControls,
        ShaderFloatControls2,
        ShaderFma,

        // Shader memory model, pointer/layout controls, and control-flow semantics.
        SubgroupOperations,
        SubgroupExtendedTypes,
        SubgroupSizeControl,
        SubgroupUniformControlFlow,
        SubgroupPartitioned,
        SubgroupRotate,
        ShaderQuadControl,
        ShaderMaximalReconvergence,
        ShaderDemoteToHelperInvocation,
        ShaderTerminateInvocation,
        ShaderAbort,
        ShaderExpectAssume,
        ShaderRelaxedExtendedInstruction,
        ShaderNonSemanticInfo,
        Spirv14,
        ShaderClock,
        VulkanMemoryModel,
        ScalarBlockLayout,
        UniformBufferStandardLayout,
        RelaxedBlockLayout,
        VariablePointers,
        VariablePointersStorageBuffer,
        WorkgroupMemoryExplicitLayout,
        ZeroInitializeWorkgroupMemory,
        ShaderUntypedPointers,
        ShaderResourceResidency,
        ShaderResourceMinLod,
        ShaderClipDistance,
        ShaderCullDistance,
        ShaderTessellationAndGeometryPointSize,
        ShaderImageGatherExtended,
        ShaderStencilExport,
        ShaderViewportIndexLayer,
        ShaderConstantData,
        ShaderUniformBufferUnsizedArray,
        Shader64BitIndexing,
        VertexPipelineStoresAndAtomics,
        FragmentStoresAndAtomics,

        // Rasterization and fixed-function pipeline state.
        DepthClamp,
        DepthClampZeroOne,
        DepthClipEnable,
        DepthClipControl,
        DepthBiasClamp,
        DepthBoundsTest,
        WireframeFill,
        PointPolygonMode,
        WideLines,
        LargePoints,
        AlphaToOne,
        MultiViewport,
        IndependentBlend,
        DualSourceBlending,
        LogicOp,
        SampleRateShading,
        VariableMultisampleRate,
        ConservativeRasterization,
        FragmentShaderInterlock,
        RasterizationOrderAttachmentAccess,
        FragmentShaderBarycentric,
        PostDepthCoverage,
        AdvancedBlendOperations,
        LineRasterization,
        ProvokingVertex,
        SampleLocations,
        DiscardRectangles,
        TransformFeedback,
        ConditionalRendering,
        ColorWriteEnable,
        ExtendedDynamicState,
        ExtendedDynamicState2,
        ExtendedDynamicState3,
        AttachmentFeedbackLoopDynamicState,
        VertexInputDynamicState,
        DepthBiasControl,
        DepthRangeUnrestricted,
        VertexAttributeDivisor,
        LegacyVertexAttributes,
        LegacyDithering,

        // Fragment shading rate / density and tile-local attachment access.
        VariableRateShading,
        PipelineFragmentShadingRate,
        PrimitiveFragmentShadingRate,
        AttachmentFragmentShadingRate,
        FragmentDensityMap,
        FragmentDensityMap2,
        FragmentDensityMapOffset,
        AttachmentFeedbackLoop,
        FramebufferFetch,
        ShaderTileImage,

        // Attachment/rendering model extras beyond the core dynamic-rendering path.
        Multiview,
        MultiviewGeometryShader,
        MultiviewTessellationShader,
        DepthStencilResolve,
        SeparateDepthStencilLayouts,
        MultisampledRenderToSingleSampled,
        LoadStoreOpNone,
        DynamicRenderingLocalRead,
        DynamicRenderingUnusedAttachments,
        ImagelessFramebuffer,
        ImageFormatList,
        SeparateStencilUsage,
        CustomResolve,
        CreateRenderPass2,
        SubpassMergeFeedback,

        // Draw, dispatch, and command-generation features.
        FullDrawIndexUint32,
        IndexTypeUint8,
        PrimitiveRestart,
        PrimitiveTopologyListRestart,
        MultiDrawIndirect,
        DrawIndirectFirstInstance,
        DrawIndirectCount,
        IndirectCommandsLayout,
        DeviceGeneratedCommands,
        DeviceGeneratedCommandsCompute,
        DeviceAddressCommands,
        CopyMemoryIndirect,
        CopyMemoryToImageIndirect,
        CopyCommands2,
        NestedCommandBuffers,
        RenderBundles,
        InheritedQueries,

        // Buffers, addresses, sparse/tiled resources, residency, and memory management.
        BufferDeviceAddress,
        BufferDeviceAddressCaptureReplay,
        BufferDeviceAddressMultiDevice,
        SparseBinding,
        SparseResidencyBuffer,
        SparseResidencyImage2D,
        SparseResidencyImage3D,
        SparseResidency2Samples,
        SparseResidency4Samples,
        SparseResidency8Samples,
        SparseResidency16Samples,
        SparseResidencyAliased,
        SparseResources,
        DedicatedAllocation,
        MemoryBudget,
        MemoryPriority,
        PageableDeviceLocalMemory,
        HostImageCopy,
        MemoryDecompression,
        ZeroInitializeDeviceMemory,
        DeviceCoherentMemory,
        MapMemory2,
        MapMemoryPlaced,
        BindMemory2,
        MemoryRequirements2,
        DeviceMemoryReport,
        DeviceAddressBindingReport,
        DeviceGroup,
        MultiGpuDeviceGroup,
        GlobalQueuePriority,
        UnifiedImageLayouts,
        MetalObjects,
        Win32KeyedMutex,

        // Texture/image/sampler capability bits. Per-format support should still be queried separately;
        // these name global shader/sampler/view behaviors and compression families.
        ImageCubeArray,
        Format4444,
        Rgba10x6Formats,
        TextureCompressionBC,
        TextureCompressionASTC,
        TextureCompressionASTCHDR,
        TextureCompressionASTC3D,
        TextureCompressionETC2,
        AstcDecodeMode,
        AnisotropicFiltering,
        SamplerMirrorClampToEdge,
        SamplerYcbcrConversion,
        SamplerFilterMinMax,
        CustomBorderColor,
        BorderColorSwizzle,
        CubicFiltering,
        NonSeamlessCubeMap,
        Image2DViewOf3D,
        ImageSlicedViewOf3D,
        ImageViewMinLod,
        ImageCompressionControl,
        ImageDrmFormatModifier,
        FormatFeatureFlags2,
        TexelBufferAlignment,
        StorageImageExtendedFormats,
        StorageImageMultisample,
        StorageImageReadWithoutFormat,
        StorageImageWriteWithoutFormat,
        Ycbcr2Plane444Formats,
        YcbcrImageArrays,

        // Queries, timing, and diagnostics/introspection that affect renderer behavior.
        OcclusionQueries,
        PreciseOcclusionQueries,
        TimestampQueries,
        CalibratedTimestamps,
        HostQueryReset,
        PipelineStatisticsQueries,
        PrimitivesGeneratedQueries,
        PerformanceQueries,
        PipelineExecutableProperties,
        ShaderModuleIdentifier,
        DebugUtils,
        FrameBoundary,
        LayerSettings,
        ToolingInfo,
        ValidationCache,
        DeviceFault,
        PrivateData,
        PipelineCreationCacheControl,
        PipelineCreationFeedback,
        PipelineProperties,
        PipelineBinary,

        // Queueing, synchronization interop, presentation, and display.
        AsyncCompute,
        AsyncTransfer,
        QueueFamilyForeign,
        InternallySynchronizedQueues,
        ExternalMemory,
        ExternalMemoryFd,
        ExternalMemoryWin32,
        ExternalMemoryDmaBuf,
        ExternalMemoryHost,
        ExternalMemoryMetal,
        ExternalMemoryAcquireUnmodified,
        ExternalSemaphore,
        ExternalSemaphoreFd,
        ExternalSemaphoreWin32,
        ExternalFence,
        ExternalFenceFd,
        ExternalFenceWin32,
        IncrementalPresent,
        PresentId,
        PresentWait,
        PresentTiming,
        PresentModeFifoLatestReady,
        SurfaceCapabilities2,
        SurfaceMaintenance,
        SurfaceProtectedCapabilities,
        SwapchainMutableFormat,
        SwapchainMaintenance,
        SharedPresentableImage,
        ImageCompressionControlSwapchain,
        FullScreenExclusive,
        DirectDisplay,
        DisplayControl,
        DisplaySurfaceCounter,
        DisplaySwapchain,
        HdrOutput,
        HdrMetadata,
        SwapchainColorspace,

        // Video encode/decode acceleration. These are RHI-visible only when media work is scheduled on
        // graphics API queues; a separate media subsystem may also consume the same bits.
        VideoDecodeQueue,
        VideoEncodeQueue,
        VideoDecodeH264,
        VideoDecodeH265,
        VideoDecodeAV1,
        VideoDecodeVP9,
        VideoEncodeH264,
        VideoEncodeH265,
        VideoEncodeAV1,
        VideoMaintenance1,
        VideoMaintenance2,
        VideoEncodeIntraRefresh,
        VideoEncodeQuantizationMap,

        // API maintenance/version bundles are intentionally not RHI features. Split them into concrete
        // behavior names above when renderer code can directly rely on that behavior; otherwise keep the
        // raw backend extension/version in the backend mapping layer or an `ExtensionId`.

        // Portability and device/driver introspection.
        PortabilityEnumeration,
        PortabilitySubset,
        DriverProperties,
        PhysicalDeviceProperties2,
        PciBusInfo,
        PhysicalDeviceDrm,

        // Keep last — the count of features, used to size FeatureSet. Never assign it a value or
        // reorder entries above it without updating any persisted feature sets.
        Count,
    };

    inline constexpr usize feature_count = static_cast<usize>(Feature::Count);

    // A short human-readable name for a feature — for logs and, importantly, the "this title requires
    // <X> and cannot run on this GPU" diagnostic an application surfaces when a required feature is
    // missing.
    [[nodiscard]] constexpr const char *feature_name(Feature feature) noexcept {
        switch (feature) {
            case Feature::TimelineSynchronization: return "timeline synchronization";
            case Feature::Synchronization2: return "synchronization2-style barriers";
            case Feature::DynamicRendering: return "dynamic rendering";
            case Feature::RobustBufferAccess: return "robust buffer access";
            case Feature::RobustBufferAccess2: return "robust buffer access 2";
            case Feature::RobustImageAccess: return "robust image access";
            case Feature::NullDescriptors: return "null descriptors";
            case Feature::PipelineRobustness: return "pipeline robustness";
            case Feature::ProtectedMemory: return "protected memory";
            case Feature::PipelineProtectedAccess: return "pipeline protected access";
            case Feature::GeometryShader: return "geometry shaders";
            case Feature::TessellationShader: return "tessellation shaders";
            case Feature::MeshShader: return "mesh shaders";
            case Feature::TaskShader: return "task/amplification shaders";
            case Feature::MeshShaderQueries: return "mesh shader queries";
            case Feature::MultiviewMeshShader: return "multiview mesh shaders";
            case Feature::ShaderObject: return "shader objects";
            case Feature::ShaderDrawParameters: return "shader draw parameters";
            case Feature::AccelerationStructures: return "acceleration structures";
            case Feature::AccelerationStructureCaptureReplay: return "acceleration-structure capture/replay";
            case Feature::AccelerationStructureIndirectBuild: return "indirect acceleration-structure builds";
            case Feature::RayQuery: return "ray query (inline ray tracing)";
            case Feature::RayTracingPipeline: return "ray tracing pipelines";
            case Feature::RayTracingPipelineLibrary: return "ray tracing pipeline libraries";
            case Feature::RayTracingPipelineTraceRaysIndirect: return "indirect trace rays";
            case Feature::RayTraversalPrimitiveCulling: return "ray traversal primitive culling";
            case Feature::RayTracingMaintenance1: return "ray tracing maintenance 1";
            case Feature::RayTracingPositionFetch: return "ray tracing position fetch";
            case Feature::RayTracingInvocationReorder: return "ray tracing invocation reorder";
            case Feature::OpacityMicromap: return "opacity micromaps";
            case Feature::DisplacementMicromap: return "displacement micromaps";
            case Feature::RayTracingMotionBlur: return "ray tracing motion blur";
            case Feature::BindlessResources: return "bindless resources";
            case Feature::DescriptorIndexing: return "descriptor indexing";
            case Feature::RuntimeDescriptorArrays: return "runtime descriptor arrays";
            case Feature::DescriptorBindingVariableCount: return "variable descriptor counts";
            case Feature::DescriptorBindingPartiallyBound: return "partially-bound descriptors";
            case Feature::DescriptorBindingUpdateAfterBind: return "descriptor update-after-bind";
            case Feature::DescriptorBindingUpdateUnusedWhilePending: return "descriptor update-unused-while-pending";
            case Feature::NonUniformResourceIndexing: return "non-uniform resource indexing";
            case Feature::UniformBufferArrayDynamicIndexing: return "dynamic uniform-buffer array indexing";
            case Feature::SampledImageArrayDynamicIndexing: return "dynamic sampled-image array indexing";
            case Feature::StorageBufferArrayDynamicIndexing: return "dynamic storage-buffer array indexing";
            case Feature::StorageImageArrayDynamicIndexing: return "dynamic storage-image array indexing";
            case Feature::InputAttachmentArrayDynamicIndexing: return "dynamic input-attachment array indexing";
            case Feature::UniformTexelBufferArrayDynamicIndexing: return "dynamic uniform-texel-buffer array indexing";
            case Feature::StorageTexelBufferArrayDynamicIndexing: return "dynamic storage-texel-buffer array indexing";
            case Feature::UniformBufferArrayNonUniformIndexing: return "non-uniform uniform-buffer array indexing";
            case Feature::SampledImageArrayNonUniformIndexing: return "non-uniform sampled-image array indexing";
            case Feature::StorageBufferArrayNonUniformIndexing: return "non-uniform storage-buffer array indexing";
            case Feature::StorageImageArrayNonUniformIndexing: return "non-uniform storage-image array indexing";
            case Feature::InputAttachmentArrayNonUniformIndexing: return "non-uniform input-attachment array indexing";
            case Feature::UniformTexelBufferArrayNonUniformIndexing: return "non-uniform uniform-texel-buffer array indexing";
            case Feature::StorageTexelBufferArrayNonUniformIndexing: return "non-uniform storage-texel-buffer array indexing";
            case Feature::DescriptorBuffer: return "descriptor buffers";
            case Feature::PushDescriptors: return "push descriptors";
            case Feature::InlineUniformBlocks: return "inline uniform blocks";
            case Feature::InlineUniformBlockUpdateAfterBind: return "inline uniform block update-after-bind";
            case Feature::MutableDescriptorTypes: return "mutable descriptor types";
            case Feature::ShaderInt8: return "8-bit integer shader ops";
            case Feature::ShaderInt16: return "16-bit integer shader ops";
            case Feature::ShaderInt64: return "64-bit integer shader ops";
            case Feature::ShaderFloat8: return "8-bit float shader ops";
            case Feature::ShaderFloat16: return "16-bit float shader ops";
            case Feature::ShaderFloat64: return "64-bit float shader ops";
            case Feature::ShaderBFloat16: return "bfloat16 shader ops";
            case Feature::Shader16BitStorage: return "16-bit shader storage";
            case Feature::Shader8BitStorage: return "8-bit shader storage";
            case Feature::ShaderStorageInputOutput16: return "16-bit shader input/output storage";
            case Feature::ShaderIntegerDotProduct: return "integer dot-product shader ops";
            case Feature::ShaderBufferInt64Atomics: return "64-bit buffer atomics";
            case Feature::ShaderSharedInt64Atomics: return "64-bit shared-memory atomics";
            case Feature::ShaderImageInt64Atomics: return "64-bit image atomics";
            case Feature::ShaderBufferFloat32Atomics: return "32-bit float buffer atomics";
            case Feature::ShaderBufferFloat32AtomicAdd: return "32-bit float buffer atomic add";
            case Feature::ShaderBufferFloat64Atomics: return "64-bit float buffer atomics";
            case Feature::ShaderBufferFloat64AtomicAdd: return "64-bit float buffer atomic add";
            case Feature::ShaderSharedFloat32Atomics: return "32-bit float shared-memory atomics";
            case Feature::ShaderSharedFloat32AtomicAdd: return "32-bit float shared-memory atomic add";
            case Feature::ShaderSharedFloat64Atomics: return "64-bit float shared-memory atomics";
            case Feature::ShaderSharedFloat64AtomicAdd: return "64-bit float shared-memory atomic add";
            case Feature::ShaderImageFloat32Atomics: return "32-bit float image atomics";
            case Feature::ShaderImageFloat32AtomicAdd: return "32-bit float image atomic add";
            case Feature::CooperativeMatrix: return "cooperative matrices";
            case Feature::CooperativeVector: return "cooperative vectors";
            case Feature::ComputeShaderDerivatives: return "compute shader derivatives";
            case Feature::SubgroupOperations: return "subgroup/wave operations";
            case Feature::SubgroupExtendedTypes: return "subgroup extended types";
            case Feature::SubgroupSizeControl: return "subgroup size control";
            case Feature::SubgroupUniformControlFlow: return "subgroup uniform control flow";
            case Feature::SubgroupPartitioned: return "partitioned subgroup operations";
            case Feature::SubgroupRotate: return "subgroup rotate operations";
            case Feature::ShaderQuadControl: return "shader quad control";
            case Feature::ShaderMaximalReconvergence: return "shader maximal reconvergence";
            case Feature::ShaderDemoteToHelperInvocation: return "demote to helper invocation";
            case Feature::ShaderTerminateInvocation: return "shader terminate invocation";
            case Feature::ShaderRelaxedExtendedInstruction: return "relaxed extended shader instructions";
            case Feature::VulkanMemoryModel: return "Vulkan shader memory model";
            case Feature::ScalarBlockLayout: return "scalar block layout";
            case Feature::UniformBufferStandardLayout: return "uniform buffer standard layout";
            case Feature::RelaxedBlockLayout: return "relaxed block layout";
            case Feature::VariablePointers: return "variable pointers";
            case Feature::VariablePointersStorageBuffer: return "storage-buffer variable pointers";
            case Feature::WorkgroupMemoryExplicitLayout: return "explicit workgroup memory layout";
            case Feature::ZeroInitializeWorkgroupMemory: return "zero-initialized workgroup memory";
            case Feature::ShaderUntypedPointers: return "shader untyped pointers";
            case Feature::ShaderResourceResidency: return "shader resource residency";
            case Feature::ShaderResourceMinLod: return "shader resource min LOD";
            case Feature::ShaderClipDistance: return "shader clip distance";
            case Feature::ShaderCullDistance: return "shader cull distance";
            case Feature::ShaderTessellationAndGeometryPointSize: return "tessellation/geometry point size";
            case Feature::ShaderImageGatherExtended: return "extended image gather";
            case Feature::VertexPipelineStoresAndAtomics: return "vertex-pipeline stores and atomics";
            case Feature::FragmentStoresAndAtomics: return "fragment stores and atomics";
            case Feature::DepthClamp: return "depth clamp";
            case Feature::DepthClampZeroOne: return "zero-to-one depth clamp";
            case Feature::DepthClipEnable: return "depth clip enable";
            case Feature::DepthClipControl: return "depth clip control";
            case Feature::DepthBiasClamp: return "depth bias clamp";
            case Feature::DepthBoundsTest: return "depth bounds test";
            case Feature::WireframeFill: return "wireframe fill mode";
            case Feature::PointPolygonMode: return "point polygon mode";
            case Feature::WideLines: return "wide lines";
            case Feature::LargePoints: return "large points";
            case Feature::AlphaToOne: return "alpha-to-one";
            case Feature::MultiViewport: return "multi-viewport";
            case Feature::IndependentBlend: return "independent blend state";
            case Feature::DualSourceBlending: return "dual-source blending";
            case Feature::LogicOp: return "logic op blending";
            case Feature::SampleRateShading: return "sample-rate shading";
            case Feature::VariableMultisampleRate: return "variable multisample rate";
            case Feature::ConservativeRasterization: return "conservative rasterization";
            case Feature::FragmentShaderInterlock: return "fragment shader interlock";
            case Feature::RasterizationOrderAttachmentAccess: return "rasterization-order attachment access";
            case Feature::FragmentShaderBarycentric: return "fragment shader barycentrics";
            case Feature::PostDepthCoverage: return "post-depth coverage";
            case Feature::AdvancedBlendOperations: return "advanced blend operations";
            case Feature::LineRasterization: return "advanced line rasterization";
            case Feature::ProvokingVertex: return "provoking vertex control";
            case Feature::SampleLocations: return "sample locations";
            case Feature::DiscardRectangles: return "discard rectangles";
            case Feature::TransformFeedback: return "transform feedback";
            case Feature::ConditionalRendering: return "conditional rendering";
            case Feature::ColorWriteEnable: return "dynamic color write enable";
            case Feature::ExtendedDynamicState: return "extended dynamic state";
            case Feature::ExtendedDynamicState2: return "extended dynamic state 2";
            case Feature::ExtendedDynamicState3: return "extended dynamic state 3";
            case Feature::VariableRateShading: return "variable rate shading";
            case Feature::PipelineFragmentShadingRate: return "pipeline fragment shading rate";
            case Feature::PrimitiveFragmentShadingRate: return "primitive fragment shading rate";
            case Feature::AttachmentFragmentShadingRate: return "attachment fragment shading rate";
            case Feature::FragmentDensityMap: return "fragment density map";
            case Feature::FragmentDensityMap2: return "fragment density map 2";
            case Feature::AttachmentFeedbackLoop: return "attachment feedback loops";
            case Feature::FramebufferFetch: return "framebuffer fetch";
            case Feature::ShaderTileImage: return "shader tile image";
            case Feature::Multiview: return "multiview rendering";
            case Feature::MultiviewGeometryShader: return "multiview geometry shaders";
            case Feature::MultiviewTessellationShader: return "multiview tessellation shaders";
            case Feature::DepthStencilResolve: return "depth/stencil resolve";
            case Feature::SeparateDepthStencilLayouts: return "separate depth/stencil layouts";
            case Feature::MultisampledRenderToSingleSampled: return "multisampled render to single-sampled";
            case Feature::LoadStoreOpNone: return "load/store op none";
            case Feature::DynamicRenderingLocalRead: return "dynamic rendering local read";
            case Feature::DynamicRenderingUnusedAttachments: return "dynamic rendering unused attachments";
            case Feature::ImagelessFramebuffer: return "imageless framebuffers";
            case Feature::ImageFormatList: return "image format lists";
            case Feature::SeparateStencilUsage: return "separate stencil usage";
            case Feature::CustomResolve: return "custom attachment resolve";
            case Feature::FullDrawIndexUint32: return "full 32-bit draw indices";
            case Feature::IndexTypeUint8: return "8-bit indices";
            case Feature::PrimitiveRestart: return "primitive restart";
            case Feature::PrimitiveTopologyListRestart: return "list primitive restart";
            case Feature::MultiDrawIndirect: return "multi-draw indirect";
            case Feature::DrawIndirectFirstInstance: return "indirect first instance";
            case Feature::DrawIndirectCount: return "indirect draw count";
            case Feature::IndirectCommandsLayout: return "indirect command layouts";
            case Feature::DeviceGeneratedCommands: return "device-generated commands";
            case Feature::DeviceGeneratedCommandsCompute: return "compute device-generated commands";
            case Feature::DeviceAddressCommands: return "device-address command buffers";
            case Feature::CopyMemoryIndirect: return "indirect memory copies";
            case Feature::CopyMemoryToImageIndirect: return "indirect memory-to-image copies";
            case Feature::RenderBundles: return "render bundles / parallel render-pass recording";
            case Feature::InheritedQueries: return "inherited queries";
            case Feature::BufferDeviceAddress: return "buffer device address";
            case Feature::BufferDeviceAddressCaptureReplay: return "buffer device address capture/replay";
            case Feature::BufferDeviceAddressMultiDevice: return "multi-device buffer device address";
            case Feature::SparseBinding: return "sparse binding";
            case Feature::SparseResidencyBuffer: return "sparse buffer residency";
            case Feature::SparseResidencyImage2D: return "sparse 2D image residency";
            case Feature::SparseResidencyImage3D: return "sparse 3D image residency";
            case Feature::SparseResidency2Samples: return "sparse 2x MSAA residency";
            case Feature::SparseResidency4Samples: return "sparse 4x MSAA residency";
            case Feature::SparseResidency8Samples: return "sparse 8x MSAA residency";
            case Feature::SparseResidency16Samples: return "sparse 16x MSAA residency";
            case Feature::SparseResidencyAliased: return "aliased sparse residency";
            case Feature::SparseResources: return "sparse/tiled resources";
            case Feature::MemoryBudget: return "memory budget reporting";
            case Feature::MemoryPriority: return "memory priority";
            case Feature::PageableDeviceLocalMemory: return "pageable device-local memory";
            case Feature::HostImageCopy: return "host image copy";
            case Feature::MemoryDecompression: return "memory decompression";
            case Feature::ZeroInitializeDeviceMemory: return "zero-initialized device memory";
            case Feature::DeviceCoherentMemory: return "device coherent memory";
            case Feature::DeviceMemoryReport: return "device memory reports";
            case Feature::DeviceGroup: return "device groups";
            case Feature::MultiGpuDeviceGroup: return "multi-GPU device groups";
            case Feature::GlobalQueuePriority: return "global queue priority";
            case Feature::ImageCubeArray: return "cube texture arrays";
            case Feature::TextureCompressionBC: return "BC texture compression";
            case Feature::TextureCompressionASTC: return "ASTC texture compression";
            case Feature::TextureCompressionASTCHDR: return "ASTC HDR texture compression";
            case Feature::TextureCompressionASTC3D: return "3D ASTC texture compression";
            case Feature::TextureCompressionETC2: return "ETC2 texture compression";
            case Feature::AnisotropicFiltering: return "anisotropic filtering";
            case Feature::SamplerMirrorClampToEdge: return "mirror-clamp-to-edge sampling";
            case Feature::SamplerYcbcrConversion: return "sampler YCbCr conversion";
            case Feature::SamplerFilterMinMax: return "sampler min/max filtering";
            case Feature::CustomBorderColor: return "custom border colors";
            case Feature::BorderColorSwizzle: return "border color swizzle";
            case Feature::CubicFiltering: return "cubic filtering";
            case Feature::NonSeamlessCubeMap: return "non-seamless cubemaps";
            case Feature::Image2DViewOf3D: return "2D views of 3D images";
            case Feature::ImageSlicedViewOf3D: return "sliced 3D image views";
            case Feature::ImageViewMinLod: return "image view min LOD";
            case Feature::ImageCompressionControl: return "image compression control";
            case Feature::StorageImageExtendedFormats: return "storage image extended formats";
            case Feature::StorageImageMultisample: return "multisampled storage images";
            case Feature::StorageImageReadWithoutFormat: return "storage image read without format";
            case Feature::StorageImageWriteWithoutFormat: return "storage image write without format";
            case Feature::OcclusionQueries: return "occlusion queries";
            case Feature::PreciseOcclusionQueries: return "precise occlusion queries";
            case Feature::TimestampQueries: return "timestamp queries";
            case Feature::CalibratedTimestamps: return "calibrated timestamps";
            case Feature::HostQueryReset: return "host query reset";
            case Feature::PipelineStatisticsQueries: return "pipeline statistics queries";
            case Feature::PrimitivesGeneratedQueries: return "primitives-generated queries";
            case Feature::PerformanceQueries: return "performance queries";
            case Feature::PipelineExecutableProperties: return "pipeline executable properties";
            case Feature::ShaderModuleIdentifier: return "shader module identifiers";
            case Feature::DeviceFault: return "device fault diagnostics";
            case Feature::PrivateData: return "private data slots";
            case Feature::PipelineCreationCacheControl: return "pipeline creation cache control";
            case Feature::AsyncCompute: return "async compute";
            case Feature::AsyncTransfer: return "async transfer";
            case Feature::ExternalMemory: return "external memory";
            case Feature::ExternalSemaphore: return "external semaphores";
            case Feature::ExternalFence: return "external fences";
            case Feature::PresentId: return "present IDs";
            case Feature::PresentWait: return "present wait";
            case Feature::PresentTiming: return "present timing";
            case Feature::SwapchainMutableFormat: return "mutable-format swapchains";
            case Feature::SwapchainMaintenance: return "swapchain maintenance";
            case Feature::FullScreenExclusive: return "fullscreen exclusive";
            case Feature::DirectDisplay: return "direct display";
            case Feature::HdrOutput: return "HDR output";
            case Feature::VideoDecodeQueue: return "video decode queues";
            case Feature::VideoEncodeQueue: return "video encode queues";
            case Feature::VideoDecodeH264: return "H.264 video decode";
            case Feature::VideoDecodeH265: return "H.265/HEVC video decode";
            case Feature::VideoDecodeAV1: return "AV1 video decode";
            case Feature::VideoDecodeVP9: return "VP9 video decode";
            case Feature::VideoEncodeH264: return "H.264 video encode";
            case Feature::VideoEncodeH265: return "H.265/HEVC video encode";
            case Feature::VideoEncodeAV1: return "AV1 video encode";
            case Feature::DeferredHostOperations: return "deferred host operations";
            case Feature::GraphicsPipelineLibrary: return "graphics pipeline libraries";
            case Feature::PipelineLibrary: return "pipeline libraries";
            case Feature::PipelineLibraryGroupHandles: return "pipeline library group handles";
            case Feature::DescriptorHeap: return "descriptor heaps";
            case Feature::DescriptorUpdateTemplate: return "descriptor update templates";
            case Feature::ShaderLongVector: return "shader long vectors";
            case Feature::ShaderReplicatedComposites: return "shader replicated composites";
            case Feature::ShaderFloatControls: return "shader float controls";
            case Feature::ShaderFloatControls2: return "shader float controls 2";
            case Feature::ShaderFma: return "shader fused multiply-add";
            case Feature::ShaderAbort: return "shader abort";
            case Feature::ShaderExpectAssume: return "shader expect/assume";
            case Feature::ShaderNonSemanticInfo: return "shader non-semantic info";
            case Feature::Spirv14: return "SPIR-V 1.4";
            case Feature::ShaderClock: return "shader clock";
            case Feature::ShaderStencilExport: return "shader stencil export";
            case Feature::ShaderViewportIndexLayer: return "shader viewport index/layer";
            case Feature::ShaderConstantData: return "shader constant data";
            case Feature::ShaderUniformBufferUnsizedArray: return "unsized uniform-buffer arrays";
            case Feature::Shader64BitIndexing: return "64-bit shader indexing";
            case Feature::AttachmentFeedbackLoopDynamicState: return "attachment feedback-loop dynamic state";
            case Feature::VertexInputDynamicState: return "vertex input dynamic state";
            case Feature::DepthBiasControl: return "depth bias control";
            case Feature::DepthRangeUnrestricted: return "unrestricted depth range";
            case Feature::VertexAttributeDivisor: return "vertex attribute divisors";
            case Feature::LegacyVertexAttributes: return "legacy vertex attributes";
            case Feature::LegacyDithering: return "legacy dithering";
            case Feature::FragmentDensityMapOffset: return "fragment density map offset";
            case Feature::CreateRenderPass2: return "render pass 2 creation";
            case Feature::SubpassMergeFeedback: return "subpass merge feedback";
            case Feature::CopyCommands2: return "copy commands 2";
            case Feature::NestedCommandBuffers: return "nested command buffers";
            case Feature::DedicatedAllocation: return "dedicated memory allocation";
            case Feature::MapMemory2: return "map memory 2";
            case Feature::MapMemoryPlaced: return "placed memory mapping";
            case Feature::BindMemory2: return "bind memory 2";
            case Feature::MemoryRequirements2: return "memory requirements 2";
            case Feature::DeviceAddressBindingReport: return "device address binding reports";
            case Feature::UnifiedImageLayouts: return "unified image layouts";
            case Feature::MetalObjects: return "Metal object interop";
            case Feature::Win32KeyedMutex: return "Win32 keyed mutex interop";
            case Feature::Format4444: return "4:4:4:4 formats";
            case Feature::Rgba10x6Formats: return "RGBA10x6 formats";
            case Feature::AstcDecodeMode: return "ASTC decode mode";
            case Feature::ImageDrmFormatModifier: return "DRM format modifier images";
            case Feature::FormatFeatureFlags2: return "format feature flags 2";
            case Feature::TexelBufferAlignment: return "texel buffer alignment";
            case Feature::Ycbcr2Plane444Formats: return "YCbCr 2-plane 4:4:4 formats";
            case Feature::YcbcrImageArrays: return "YCbCr image arrays";
            case Feature::DebugUtils: return "debug utils";
            case Feature::FrameBoundary: return "frame boundary markers";
            case Feature::LayerSettings: return "layer settings";
            case Feature::ToolingInfo: return "tooling info";
            case Feature::ValidationCache: return "validation cache";
            case Feature::PipelineCreationFeedback: return "pipeline creation feedback";
            case Feature::PipelineProperties: return "pipeline properties";
            case Feature::PipelineBinary: return "pipeline binaries";
            case Feature::QueueFamilyForeign: return "foreign queue families";
            case Feature::InternallySynchronizedQueues: return "internally synchronized queues";
            case Feature::ExternalMemoryFd: return "external memory file descriptors";
            case Feature::ExternalMemoryWin32: return "external memory Win32 handles";
            case Feature::ExternalMemoryDmaBuf: return "external memory DMA-BUF";
            case Feature::ExternalMemoryHost: return "external host memory";
            case Feature::ExternalMemoryMetal: return "external Metal memory";
            case Feature::ExternalMemoryAcquireUnmodified: return "external memory acquire-unmodified";
            case Feature::ExternalSemaphoreFd: return "external semaphore file descriptors";
            case Feature::ExternalSemaphoreWin32: return "external semaphore Win32 handles";
            case Feature::ExternalFenceFd: return "external fence file descriptors";
            case Feature::ExternalFenceWin32: return "external fence Win32 handles";
            case Feature::IncrementalPresent: return "incremental present";
            case Feature::PresentModeFifoLatestReady: return "FIFO latest-ready present mode";
            case Feature::SurfaceCapabilities2: return "surface capabilities 2";
            case Feature::SurfaceMaintenance: return "surface maintenance";
            case Feature::SurfaceProtectedCapabilities: return "surface protected capabilities";
            case Feature::SharedPresentableImage: return "shared presentable images";
            case Feature::ImageCompressionControlSwapchain: return "swapchain image compression control";
            case Feature::DisplayControl: return "display control";
            case Feature::DisplaySurfaceCounter: return "display surface counters";
            case Feature::DisplaySwapchain: return "display swapchains";
            case Feature::HdrMetadata: return "HDR metadata";
            case Feature::SwapchainColorspace: return "swapchain color spaces";
            case Feature::VideoMaintenance1: return "video maintenance 1";
            case Feature::VideoMaintenance2: return "video maintenance 2";
            case Feature::VideoEncodeIntraRefresh: return "video encode intra refresh";
            case Feature::VideoEncodeQuantizationMap: return "video encode quantization maps";
            case Feature::PortabilityEnumeration: return "portability enumeration";
            case Feature::PortabilitySubset: return "portability subset";
            case Feature::DriverProperties: return "driver properties";
            case Feature::PhysicalDeviceProperties2: return "physical device properties 2";
            case Feature::PciBusInfo: return "PCI bus info";
            case Feature::PhysicalDeviceDrm: return "physical device DRM info";
            case Feature::Count: return "<invalid>";
        }
        return "<unknown>";
    }

    // ─── Feature set ─────────────────────────────────────────────────────────────

    // A set of `Feature`s — used three ways with one type: what an adapter *supports* (queried before
    // device creation), what a device request *requires* / *optionally wants*, and what a device
    // actually *enabled*. Backed by a fixed-size bitset over the enum; storage is an implementation
    // detail (it can widen past 64 features without touching this interface).
    class FeatureSet {
      public:
        constexpr FeatureSet() = default;

        FeatureSet &set(Feature feature, bool enabled = true) noexcept;

        FeatureSet &unset(Feature feature) noexcept;

        [[nodiscard]] bool has(Feature feature) const noexcept;

        // Every feature in `required` is present in this set (`required` ⊆ `*this`) — the check a
        // backend runs on `adapter.supported_features()` against a request's required features.
        [[nodiscard]] bool contains_all(const FeatureSet &required) const noexcept;

        // The features in `required` that this set is missing — for building a precise "unsupported:
        // A, B, C" message when a required-feature check fails. Iterate it with `for_each`.
        [[nodiscard]] FeatureSet missing(const FeatureSet &required) const noexcept;

        [[nodiscard]] FeatureSet intersection(const FeatureSet &other) const noexcept;

        [[nodiscard]] FeatureSet difference(const FeatureSet &other) const noexcept;

        [[nodiscard]] bool any() const noexcept;
        [[nodiscard]] bool none() const noexcept;
        [[nodiscard]] usize count() const noexcept;

        FeatureSet &operator|=(const FeatureSet &other) noexcept;

        [[nodiscard]] friend FeatureSet operator|(FeatureSet a, const FeatureSet &b) noexcept {
            a |= b;
            return a;
        }

        friend bool operator==(const FeatureSet &, const FeatureSet &) noexcept = default;

        // Invoke `fn(Feature)` for each feature present, in enum order — e.g. to log an enabled set or
        // format a list of missing required features.
        template <class Fn>
        void for_each(Fn &&fn) const {
            for (usize i = 0; i < feature_count; ++i) {
                if (bits_.test(i)) {
                    fn(static_cast<Feature>(i));
                }
            }
        }

      private:
        std::bitset<feature_count> bits_;
    };

    struct FeatureNegotiationReport {
        FeatureSet supported_features;
        FeatureSet requested_required_features;
        FeatureSet requested_optional_features;
        FeatureSet enabled_required_features;
        FeatureSet enabled_optional_features;
        FeatureSet missing_required_features;
        FeatureSet unavailable_optional_features;

        [[nodiscard]] bool required_satisfied() const noexcept;
        [[nodiscard]] bool optional_fully_enabled() const noexcept;
        [[nodiscard]] FeatureSet enabled_features() const noexcept;
    };

    [[nodiscard]] FeatureNegotiationReport negotiate_features(
        const FeatureSet &supported,
        const FeatureSet &required,
        const FeatureSet &optional) noexcept;

    // Convenience: a `FeatureSet` containing exactly the listed features, for terse request
    // construction — `features_of({Feature::RayTracingPipeline, Feature::RayQuery})`.
    [[nodiscard]] FeatureSet features_of(std::initializer_list<Feature> features) noexcept;

    // ─── Feature-associated properties (graded values) ───────────────────────────
    //
    // The numeric limits/parameters that come *with* a feature when it's supported — meaningless when
    // the feature is off, so kept out of the boolean set. Queried per-adapter alongside
    // `supported_features()`.

    struct RayTracingProperties {
        u32 max_ray_recursion_depth = 0;
        u32 shader_group_handle_size = 0; // SBT record handle size (Vulkan-shaped, but portable)
        u32 shader_group_base_alignment = 0;
        u32 max_ray_hit_attribute_size = 0;
        u32 max_acceleration_structure_geometry_count = 0;
        u32 max_acceleration_structure_instance_count = 0;
        u64 min_acceleration_structure_scratch_offset_alignment = 0;
    };

    struct MeshShaderProperties {
        u32 max_task_work_group_invocations = 0;
        u32 max_mesh_work_group_invocations = 0;
        u32 max_mesh_output_vertices = 0;
        u32 max_mesh_output_primitives = 0;
        u32 max_mesh_multiview_view_count = 0;
        u32 max_mesh_payload_size = 0;
    };

    struct VariableRateShadingProperties {
        u32 min_tile_width = 0;
        u32 min_tile_height = 0;
        u32 max_tile_width = 0;
        u32 max_tile_height = 0;
    };

    struct SubgroupProperties {
        u32 min_subgroup_size = 0;
        u32 max_subgroup_size = 0;
        u32 supported_stage_mask = 0;
        u32 supported_operation_mask = 0;
    };

    struct DescriptorIndexingProperties {
        u32 max_update_after_bind_descriptors = 0;
        u32 max_variable_descriptor_count = 0;
    };

    struct SparseResourceProperties {
        bool residency_standard_2d_block_shape = false;
        bool residency_standard_3d_block_shape = false;
        bool residency_aligned_mip_size = false;
        bool residency_non_resident_strict = false;
    };

    struct FeatureProperties {
        RayTracingProperties ray_tracing{};
        MeshShaderProperties mesh_shader{};
        VariableRateShadingProperties variable_rate_shading{};
        SubgroupProperties subgroup{};
        DescriptorIndexingProperties descriptor_indexing{};
        SparseResourceProperties sparse_resources{};
    };

} // namespace SFT::RHI
