#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

namespace SFT::Core::WebGpu {

    // Everything in this file is a capability the RHI models because Vulkan and D3D12 have it and
    // WebGPU simply does not. They are gathered here rather than scattered through the other
    // implementation files so the boundary of what this backend can do is one thing to read.
    //
    // Each returns a specific error naming the feature. That matters: a caller that trips one is
    // not looking at an unfinished backend it should wait on, it is looking at a hard limit of the
    // API, and the only useful responses are to take a different path or to select a different
    // backend.

    /// Creates a ray tracing pipeline.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU has no ray tracing.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::RayTracingPipelineHandle> WebGpuDevice::create_ray_tracing_pipeline(
        const rhi::RayTracingPipelineDesc &desc) {
        (void)desc;
        return std::unexpected(unsupported_by_webgpu("Ray-tracing pipelines"));
    }

    /// Destroys a ray tracing pipeline. No such pipeline can exist, so this does nothing.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept {
        (void)handle;
    }

    /// Writes ray tracing shader group handles.
    ///
    /// @param pipeline `pipeline` value used by the operation.
    /// @param first_group `first_group` value used by the operation.
    /// @param group_count `group_count` value used by the operation.
    /// @param dst Destination buffer.
    ///
    /// @return Returns the error alternative; WebGPU has no ray tracing.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::write_ray_tracing_shader_group_handles(
        rhi::RayTracingPipelineHandle pipeline, u32 first_group, u32 group_count, span<std::byte> dst) {
        (void)pipeline;
        (void)first_group;
        (void)group_count;
        (void)dst;
        return std::unexpected(unsupported_by_webgpu("Ray-tracing shader group handles"));
    }

    /// Computes acceleration structure build sizes.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU has no acceleration structures.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> WebGpuDevice::acceleration_structure_build_sizes(
        const rhi::AccelerationStructureBuildDesc &desc) const {
        (void)desc;
        return std::unexpected(unsupported_by_webgpu("Acceleration structures"));
    }

    /// Creates an acceleration structure.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU has no acceleration structures.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::AccelerationStructureHandle> WebGpuDevice::create_acceleration_structure(
        const rhi::AccelerationStructureDesc &desc) {
        (void)desc;
        return std::unexpected(unsupported_by_webgpu("Acceleration structures"));
    }

    /// Destroys an acceleration structure. None can exist, so this does nothing.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept {
        (void)handle;
    }

    /// Computes opacity micromap build sizes.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU has no opacity micromaps.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::OpacityMicromapBuildSizes> WebGpuDevice::opacity_micromap_build_sizes(
        const rhi::OpacityMicromapDesc &desc) const {
        (void)desc;
        return std::unexpected(unsupported_by_webgpu("Opacity micromaps"));
    }

    /// Creates an opacity micromap.
    ///
    /// @param desc `desc` value used by the operation.
    /// @param size `size` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU has no opacity micromaps.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::OpacityMicromapHandle> WebGpuDevice::create_opacity_micromap(
        const rhi::OpacityMicromapDesc &desc, u64 size) {
        (void)desc;
        (void)size;
        return std::unexpected(unsupported_by_webgpu("Opacity micromaps"));
    }

    /// Destroys an opacity micromap. None can exist, so this does nothing.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_opacity_micromap(rhi::OpacityMicromapHandle handle) noexcept {
        (void)handle;
    }

    /// Returns a buffer's GPU virtual address.
    ///
    /// @param buffer `buffer` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU deliberately hides GPU addresses.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<u64> WebGpuDevice::buffer_device_address(rhi::BufferHandle buffer) const {
        (void)buffer;
        // Not an oversight in the API: exposing raw GPU addresses would defeat WebGPU's bounds
        // guarantees, which are the reason it can be handed untrusted shaders at all.
        return std::unexpected(unsupported_by_webgpu("Buffer device addresses"));
    }

    /// Returns an acceleration structure's GPU virtual address.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the error alternative; WebGPU has no acceleration structures.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<u64> WebGpuDevice::acceleration_structure_device_address(
        rhi::AccelerationStructureHandle handle) const {
        (void)handle;
        return std::unexpected(unsupported_by_webgpu("Acceleration structures"));
    }

} // namespace SFT::Core::WebGpu
