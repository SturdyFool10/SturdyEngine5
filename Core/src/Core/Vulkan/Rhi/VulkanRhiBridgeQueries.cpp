// RHI query set creation, host readback, and host reset backed by VulkanQueryPool.
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

using std::span;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::QuerySetHandle> VulkanRhiDeviceBridge::create_query_set(const rhi::QuerySetDesc &desc) {
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

    void VulkanRhiDeviceBridge::destroy_query_set(rhi::QuerySetHandle handle) noexcept {
        query_sets_.erase(handle);
    }

    rhi::RhiResult VulkanRhiDeviceBridge::get_query_set_results(rhi::QuerySetHandle query_set,
                                                                u32 first,
                                                                u32 count,
                                                                span<std::byte> dst,
                                                                u64 stride,
                                                                rhi::QueryResultFlags flags) {
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

    void VulkanRhiDeviceBridge::reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept {
        if (VulkanQueryPool *pool = query_sets_.find(query_set)) {
            pool->reset(first, count);
        }
    }

} // namespace SFT::Core::Vulkan
