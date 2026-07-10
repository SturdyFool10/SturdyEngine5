// Bridge construction, introspection, wait_idle/render_frame, and the shared error-conversion
// helpers every other VulkanRhiBridge*.cpp file relies on. Also VulkanBackend::installRhiBridge()/
// rhi_device() — the two-line glue that used to live in the now-removed flat VulkanBackendRhi.cpp.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#pragma endregion

module Sturdy.Core;

import :GraphicsBackendError;
import :VulkanAllocator;
import :VulkanBackend;
import :VulkanCommandPool;
import :VulkanDevice;
import :VulkanPhysicalDevice;
import :VulkanQueue;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import :VulkanSync;
import Sturdy.Foundation;
import Sturdy.RHI;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    VulkanRhiDeviceBridge::VulkanRhiDeviceBridge(VulkanBackend &backend,
                                                 VkInstance instance,
                                                 const VulkanPhysicalDevice &physical_device,
                                                 VulkanDevice &logical_device,
                                                 VulkanQueue &graphics_queue,
                                                 VulkanAllocator &allocator,
                                                 rhi::FeatureNegotiationReport feature_report)
        : backend_(&backend), instance_(instance), physical_device_(&physical_device),
          logical_device_(&logical_device), graphics_queue_(&graphics_queue), allocator_(&allocator),
          feature_report_(feature_report), enabled_features_(feature_report_.enabled_features()) {
        const VkPhysicalDeviceProperties &props = physical_device.properties();
        const VkPhysicalDeviceLimits &limits = props.limits;

        adapter_info_.name = physical_device.name().cpp_string();
        adapter_info_.vendor = physical_device.vendor_name();
        adapter_info_.driver_version = physical_device.driver_version_string().cpp_string();
        adapter_info_.api_version = physical_device.api_version_string().cpp_string();
        adapter_info_.backend = rhi::BackendType::Vulkan;
        adapter_info_.device_type = to_rhi_device_type(props.deviceType);
        adapter_info_.vendor_id = props.vendorID;
        adapter_info_.device_id = props.deviceID;
        adapter_info_.is_discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

        limits_.max_texture_dimension_2d = limits.maxImageDimension2D;
        limits_.max_texture_array_layers = limits.maxImageArrayLayers;
        limits_.max_bind_groups = limits.maxBoundDescriptorSets;
        limits_.max_push_constants_size = limits.maxPushConstantsSize;
        limits_.max_vertex_buffers = limits.maxVertexInputBindings;
        limits_.max_vertex_attributes = limits.maxVertexInputAttributes;
        limits_.max_color_attachments = limits.maxColorAttachments;
        limits_.max_compute_workgroup_size_x = limits.maxComputeWorkGroupSize[0];
        limits_.max_compute_workgroup_size_y = limits.maxComputeWorkGroupSize[1];
        limits_.max_compute_workgroup_size_z = limits.maxComputeWorkGroupSize[2];
        limits_.min_uniform_buffer_offset_alignment = limits.minUniformBufferOffsetAlignment;
        limits_.min_storage_buffer_offset_alignment = limits.minStorageBufferOffsetAlignment;
        limits_.timestamp_period_ns = limits.timestampPeriod;
        limits_.timestamp_valid_bits = physical_device.timestamp_valid_bits(graphics_queue.family_index());

        queue_infos_.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Graphics,
            .capabilities = rhi::QueueCapability::Graphics | rhi::QueueCapability::Compute |
                            rhi::QueueCapability::Transfer | rhi::QueueCapability::Present,
            .lane_count = 1,
            .dedicated = false,
            .label = "Vulkan graphics/present queue",
        });

        // Best-effort: backs write_buffer()'s staged upload path for DeviceLocal buffers (see
        // VulkanRhiBridgeBuffers.cpp). If either fails to create, upload_via_staging() reports
        // OperationFailed at the point of use rather than here — every other bridge capability is
        // independent of this pool/fence.
        if (auto pool = VulkanCommandPool::create(logical_device_->vk_handle(), graphics_queue_->family_index());
            pool.has_value()) {
            upload_command_pool_ = std::move(*pool);
        }
        if (auto fence = VulkanFence::create(logical_device_->vk_handle()); fence.has_value()) {
            upload_fence_ = std::move(*fence);
        }
    }

    rhi::BackendType VulkanRhiDeviceBridge::backend_type() const noexcept { return rhi::BackendType::Vulkan; }
    const rhi::AdapterInfo &VulkanRhiDeviceBridge::adapter_info() const noexcept { return adapter_info_; }
    const rhi::DeviceLimits &VulkanRhiDeviceBridge::limits() const noexcept { return limits_; }
    const rhi::FeatureNegotiationReport &VulkanRhiDeviceBridge::feature_negotiation_report() const noexcept { return feature_report_; }
    const rhi::FeatureSet &VulkanRhiDeviceBridge::enabled_features() const noexcept { return enabled_features_; }
    const rhi::FeatureProperties &VulkanRhiDeviceBridge::feature_properties() const noexcept { return feature_properties_; }
    std::span<const rhi::QueueInfo> VulkanRhiDeviceBridge::queue_infos() const noexcept { return queue_infos_; }
    std::span<const rhi::ExtensionId> VulkanRhiDeviceBridge::enabled_extensions() const noexcept { return enabled_extensions_; }
    rhi::RhiDeviceExtension *VulkanRhiDeviceBridge::extension_interface(rhi::ExtensionId) noexcept { return nullptr; }

    void VulkanRhiDeviceBridge::wait_idle() noexcept {
        if (logical_device_ != nullptr) {
            logical_device_->wait_idle();
        }
    }



    std::unexpected<rhi::RhiError> VulkanRhiDeviceBridge::rhi_error_from_graphics(const GraphicsBackendError &error) {
        rhi::RhiErrorCode code = rhi::RhiErrorCode::OperationFailed;
        switch (error.code) {
            case GraphicsBackendErrorCode::Unsupported: code = rhi::RhiErrorCode::Unsupported; break;
            case GraphicsBackendErrorCode::OutOfMemory: code = rhi::RhiErrorCode::OutOfMemory; break;
            case GraphicsBackendErrorCode::DeviceLost: code = rhi::RhiErrorCode::DeviceLost; break;
            case GraphicsBackendErrorCode::SurfaceLost: code = rhi::RhiErrorCode::SurfaceLost; break;
            case GraphicsBackendErrorCode::InitializationFailed:
            case GraphicsBackendErrorCode::OperationFailed:
                code = rhi::RhiErrorCode::OperationFailed;
                break;
        }

        return std::unexpected(rhi::RhiError{code, error.message});
    }

    rhi::RhiDevice *VulkanBackend::rhi_device() noexcept {
        return rhiDevice.get();
    }

    const rhi::RhiDevice *VulkanBackend::rhi_device() const noexcept {
        return rhiDevice.get();
    }

    void VulkanBackend::installRhiBridge() {
        rhiDevice = std::make_unique<VulkanRhiDeviceBridge>(*this, vulkan_instance, physicalDevice, logicalDevice, gfxQueue, vmaAllocator, feature_report_);
    }

} // namespace SFT::Core::Vulkan
