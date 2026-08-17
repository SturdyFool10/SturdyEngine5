#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanAccelerationStructure.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanCommandBuffer.hpp>
#include <Core/Vulkan/VulkanCommandPool.hpp>
#include <Core/Vulkan/VulkanDescriptors.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanPipeline.hpp>
#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridgeComposition.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridgeFullScreenExclusive.hpp>
#include <Core/Vulkan/VulkanQueryPool.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiResourcePool.hpp>
#include <Core/Vulkan/VulkanSampler.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

using std::span;
using std::unique_ptr;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;


    class VulkanRhiDeviceBridge final : public rhi::RhiDevice {
      public:
        /// Constructs a `VulkanRhiDeviceBridge` from the supplied initialization values.
        ///
        /// @param backend Backend value to inspect, select, or convert.
        /// @param instance Instance used or affected by the operation.
        /// @param physical_device Device used or affected by the operation.
        /// @param logical_device Device used or affected by the operation.
        /// @param graphics_queue Queue used or affected by the operation.
        /// @param present_queue Queue used or affected by the operation.
        /// @param compute_queue Queue used or affected by the operation.
        /// @param transfer_queue Queue used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param feature_report `feature_report` value used by the operation.
        /// @param enable_native_access_extension Whether the associated behavior is enabled.
        /// @param hdr_swapchain_colorspace_enabled Swapchain used or affected by the operation.
        /// @param hdr_metadata_enabled `hdr_metadata_enabled` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        VulkanRhiDeviceBridge(VulkanBackend &backend,
                              VkInstance instance,
                              const VulkanPhysicalDevice &physical_device,
                              VulkanDevice &logical_device,
                              VulkanQueue &graphics_queue,
                              VulkanQueue &present_queue,
                              VulkanQueue *compute_queue,
                              VulkanQueue *transfer_queue,
                              VulkanAllocator &allocator,
                              rhi::FeatureNegotiationReport feature_report,
                              bool enable_native_access_extension = false,
                              bool hdr_swapchain_colorspace_enabled = false,
                              bool hdr_metadata_enabled = false);

        /// Destroys the `VulkanRhiDeviceBridge` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanRhiDeviceBridge() override;


        /// Returns the current or globally available backend type value.
        ///
        /// @return Returns the current backend type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::BackendType backend_type() const noexcept override;
        /// Returns the current adapter info.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::AdapterInfo &adapter_info() const noexcept override;
        /// Returns the current or globally available limits value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override;
        /// Returns the current or globally available feature negotiation report value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::FeatureNegotiationReport &feature_negotiation_report() const noexcept override;
        /// Returns the current or globally available enabled features value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::FeatureSet &enabled_features() const noexcept override;
        /// Returns the current or globally available feature properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override;
        /// Returns the current or globally available queue infos value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override;
        /// Returns the current or globally available enabled extensions value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const rhi::ExtensionId> enabled_extensions() const noexcept override;
        /// Performs the extension interface operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param extension `extension` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::RhiDeviceExtension *extension_interface(rhi::ExtensionId extension) noexcept override;


        /// Creates a buffer from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::BufferHandle> create_buffer(const rhi::BufferDesc &desc) override;
        /// Destroys the buffer identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_buffer(rhi::BufferHandle handle) noexcept override;
        /// Creates a texture from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::TextureHandle> create_texture(const rhi::TextureDesc &desc) override;
        /// Destroys the texture identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_texture(rhi::TextureHandle handle) noexcept override;
        /// Creates a texture view from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::TextureViewHandle> create_texture_view(const rhi::TextureViewDesc &desc) override;
        /// Destroys the texture view identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_texture_view(rhi::TextureViewHandle handle) noexcept override;
        /// Creates a sampler from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SamplerHandle> create_sampler(const rhi::SamplerDesc &desc) override;
        /// Destroys the sampler identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_sampler(rhi::SamplerHandle handle) noexcept override;
        /// Creates a shader module from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::ShaderModuleHandle> create_shader_module(const rhi::ShaderModuleDesc &desc) override;
        /// Destroys the shader module identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_shader_module(rhi::ShaderModuleHandle handle) noexcept override;
        /// Creates a bind group layout from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::BindGroupLayoutHandle> create_bind_group_layout(const rhi::BindGroupLayoutDesc &desc) override;
        /// Destroys the bind group layout identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bind_group_layout(rhi::BindGroupLayoutHandle handle) noexcept override;
        /// Creates a bind group from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::BindGroupHandle> create_bind_group(const rhi::BindGroupDesc &desc) override;
        /// Destroys the bind group identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_bind_group(rhi::BindGroupHandle handle) noexcept override;
        /// Creates a pipeline layout from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::PipelineLayoutHandle> create_pipeline_layout(const rhi::PipelineLayoutDesc &desc) override;
        /// Destroys the pipeline layout identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_pipeline_layout(rhi::PipelineLayoutHandle handle) noexcept override;


        /// Creates a render pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::RenderPipelineHandle> create_render_pipeline(const rhi::RenderPipelineDesc &desc) override;
        /// Destroys the render pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept override;
        /// Creates a compute pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::ComputePipelineHandle> create_compute_pipeline(const rhi::ComputePipelineDesc &desc) override;
        /// Destroys the compute pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept override;
        /// Creates a ray tracing pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::RayTracingPipelineHandle> create_ray_tracing_pipeline(
            const rhi::RayTracingPipelineDesc &desc) override;
        /// Destroys the ray tracing pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept override;
        /// Writes ray tracing shader group handles to the associated destination.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        /// @param first_group `first_group` value used by the operation.
        /// @param group_count Number of elements or operations to process.
        /// @param dst Destination value or resource.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult write_ray_tracing_shader_group_handles(
            rhi::RayTracingPipelineHandle pipeline, u32 first_group, u32 group_count, span<std::byte> dst) override;


        /// Performs the acceleration structure build sizes operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> acceleration_structure_build_sizes(
            const rhi::AccelerationStructureBuildDesc &desc) const override;
        /// Creates a acceleration structure from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureHandle> create_acceleration_structure(
            const rhi::AccelerationStructureDesc &desc) override;
        /// Destroys the acceleration structure identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept override;
        /// Performs the buffer device address operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<u64> buffer_device_address(rhi::BufferHandle buffer) const override;
        /// Performs the acceleration structure device address operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<u64> acceleration_structure_device_address(
            rhi::AccelerationStructureHandle handle) const override;


        /// Writes buffer to the associated destination.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        /// Maps buffer for access.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<span<std::byte>> map_buffer(rhi::BufferHandle buffer) override;
        /// Unmaps buffer.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void unmap_buffer(rhi::BufferHandle buffer) noexcept override;


        /// Creates a command encoder from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> create_command_encoder(const rhi::CommandEncoderDesc &desc) override;
        /// Destroys the command buffer identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept override;
        /// Creates a render bundle encoder from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> create_render_bundle_encoder(const rhi::RenderBundleDesc &desc) override;
        /// Destroys the render bundle identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept override;
        /// Submits the requested work.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult submit(const rhi::SubmitDesc &desc) override;
        /// Creates a surface from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> create_surface(const rhi::SurfaceDesc &desc) override;


        /// Imports surface using the supplied arguments and current state.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> import_surface(VkSurfaceKHR surface, const rhi::SurfaceDesc &desc);
        /// Destroys the surface identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_surface(rhi::SurfaceHandle handle) noexcept override;
        /// Creates a swapchain from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SwapchainHandle> create_swapchain(const rhi::SwapchainDesc &desc) override;
        /// Queries HDR capabilities from the active backend or runtime state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            rhi::SurfaceHandle handle) const override;
        /// Updates HDR content light level from the supplied values.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param update `update` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult update_hdr_content_light_level(
            rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) override;
        /// Destroys the swapchain identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_swapchain(rhi::SwapchainHandle handle) noexcept override;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::PresentationResolution presentation_resolution(rhi::SwapchainHandle handle) const noexcept override;
        /// Acquires next texture.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        /// @param frame_slot_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceTexture> acquire_next_texture(rhi::SwapchainHandle swapchain, u32 frame_slot_index) override;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param queue_lock_wait_ms Queue used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::PresentOutcome> present(const rhi::PresentDesc &desc, f64 *queue_lock_wait_ms = nullptr) override;

        /// Creates a semaphore from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SemaphoreHandle> create_semaphore(const rhi::SemaphoreDesc &desc) override;
        /// Destroys the semaphore identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_semaphore(rhi::SemaphoreHandle handle) noexcept override;
        /// Performs the semaphore value operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<u64> semaphore_value(rhi::SemaphoreHandle handle) const override;
        /// Waits for semaphore to complete.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param value Value consumed by the operation.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns = rhi::wait_forever) override;
        /// Signals semaphore.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult signal_semaphore(rhi::SemaphoreHandle handle, u64 value) override;
        /// Creates a fence from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::FenceHandle> create_fence(const rhi::FenceDesc &desc) override;
        /// Destroys the fence identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_fence(rhi::FenceHandle handle) noexcept override;
        /// Waits for fences to complete.
        ///
        /// @param fences Fence used or affected by the operation.
        /// @param wait_all `wait_all` value used by the operation.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<bool> wait_fences(span<const rhi::FenceHandle> fences, bool wait_all = true, u64 timeout_ns = rhi::wait_forever) override;
        /// Resets fences to its baseline state.
        ///
        /// @param fences Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult reset_fences(span<const rhi::FenceHandle> fences) override;
        /// Creates a query set from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::QuerySetHandle> create_query_set(const rhi::QuerySetDesc &desc) override;
        /// Destroys the query set identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void destroy_query_set(rhi::QuerySetHandle handle) noexcept override;
        /// Returns the query set results associated with this `VulkanRhiDeviceBridge`.
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
        [[nodiscard]] rhi::RhiResult get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count,
                                                            span<std::byte> dst, u64 stride,
                                                            rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
        /// Resets query set to its baseline state.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note This function does not throw exceptions.
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept override;

        /// Waits for idle to complete.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept override;

      private:


        struct BufferRecord {
            VulkanBuffer buffer;
            rhi::MemoryLocation memory = rhi::MemoryLocation::DeviceLocal;
        };


        struct TextureRecord {
            VulkanImage image;
            rhi::Format format = rhi::Format::Undefined;
        };


        struct BindGroupLayoutRecord {
            VulkanDescriptorSetLayout layout;
            vector<rhi::BindGroupLayoutEntry> entries;
        };


        struct DescriptorPoolChunk {
            VulkanDescriptorPool pool;
            u32 live_sets = 0;
        };


        struct BindGroupRecord {
            VulkanDescriptorPool pool;
            VkDescriptorSet set = VK_NULL_HANDLE;
            i32 shared_chunk_index = -1;
        };

        struct PipelineRecord {
            VulkanPipeline pipeline;
            rhi::PipelineLayoutHandle layout{};
        };

        struct CommandBufferRecord {
            VulkanCommandPool pool;
            VulkanCommandBuffer command_buffer;
            rhi::QueueLane queue{};
        };


        struct UploadResources {
            VulkanCommandPool command_pool;
            VulkanFence fence;
        };

        struct RenderBundleRecord {
            VulkanCommandPool pool;
            VulkanCommandBuffer command_buffer;
        };

        struct AccelerationStructureRecord {
            VulkanBuffer backing_buffer;
            VulkanAccelerationStructure acceleration_structure;
        };

        struct SurfaceRecord {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            bool owns_surface = true;


            rhi::SurfaceDesc desc{};
            std::string stored_label{};
        };


        /// Creates a surface record value from the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param owns_surface Surface used or affected by the operation.
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static SurfaceRecord make_surface_record(VkSurfaceKHR surface, bool owns_surface,
                                                                const rhi::SurfaceDesc &desc);

        struct SwapchainRecord {
            VulkanSwapchain swapchain;
            rhi::SurfaceHandle surface{};
            vector<rhi::TextureHandle> textures;
            vector<rhi::TextureViewHandle> views;
            vector<VulkanSemaphore> image_available_semaphores;
            vector<u32> image_available_signal_indices;
            vector<VulkanSemaphore> render_finished_semaphores;
            u32 current_image = ~0u;
            bool current_suboptimal = false;


            rhi::PresentationResolution presentation_resolution{};


            bool present_via_compute = false;


            VkHdrMetadataEXT stored_hdr_metadata{};
            bool has_hdr_metadata = false;


            CompositionSwapchainResources composition{};


            u32 composition_sync_interval = 1;


            VkFormat composition_vk_format = VK_FORMAT_UNDEFINED;
            GraphicsPlatform::CompositionAlphaMode composition_alpha_mode =
                GraphicsPlatform::CompositionAlphaMode::Ignore;


            bool owns_composition_resources = false;


            bool full_screen_exclusive_active = false;

            /// Reports whether composition present holds for this `SwapchainRecord`.
            ///
            /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] bool is_composition_present() const noexcept;
        };

        /// Converts the supplied engine/RHI value to its Vulkan representation.
        ///
        /// @param geometry `geometry` value used by the operation.
        /// @param bridge `bridge` value used by the operation.
        ///
        /// @return Returns the value converted to Vulkan geometry representation.
        /// @note This function does not throw exceptions.
        friend VkAccelerationStructureGeometryKHR to_vk_geometry(
            const rhi::AccelerationStructureGeometryDesc &geometry,
            const VulkanRhiDeviceBridge &bridge) noexcept;
        friend class VulkanRhiEncoderCommon;
        friend class VulkanRhiCommandEncoder;
        friend class VulkanRhiRenderPassEncoder;
        friend class VulkanRhiComputePassEncoder;
        friend class VulkanRhiRenderBundleEncoder;


        /// Returns the current or globally available device not ready value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <typename Value>
        [[nodiscard]] static rhi::RhiExpected<Value> device_not_ready(const char *operation) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  std::string("Vulkan RHI bridge cannot run ") + operation + ": device resources are not ready.");
        }
        /// Performs the RHI error from graphics operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param error Error value describing the failure.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static std::unexpected<rhi::RhiError> rhi_error_from_graphics(const GraphicsBackendError &error);
        /// Performs the queue for lane operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanQueue *queue_for_lane(rhi::QueueLane lane) const noexcept;
        /// Performs the queue family for lane operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 queue_family_for_lane(rhi::QueueLane lane) const noexcept;
        /// Validates queue lane.
        ///
        /// @param lane `lane` value used by the operation.
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult validate_queue_lane(rhi::QueueLane lane, const char *operation) const;


        /// Uploads via staging using the supplied arguments and current state.
        ///
        /// @param destination Destination value or resource.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult upload_via_staging(VulkanBuffer &destination, u64 offset, span<const std::byte> data);


        /// Acquires upload resources.
        ///
        /// @return Returns the current acquire upload resources value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UploadResources acquire_upload_resources();


        /// Releases upload resources using the supplied arguments and current state.
        ///
        /// @param resources `resources` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void release_upload_resources(UploadResources resources) noexcept;


        /// Performs the checkout command buffer operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param family_index Zero-based index of the target element or entry.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<CommandBufferRecord> checkout_command_buffer(u32 family_index) noexcept;


        /// Performs the return command buffer operation for `VulkanRhiDeviceBridge` using the supplied arguments.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void return_command_buffer(CommandBufferRecord &&record) noexcept;

        VulkanBackend *backend_ = nullptr;
        VkInstance instance_ = VK_NULL_HANDLE;
        const VulkanPhysicalDevice *physical_device_ = nullptr;
        VulkanDevice *logical_device_ = nullptr;
        VulkanQueue *graphics_queue_ = nullptr;


        VulkanQueue *present_queue_ = nullptr;
        VulkanAllocator *allocator_ = nullptr;


        VulkanPipelineCache pipeline_cache_{};


        Async::Mutex<u8> pipeline_cache_mutex_{0};

        rhi::AdapterInfo adapter_info_{};
        rhi::DeviceLimits limits_{};
        rhi::FeatureNegotiationReport feature_report_{};
        rhi::FeatureSet enabled_features_{};
        rhi::FeatureProperties feature_properties_{};
        vector<rhi::QueueInfo> queue_infos_{};
        vector<rhi::ExtensionId> enabled_extensions_{};
        bool hdr_swapchain_colorspace_enabled_ = false;
        bool hdr_metadata_enabled_ = false;

        VulkanRhiResourcePool<rhi::BufferHandle, BufferRecord> buffers_;
        VulkanRhiResourcePool<rhi::TextureHandle, TextureRecord> textures_;
        VulkanRhiResourcePool<rhi::TextureViewHandle, VulkanImageView> texture_views_;
        VulkanRhiResourcePool<rhi::SamplerHandle, VulkanSampler> samplers_;
        VulkanRhiResourcePool<rhi::ShaderModuleHandle, VkShaderModule> shader_modules_;
        VulkanRhiResourcePool<rhi::BindGroupLayoutHandle, BindGroupLayoutRecord> bind_group_layouts_;
        VulkanRhiResourcePool<rhi::BindGroupHandle, BindGroupRecord> bind_groups_;


        Async::Mutex<vector<DescriptorPoolChunk>> descriptor_pool_chunks_;
        VulkanRhiResourcePool<rhi::PipelineLayoutHandle, VulkanPipelineLayout> pipeline_layouts_;
        VulkanRhiResourcePool<rhi::RenderPipelineHandle, PipelineRecord> render_pipelines_;
        VulkanRhiResourcePool<rhi::ComputePipelineHandle, PipelineRecord> compute_pipelines_;
        VulkanRhiResourcePool<rhi::RayTracingPipelineHandle, PipelineRecord> ray_tracing_pipelines_;
        VulkanRhiResourcePool<rhi::AccelerationStructureHandle, AccelerationStructureRecord> acceleration_structures_;
        VulkanRhiResourcePool<rhi::CommandBufferHandle, CommandBufferRecord> command_buffers_;
        VulkanRhiResourcePool<rhi::RenderBundleHandle, RenderBundleRecord> render_bundles_;
        VulkanRhiResourcePool<rhi::SemaphoreHandle, VulkanSemaphore> semaphores_;
        VulkanRhiResourcePool<rhi::FenceHandle, VulkanFence> fences_;
        VulkanRhiResourcePool<rhi::QuerySetHandle, VulkanQueryPool> query_sets_;
        VulkanRhiResourcePool<rhi::SurfaceHandle, SurfaceRecord> surfaces_;
        VulkanRhiResourcePool<rhi::SwapchainHandle, SwapchainRecord> swapchains_;


        Async::Mutex<vector<UploadResources>> upload_pool_;


        Async::Mutex<std::unordered_map<u32, vector<CommandBufferRecord>>> command_buffer_free_list_;


        VulkanQueue *compute_queue_ = nullptr;
        VulkanQueue *transfer_queue_ = nullptr;


        std::optional<VulkanNativeAccessExtension> native_access_extension_;
    };

} // namespace SFT::Core::Vulkan
