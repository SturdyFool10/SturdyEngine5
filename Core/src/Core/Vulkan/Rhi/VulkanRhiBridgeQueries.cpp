
#pragma region Imports
#include "volk.h"
#include <cstddef>
#include <span>
#include <utility>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanQueryPool.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::span;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    /// Creates a query set from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::QuerySetHandle> VulkanRhiDeviceBridge::create_query_set(const rhi::QuerySetDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_query_set");
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::QuerySetHandle>("create_query_set");
        }
        if (desc.count == 0) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_query_set: query count must be non-zero.");
        }

        auto pool = VulkanQueryPool::create(logical_device_->vk_handle(), to_vk(desc.type), desc.count,
                                            desc.type == rhi::QueryType::PipelineStatistics ? to_vk(desc.statistics) : 0);
        if (!pool) {
            return rhi_error_from_graphics(pool.error());
        }
        return query_sets_.insert(std::move(*pool));
    }

    /// Destroys the query set identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_query_set(rhi::QuerySetHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_query_set");
        query_sets_.erase(handle);
    }

    /// Returns the query set results associated with this `Vulkan`.
    ///
    /// @param query_set `query_set` value used by the operation.
    /// @param first First position or element included in the operation.
    /// @param count Number of elements or operations to process.
    /// @param dst Destination value or resource.
    /// @param stride `stride` value used by the operation.
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiResult VulkanRhiDeviceBridge::get_query_set_results(rhi::QuerySetHandle query_set,
                                                                u32 first,
                                                                u32 count,
                                                                span<std::byte> dst,
                                                                u64 stride,
                                                                rhi::QueryResultFlags flags) {
        ZoneScopedN("VulkanRhiDeviceBridge::get_query_set_results");
        VulkanQueryPool *pool = query_sets_.find(query_set);
        if (pool == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "get_query_set_results: unknown query set handle.");
        }
        span<u8> bytes(reinterpret_cast<u8 *>(dst.data()), dst.size());
        if (auto result = pool->get_results(first, count, bytes, stride, to_vk(flags)); !result) {
            return rhi_error_from_graphics(result.error());
        }
        return {};
    }

    /// Resets query set to its baseline state.
    ///
    /// @param query_set `query_set` value used by the operation.
    /// @param first First position or element included in the operation.
    /// @param count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::reset_query_set");
        if (VulkanQueryPool *pool = query_sets_.find(query_set)) {
            pool->reset(first, count);
        }
    }

} // namespace SFT::Core::Vulkan
