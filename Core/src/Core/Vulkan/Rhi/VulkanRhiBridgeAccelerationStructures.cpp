
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

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanAccelerationStructure.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns the value converted to Vulkan representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr VkAccelerationStructureTypeKHR to_vk(rhi::AccelerationStructureType type) noexcept {
            switch (type) {
                case rhi::AccelerationStructureType::BottomLevel: return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                case rhi::AccelerationStructureType::TopLevel: return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            }
            return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        }

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to Vulkan representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr VkBuildAccelerationStructureFlagsKHR to_vk(rhi::AccelerationStructureBuildFlags flags) noexcept {
            VkBuildAccelerationStructureFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowUpdate)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::AllowCompaction)) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastTrace)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::PreferFastBuild)) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureBuildFlags::MinimizeMemory)) out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
            return out;
        }

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value converted to Vulkan representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr VkGeometryFlagsKHR to_vk(rhi::AccelerationStructureGeometryFlags flags) noexcept {
            VkGeometryFlagsKHR out = 0;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::Opaque)) out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
            if (rhi::has_any(flags, rhi::AccelerationStructureGeometryFlags::NoDuplicateAnyHitInvocation)) out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            return out;
        }

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value converted to Vulkan representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr VkCopyAccelerationStructureModeKHR to_vk(rhi::AccelerationStructureCopyMode mode) noexcept {
            switch (mode) {
                case rhi::AccelerationStructureCopyMode::Clone: return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
                case rhi::AccelerationStructureCopyMode::Compact: return VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                case rhi::AccelerationStructureCopyMode::Serialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
                case rhi::AccelerationStructureCopyMode::Deserialize: return VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
            }
            return VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
        }

    /// Performs the device address const operation for `Vulkan` using the supplied arguments.
    ///
    /// @param address `address` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] VkDeviceOrHostAddressConstKHR device_address_const(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressConstKHR out{};
            out.deviceAddress = address;
            return out;
        }

    /// Performs the device address operation for `Vulkan` using the supplied arguments.
    ///
    /// @param address `address` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] VkDeviceOrHostAddressKHR device_address(VkDeviceAddress address) noexcept {
            VkDeviceOrHostAddressKHR out{};
            out.deviceAddress = address;
            return out;
        }

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param geometry `geometry` value used by the operation.
    /// @param bridge `bridge` value used by the operation.
    /// @param omm_storage Caller-owned storage for the OMM linkage struct this geometry's `pNext`
    /// may need to point at; must be reserve()d to at least the total geometry count beforehand so a
    /// push_back() here never reallocates and invalidates a pointer an earlier geometry already took.
    ///
    /// @return Returns the value converted to Vulkan geometry representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] VkAccelerationStructureGeometryKHR to_vk_geometry(
            const rhi::AccelerationStructureGeometryDesc &geometry,
            const VulkanRhiDeviceBridge &bridge,
            vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> &omm_storage) noexcept {
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
                    if (geometry.triangles.opacity_micromap.is_valid()) {
                        const auto *micromap = bridge.opacity_micromaps_.find(geometry.triangles.opacity_micromap);
                        const auto *omm_indices = bridge.buffers_.find(geometry.triangles.opacity_micromap_index_buffer);
                        if (micromap != nullptr && omm_indices != nullptr) {
                            omm_storage.push_back(VkAccelerationStructureTrianglesOpacityMicromapEXT{
                                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,
                                .indexType = to_vk(geometry.triangles.opacity_micromap_index_format),
                                .indexBuffer = device_address_const(omm_indices->buffer.device_address() +
                                                                    geometry.triangles.opacity_micromap_index_offset),
                                .indexStride = static_cast<VkDeviceSize>(
                                    geometry.triangles.opacity_micromap_index_format == rhi::IndexFormat::Uint16 ? 2 : 4),
                                .baseTriangle = 0,
                                .usageCountsCount = 0,
                                .pUsageCounts = nullptr,
                                .micromap = micromap->micromap,
                            });
                            out.geometry.triangles.pNext = &omm_storage.back();
                        }
                    }
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

    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the value converted to Vulkan range representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] VkAccelerationStructureBuildRangeInfoKHR to_vk_range(
            const rhi::AccelerationStructureBuildRangeInfo &range) noexcept {
            return VkAccelerationStructureBuildRangeInfoKHR{
                .primitiveCount = range.primitive_count,
                .primitiveOffset = range.primitive_offset,
                .firstVertex = range.first_vertex,
                .transformOffset = range.transform_offset,
            };
        }

    /// Performs the acceleration structure build sizes operation for `Vulkan` using the supplied arguments.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
    rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> VulkanRhiDeviceBridge::acceleration_structure_build_sizes(
        const rhi::AccelerationStructureBuildDesc &desc) const {
        ZoneScopedN("VulkanRhiDeviceBridge::acceleration_structure_build_sizes");
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
        vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> omm_storage;
        geometries.reserve(desc.geometries.size());
        omm_storage.reserve(desc.geometries.size());
        for (const rhi::AccelerationStructureGeometryDesc &geometry : desc.geometries) {
            geometries.push_back(to_vk_geometry(geometry, *this, omm_storage));
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

    /// Creates a acceleration structure from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::AccelerationStructureHandle> VulkanRhiDeviceBridge::create_acceleration_structure(
        const rhi::AccelerationStructureDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_acceleration_structure");
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

    /// Destroys the acceleration structure identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_acceleration_structure");
        acceleration_structures_.erase(handle);
    }

    /// Performs the buffer device address operation for `Vulkan` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<u64> VulkanRhiDeviceBridge::buffer_device_address(rhi::BufferHandle handle) const {
        ZoneScopedN("VulkanRhiDeviceBridge::buffer_device_address");
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

    /// Reports opacity micromap build sizes for `Vulkan` using the supplied arguments.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`.
    /// @note VK_EXT_opacity_micromap is a newer, less-widely-used extension than the rest of this
    /// file's ray-tracing surface; its exact struct layout here (VkMicromapBuildInfoEXT and
    /// vkGetMicromapBuildSizesEXT's parameter shape) is implemented from a lower-confidence memory of
    /// the spec than the acceleration-structure code above and has not been checked against a live
    /// SDK header this session -- verify against the actual Vulkan headers before trusting this path.
    rhi::RhiExpected<rhi::OpacityMicromapBuildSizes> VulkanRhiDeviceBridge::opacity_micromap_build_sizes(
        const rhi::OpacityMicromapDesc &desc) const {
        ZoneScopedN("VulkanRhiDeviceBridge::opacity_micromap_build_sizes");
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::OpacityMicromapBuildSizes>("opacity_micromap_build_sizes");
        }
        if (!enabled_features_.has(rhi::Feature::OpacityMicromap)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "opacity_micromap_build_sizes: requires enabled Feature::OpacityMicromap.");
        }
        if (vkGetMicromapBuildSizesEXT == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "opacity_micromap_build_sizes: opacity-micromap entry points are not loaded.");
        }

        vector<VkMicromapUsageEXT> usage_counts;
        usage_counts.reserve(desc.usage_counts.size());
        for (const rhi::OpacityMicromapUsageCount &usage : desc.usage_counts) {
            usage_counts.push_back(to_vk(usage));
        }
        const VkMicromapBuildInfoEXT build_info{
            .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT,
            .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
            .flags = 0,
            .mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT,
            .dstMicromap = VK_NULL_HANDLE,
            .usageCountsCount = static_cast<u32>(usage_counts.size()),
            .pUsageCounts = usage_counts.empty() ? nullptr : usage_counts.data(),
        };
        VkMicromapBuildSizesInfoEXT sizes{.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT};
        vkGetMicromapBuildSizesEXT(logical_device_->vk_handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                   &build_info, &sizes);
        return rhi::OpacityMicromapBuildSizes{
            .micromap_size = sizes.micromapSize,
            .build_scratch_size = sizes.buildScratchSize,
        };
    }

    /// Creates an opacity micromap from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    /// @param size Backing-storage size, from a prior opacity_micromap_build_sizes() call.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::OpacityMicromapHandle> VulkanRhiDeviceBridge::create_opacity_micromap(
        const rhi::OpacityMicromapDesc &desc, u64 size) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_opacity_micromap");
        (void)desc;
        if (allocator_ == nullptr || logical_device_ == nullptr) {
            return device_not_ready<rhi::OpacityMicromapHandle>("create_opacity_micromap");
        }
        if (!enabled_features_.has(rhi::Feature::OpacityMicromap)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_opacity_micromap: requires enabled Feature::OpacityMicromap.");
        }
        if (vkCreateMicromapEXT == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_opacity_micromap: opacity-micromap entry points are not loaded.");
        }
        if (size == 0) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_opacity_micromap: size must be non-zero.");
        }

        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
        auto backing_buffer = allocator_->create_buffer(logical_device_->vk_handle(), buffer_info, allocation_info);
        if (!backing_buffer) {
            return rhi_error_from_graphics(backing_buffer.error());
        }

        const VkMicromapCreateInfoEXT create_info{
            .sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT,
            .buffer = backing_buffer->vk_handle(),
            .offset = 0,
            .size = size,
            .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
        };
        VkMicromapEXT micromap = VK_NULL_HANDLE;
        if (const VkResult result =
                vkCreateMicromapEXT(logical_device_->vk_handle(), &create_info, nullptr, &micromap);
            result != VK_SUCCESS) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_opacity_micromap: vkCreateMicromapEXT failed.");
        }

        return opacity_micromaps_.insert(OpacityMicromapRecord{std::move(*backing_buffer), micromap});
    }

    /// Destroys the opacity micromap identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_opacity_micromap(rhi::OpacityMicromapHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_opacity_micromap");
        if (const OpacityMicromapRecord *record = opacity_micromaps_.find(handle);
            record != nullptr && vkDestroyMicromapEXT != nullptr && logical_device_ != nullptr) {
            vkDestroyMicromapEXT(logical_device_->vk_handle(), record->micromap, nullptr);
        }
        opacity_micromaps_.erase(handle);
    }

    /// Performs the acceleration structure device address operation for `Vulkan` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<u64> VulkanRhiDeviceBridge::acceleration_structure_device_address(
        rhi::AccelerationStructureHandle handle) const {
        ZoneScopedN("VulkanRhiDeviceBridge::acceleration_structure_device_address");
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
