#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <cstring>

namespace SFT::Core::WebGpu {

    /// Creates a query set.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::QuerySetHandle> WebGpuDevice::create_query_set(const rhi::QuerySetDesc &desc) {
        WGPUQuerySetDescriptor query_desc{};
        query_desc.label = wgpu_string(desc.label);
        query_desc.count = desc.count;
        switch (desc.type) {
            case rhi::QueryType::Occlusion:
                query_desc.type = WGPUQueryType_Occlusion;
                break;
            case rhi::QueryType::Timestamp:
                query_desc.type = WGPUQueryType_Timestamp;
                break;
            default:
                // Pipeline-statistics queries exist in Vulkan and D3D12 but not in WebGPU, which
                // offers only occlusion and timestamp.
                return std::unexpected(unsupported_by_webgpu("Pipeline statistics queries"));
        }

        WGPUQuerySet query_set = wgpuDeviceCreateQuerySet(device_, &query_desc);
        if (query_set == nullptr) {
            return std::unexpected(webgpu_error("create_query_set"));
        }
        return query_sets_.insert(std::move(query_set));
    }

    /// Destroys a query set.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_query_set(rhi::QuerySetHandle handle) noexcept {
        query_sets_.erase(handle, [](WGPUQuerySet &set) {
            wgpuQuerySetDestroy(set);
            wgpuQuerySetRelease(set);
        });
    }

    /// Resolves a query set handle to the Dawn query set behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUQuerySet WebGpuDevice::lookup_query_set(rhi::QuerySetHandle handle) noexcept {
        WGPUQuerySet *set = query_sets_.find(handle);
        return set != nullptr ? *set : nullptr;
    }

    /// Reads query results back to the host.
    ///
    /// @param query_set `query_set` value used by the operation.
    /// @param first `first` value used by the operation.
    /// @param count `count` value used by the operation.
    /// @param dst Destination buffer.
    /// @param stride `stride` value used by the operation.
    /// @param flags `flags` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count,
                                                       span<std::byte> dst, u64 stride,
                                                       rhi::QueryResultFlags flags) {
        (void)query_set;
        (void)first;
        (void)count;
        (void)dst;
        (void)stride;
        (void)flags;
        // WebGPU has no host-side query read. Results are resolved on the GPU into a buffer
        // (wgpuCommandEncoderResolveQuerySet, which this backend's command encoder exposes), and
        // that buffer is then mapped like any other. There is no path that reads a query set
        // directly, so this cannot be implemented without silently changing the caller's model of
        // when the data becomes available.
        return std::unexpected(unsupported_by_webgpu(
            "Reading query results directly from a query set (resolve them into a buffer instead)"));
    }

    /// Resets a range of queries.
    ///
    /// @param query_set `query_set` value used by the operation.
    /// @param first `first` value used by the operation.
    /// @param count `count` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept {
        (void)query_set;
        (void)first;
        (void)count;
        // WebGPU query sets need no explicit reset: writing a query overwrites its slot, and the
        // resolve step reads whatever was last written. Nothing to do rather than unsupported --
        // the caller's intent is already satisfied.
    }

} // namespace SFT::Core::WebGpu
