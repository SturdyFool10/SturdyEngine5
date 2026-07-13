// RHI acceleration-structure resources and device-address queries backed by Vulkan ray tracing objects.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

module Sturdy.Core;

import :VulkanAccelerationStructure;
import :VulkanAllocator;
import :VulkanBuffer;
import :VulkanDevice;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import Sturdy.RHI;

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    [[nodiscard]] constexpr VkAccelerationStructureTypeKHR to_vk(rhi::AccelerationStructureType type) noexcept {
            switch (type) {
                case rhi::AccelerationStructureType::BottomLevel: return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                case rhi::AccelerationStructureType::TopLevel: return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            }
            return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        }

    [[nodiscard]] constexpr VkBuildAccelerationStructureFlagsKHR to_vk(rhi::AccelerationStructureBuildFlags flags) noexcept {
            VkBuildAccelerationStructureFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowUpdate)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowCompaction)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastTrace)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastBuild)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::MinimizeMemory)) out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
            return out;
        }

    [[nodiscard]] constexpr VkGeometryFlagsKHR to_vk(rhi::AccelerationStructureGeometryFlags flags) noexcept {
            VkGeometryFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::Opaque)) out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::NoDuplicateAnyHitInvocation)) out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            return out;
        }

    [[nodiscard]] constexpr VkCopyAccelerationStructureModeKHR to_vk(rhi::AccelerationStructureCopyMode mode) noexcept {
            switch (mode) {
                case rhi::AccelerationStructureCopyMode::Clone: return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
                case rhi::AccelerationStructureCopyMode::Compact: return VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                case rhi::AccelerationStructureCopyMode::Serialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
                case rhi::AccelerationStructureCopyMode::Deserialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
            }
            return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
        }

    [[nodiscard]] VkDeviceOrHostAddressConstKHR device_address_const(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressConstKHR out{};
            out.deviceAddress = address;
            return out;
        }

    [[nodiscard]] VkDeviceOrHostAddressKHR device_address(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressKHR out{};
            out.deviceAddress = address;
            return out;
        }

    [[nodiscard]] VkAccelerationStructureGeometryKHR to_vk_geometry(
            const rhi::AccelerationStructureGeometryDesc &geometry,
            const VulkanRhiDeviceBridge &bridge) noexcept {
            VkAccelerationStructureGeometryKHR out{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry = {},
                .flags = to_vk(geometry.flags),
            };

            switch (geometry.type) {
                case rhi::AccelerationStructureGeometryType::Triangles: {
                    const auto *vertices = bridge.buffers_.find(geometry.triangles.vertex_buffer);
                    const auto *indices = bridge.buffers_.find(geometry.triangles.index_buffer);
                    const auto *transform = bridge.buffers_.find(geometry.triangles.transform_buffer);
                    out.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                    out.geometry.triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                        .vertexFormat = to_vk(geometry.triangles.vertex_format),
                        .vertexData = device_address_const(vertices ? vertices->buffer.device_address() + geometry.triangles.vertex_offset : 0),
                        .vertexStride = geometry.triangles.vertex_stride,
                        .maxVertex = geometry.triangles.max_vertex,
                        .indexType = indices ? to_vk(geometry.triangles.index_format) : VK_INDEX_TYPE_NONE_KHR,
                        .indexData = device_address_const(indices ? indices->buffer.device_address() + geometry.triangles.index_offset : 0),
                        .transformData = device_address_const(transform ? transform->buffer.device_address() + geometry.triangles.transform_offset : 0),
                    };
                    break;
                }
                case rhi::AccelerationStructureGeometryType::Aabbs: {
                    const auto *aabb = bridge.buffers_.find(geometry.aabbs.buffer);
                    out.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                    out.geometry.aabbs = VkAccelerationStructureGeometryAabbsDataKHR{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .data = device_address_const(aabb ? aabb->buffer.device_address() + geometry.aabbs.offset : 0),
                        .stride = geometry.aabbs.stride,
                    };
                    break;
                }
                case rhi::AccelerationStructureGeometryType::Instances: {
                    const auto *instances = bridge.buffers_.find(geometry.instances.buffer);
                    out.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                    out.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                        .arrayOfPointers = geometry.instances.array_of_pointers ? VK_TRUE : VK_FALSE,
                        .data = device_address_const(instances ? instances->buffer.device_address() + geometry.instances.offset : 0),
                    };
                    break;
                }
            }
            return out;
        }

    [[nodiscard]] VkAccelerationStructureBuildRangeInfoKHR to_vk_range(
            const rhi::AccelerationStructureBuildRangeInfo &range) noexcept {
            return VkAccelerationStructureBuildRangeInfoKHR{
                .primitiveCount = range.primitive_count,
                .primitiveOffset = range.primitive_offset,
                .firstVertex = range.first_vertex,
                .transformOffset = range.transform_offset,
            };
        }

    rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> VulkanRhiDeviceBridge::acceleration_structure_build_sizes(
        const rhi::AccelerationStructureBuildDesc &desc) const {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::AccelerationStructureBuildSizes>("acceleration_structure_build_sizes");
        }
        if (!enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "acceleration_structure_build_sizes: requires enabled Feature::AccelerationStructures.");
        }
        if (vkGetAccelerationStructureBuildSizesKHR == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "acceleration_structure_build_sizes: acceleration-structure entry points are not loaded.");
        }

        vector<VkAccelerationStructureGeometryKHR> geometries;
        geometries.reserve(desc.geometries.size());
        for (const rhi::AccelerationStructureGeometryDesc &geometry : desc.geometries) {
            geometries.push_back(to_vk_geometry(geometry, *this));
        }

        vector<u32> primitive_counts;
        primitive_counts.reserve(desc.ranges.size());
        for (const rhi::AccelerationStructureBuildRangeInfo &range : desc.ranges) {
            primitive_counts.push_back(range.primitive_count);
        }
        while (primitive_counts.size() < geometries.size()) {
            primitive_counts.push_back(0);
        }

        const VkAccelerationStructureBuildGeometryInfoKHR build_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = to_vk(desc.type),
            .flags = to_vk(desc.flags),
            .mode = desc.src.is_valid() ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = static_cast<u32>(geometries.size()),
            .pGeometries = geometries.empty() ? nullptr : geometries.data(),
        };
        const VkAccelerationStructureBuildSizesInfoKHR sizes = VulkanAccelerationStructure::build_sizes(
            logical_device_->vk_handle(), build_info, primitive_counts);
        return rhi::AccelerationStructureBuildSizes{
            .acceleration_structure_size = sizes.accelerationStructureSize,
            .build_scratch_size = sizes.buildScratchSize,
            .update_scratch_size = sizes.updateScratchSize,
        };
    }

    rhi::RhiExpected<rhi::AccelerationStructureHandle> VulkanRhiDeviceBridge::create_acceleration_structure(
        const rhi::AccelerationStructureDesc &desc) {
        if (allocator_ == nullptr || logical_device_ == nullptr) {
            return device_not_ready<rhi::AccelerationStructureHandle>("create_acceleration_structure");
        }
        if (!enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_acceleration_structure: requires enabled Feature::AccelerationStructures.");
        }
        if (vkCreateAccelerationStructureKHR == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_acceleration_structure: acceleration-structure entry points are not loaded.");
        }
        if (desc.size == 0) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_acceleration_structure: size must be non-zero.");
        }

        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = desc.size,
            .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
        auto backing_buffer = allocator_->create_buffer(logical_device_->vk_handle(), buffer_info, allocation_info);
        if (!backing_buffer) {
            return rhi_error_from_graphics(backing_buffer.error());
        }

        auto acceleration_structure = VulkanAccelerationStructure::create(logical_device_->vk_handle(),
                                                                          backing_buffer->vk_handle(), 0, desc.size,
                                                                          to_vk(desc.type));
        if (!acceleration_structure) {
            return rhi_error_from_graphics(acceleration_structure.error());
        }

        return acceleration_structures_.insert(AccelerationStructureRecord{std::move(*backing_buffer),
                                                                           std::move(*acceleration_structure)});
    }

    void VulkanRhiDeviceBridge::destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept {
        acceleration_structures_.erase(handle);
    }

    rhi::RhiExpected<u64> VulkanRhiDeviceBridge::buffer_device_address(rhi::BufferHandle handle) const {
        if (!enabled_features_.has(rhi::Feature::BufferDeviceAddress)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "buffer_device_address: requires enabled Feature::BufferDeviceAddress.");
        }
        const BufferRecord *record = buffers_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "buffer_device_address: unknown buffer handle.");
        }
        return record->buffer.device_address();
    }

    rhi::RhiExpected<u64> VulkanRhiDeviceBridge::acceleration_structure_device_address(
        rhi::AccelerationStructureHandle handle) const {
        if (!enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "acceleration_structure_device_address: requires enabled Feature::AccelerationStructures.");
        }
        const AccelerationStructureRecord *record = acceleration_structures_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "acceleration_structure_device_address: unknown acceleration structure handle.");
        }
        return record->acceleration_structure.device_address();
    }

} // namespace SFT::Core::Vulkan
