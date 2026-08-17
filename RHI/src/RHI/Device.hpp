#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#pragma endregion

#include "Binding.hpp"
#include "Command.hpp"
#include "Error.hpp"
#include "Execution.hpp"
#include "Extensions.hpp"
#include "Features.hpp"
#include "Handles.hpp"
#include "HdrDisplay.hpp"
#include "Pipeline.hpp"
#include "Queries.hpp"
#include "Queues.hpp"
#include "RayTracing.hpp"
#include "Resources.hpp"
#include "Shader.hpp"
#include "Swapchain.hpp"
#include "Types.hpp"

using std::span;
using std::string;
using std::unique_ptr;

namespace SFT::RHI {


    enum class BackendType : u32 {
        Vulkan,
        D3D12,
        Metal,
        WebGpu,
    };

    /// Returns a human-readable name for the supplied backend type value.
    ///
    /// @param backend Backend value to inspect, select, or convert.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr const char *backend_type_name(BackendType backend) noexcept {
        switch (backend) {
            case BackendType::Vulkan:
                return "Vulkan";
            case BackendType::D3D12:
                return "D3D12";
            case BackendType::Metal:
                return "Metal";
            case BackendType::WebGpu:
                return "WebGPU";
        }
        return "<unknown>";
    }


    enum class DeviceType : u32 {
        Other,
        IntegratedGpu,
        DiscreteGpu,
        VirtualGpu,
        Cpu,
    };

    /// Returns a human-readable name for the supplied device type value.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr const char *device_type_name(DeviceType type) noexcept {
        switch (type) {
            case DeviceType::Other:
                return "Other";
            case DeviceType::IntegratedGpu:
                return "Integrated GPU";
            case DeviceType::DiscreteGpu:
                return "Discrete GPU";
            case DeviceType::VirtualGpu:
                return "Virtual GPU";
            case DeviceType::Cpu:
                return "CPU (software)";
        }
        return "<unknown>";
    }


    struct AdapterInfo {
        string name;
        string vendor;
        string driver_version;
        string api_version;
        BackendType backend = BackendType::Vulkan;
        DeviceType device_type = DeviceType::Other;
        u32 vendor_id = 0;
        u32 device_id = 0;


        string physical_device_id;

        bool is_discrete = false;
    };


    struct DeviceLimits {
        u32 max_texture_dimension_2d = 0;
        u32 max_texture_array_layers = 0;
        u32 max_bind_groups = 0;
        u32 max_push_constants_size = 0;
        u32 max_vertex_buffers = 0;
        u32 max_vertex_attributes = 0;
        u32 max_color_attachments = 0;
        u32 max_framebuffer_sample_count = 1;

        u32 framebuffer_sample_counts = 1;
        bool supports_minimum_depth_resolve = false;


        bool supports_bc_texture_compression = false;
        u32 max_compute_workgroup_size_x = 0;
        u32 max_compute_workgroup_size_y = 0;
        u32 max_compute_workgroup_size_z = 0;
        u64 min_uniform_buffer_offset_alignment = 0;
        u64 min_storage_buffer_offset_alignment = 0;


        f32 timestamp_period_ns = 0.0f;
        u32 timestamp_valid_bits = 0;
    };


    class RhiDevice {
      public:
        /// Destroys the `RhiDevice` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~RhiDevice() = default;

        /// Disables this construction form for `RhiDevice`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiDevice(const RhiDevice &) = delete;
        /// Assigns a new value to this `RhiDevice`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiDevice &operator=(const RhiDevice &) = delete;
        /// Disables this construction form for `RhiDevice`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiDevice(RhiDevice &&) = delete;
        /// Assigns a new value to this `RhiDevice`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        RhiDevice &operator=(RhiDevice &&) = delete;


        /// Returns the current or globally available backend type value.
        ///
        /// @return Returns the current backend type value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual BackendType backend_type() const noexcept = 0;
        /// Returns the current adapter info.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const AdapterInfo &adapter_info() const noexcept = 0;
        /// Returns the current or globally available limits value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const DeviceLimits &limits() const noexcept = 0;


        /// Returns the current or globally available feature negotiation report value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const FeatureNegotiationReport &feature_negotiation_report() const noexcept = 0;


        /// Returns the current or globally available enabled features value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const FeatureSet &enabled_features() const noexcept = 0;


        /// Returns the current or globally available feature properties value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const FeatureProperties &feature_properties() const noexcept = 0;


        /// Returns the current or globally available queue infos value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual span<const QueueInfo> queue_infos() const noexcept = 0;


        /// Returns the current or globally available enabled extensions value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual span<const ExtensionId> enabled_extensions() const noexcept = 0;


        /// Reports whether enabled holds for this `RhiDevice`.
        ///
        /// @param feature `feature` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_enabled(Feature feature) const noexcept;

        /// Reports whether extension enabled holds for this `RhiDevice`.
        ///
        /// @param extension `extension` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_extension_enabled(ExtensionId extension) const noexcept;


        /// Performs the extension interface operation for `RhiDevice` using the supplied arguments.
        ///
        /// @param extension `extension` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual RhiDeviceExtension *extension_interface(ExtensionId extension) noexcept = 0;


        /// Creates a buffer from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<BufferHandle> create_buffer(const BufferDesc &desc) = 0;
        /// Destroys the buffer identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_buffer(BufferHandle handle) noexcept = 0;

        /// Creates a texture from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<TextureHandle> create_texture(const TextureDesc &desc) = 0;
        /// Destroys the texture identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_texture(TextureHandle handle) noexcept = 0;

        /// Creates a texture view from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<TextureViewHandle> create_texture_view(const TextureViewDesc &desc) = 0;
        /// Destroys the texture view identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_texture_view(TextureViewHandle handle) noexcept = 0;

        /// Creates a sampler from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SamplerHandle> create_sampler(const SamplerDesc &desc) = 0;
        /// Destroys the sampler identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_sampler(SamplerHandle handle) noexcept = 0;

        /// Creates a shader module from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<ShaderModuleHandle> create_shader_module(const ShaderModuleDesc &desc) = 0;
        /// Destroys the shader module identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_shader_module(ShaderModuleHandle handle) noexcept = 0;

        /// Creates a bind group layout from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<BindGroupLayoutHandle> create_bind_group_layout(const BindGroupLayoutDesc &desc) = 0;
        /// Destroys the bind group layout identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_bind_group_layout(BindGroupLayoutHandle handle) noexcept = 0;

        /// Creates a bind group from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<BindGroupHandle> create_bind_group(const BindGroupDesc &desc) = 0;
        /// Destroys the bind group identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_bind_group(BindGroupHandle handle) noexcept = 0;

        /// Creates a pipeline layout from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<PipelineLayoutHandle> create_pipeline_layout(const PipelineLayoutDesc &desc) = 0;
        /// Destroys the pipeline layout identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept = 0;

        /// Creates a render pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<RenderPipelineHandle> create_render_pipeline(const RenderPipelineDesc &desc) = 0;
        /// Destroys the render pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_render_pipeline(RenderPipelineHandle handle) noexcept = 0;

        /// Creates a compute pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<ComputePipelineHandle> create_compute_pipeline(const ComputePipelineDesc &desc) = 0;
        /// Destroys the compute pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept = 0;

        /// Creates a ray tracing pipeline from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<RayTracingPipelineHandle> create_ray_tracing_pipeline(
            const RayTracingPipelineDesc &desc) = 0;
        /// Destroys the ray tracing pipeline identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_ray_tracing_pipeline(RayTracingPipelineHandle handle) noexcept = 0;
        /// Writes ray tracing shader group handles to the associated destination.
        ///
        /// @param pipeline Pipeline used or affected by the operation.
        /// @param first_group `first_group` value used by the operation.
        /// @param group_count Number of elements or operations to process.
        /// @param dst Destination value or resource.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult write_ray_tracing_shader_group_handles(
            RayTracingPipelineHandle pipeline,
            u32 first_group,
            u32 group_count,
            span<std::byte> dst) = 0;

        /// Performs the acceleration structure build sizes operation for `RhiDevice` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<AccelerationStructureBuildSizes> acceleration_structure_build_sizes(
            const AccelerationStructureBuildDesc &desc) const = 0;
        /// Creates a acceleration structure from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<AccelerationStructureHandle> create_acceleration_structure(
            const AccelerationStructureDesc &desc) = 0;
        /// Destroys the acceleration structure identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_acceleration_structure(AccelerationStructureHandle handle) noexcept = 0;


        /// Performs the buffer device address operation for `RhiDevice` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<u64> buffer_device_address(BufferHandle buffer) const = 0;


        /// Performs the acceleration structure device address operation for `RhiDevice` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<u64> acceleration_structure_device_address(
            AccelerationStructureHandle handle) const = 0;


        /// Writes buffer to the associated destination.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult write_buffer(BufferHandle buffer, u64 offset, span<const std::byte> data) = 0;


        /// Maps buffer for access.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<span<std::byte>> map_buffer(BufferHandle buffer) = 0;
        /// Unmaps buffer.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void unmap_buffer(BufferHandle buffer) noexcept = 0;


        /// Creates a command encoder from the supplied parameters.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RhiExpected<unique_ptr<CommandEncoder>> create_command_encoder();
        /// Creates a command encoder from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<unique_ptr<CommandEncoder>> create_command_encoder(
            const CommandEncoderDesc &desc) = 0;
        /// Destroys the command buffer identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_command_buffer(CommandBufferHandle handle) noexcept = 0;

        /// Creates a render bundle encoder from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<unique_ptr<RenderBundleEncoder>> create_render_bundle_encoder(
            const RenderBundleDesc &desc) = 0;
        /// Destroys the render bundle identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_render_bundle(RenderBundleHandle handle) noexcept = 0;

        /// Submits the requested work.
        ///
        /// @param command_buffers Buffer used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RhiResult submit(span<const CommandBufferHandle> command_buffers);
        /// Submits the requested work.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult submit(const SubmitDesc &desc) = 0;


        /// Creates a surface from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SurfaceHandle> create_surface(const SurfaceDesc &desc) = 0;
        /// Destroys the surface identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_surface(SurfaceHandle handle) noexcept = 0;

        /// Creates a swapchain from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SwapchainHandle> create_swapchain(const SwapchainDesc &desc) = 0;
        /// Destroys the swapchain identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_swapchain(SwapchainHandle handle) noexcept = 0;


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual PresentationResolution presentation_resolution(SwapchainHandle handle) const noexcept = 0;


        /// Queries HDR capabilities from the active backend or runtime state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            SurfaceHandle handle) const = 0;


        /// Updates HDR content light level from the supplied values.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param update `update` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult update_hdr_content_light_level(
            SwapchainHandle handle,
            const HdrContentLightLevelUpdate &update) = 0;


        /// Acquires next texture.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        /// @param frame_slot_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SurfaceTexture> acquire_next_texture(SwapchainHandle swapchain, u32 frame_slot_index) = 0;


        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param queue_lock_wait_ms Queue used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<PresentOutcome> present(const PresentDesc &desc, f64 *queue_lock_wait_ms = nullptr) = 0;


        /// Creates a semaphore from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<SemaphoreHandle> create_semaphore(const SemaphoreDesc &desc) = 0;
        /// Destroys the semaphore identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_semaphore(SemaphoreHandle handle) noexcept = 0;
        /// Performs the semaphore value operation for `RhiDevice` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<u64> semaphore_value(SemaphoreHandle handle) const = 0;
        /// Waits for semaphore to complete.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param value Value consumed by the operation.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult wait_semaphore(SemaphoreHandle handle,
                                                       u64 value,
                                                       u64 timeout_ns = wait_forever) = 0;
        /// Signals semaphore.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult signal_semaphore(SemaphoreHandle handle, u64 value) = 0;

        /// Creates a fence from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<FenceHandle> create_fence(const FenceDesc &desc) = 0;
        /// Destroys the fence identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_fence(FenceHandle handle) noexcept = 0;


        /// Waits for fences to complete.
        ///
        /// @param fences Fence used or affected by the operation.
        /// @param wait_all `wait_all` value used by the operation.
        /// @param timeout_ns Maximum amount of time to wait before giving up.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<bool> wait_fences(span<const FenceHandle> fences,
                                                            bool wait_all = true,
                                                            u64 timeout_ns = wait_forever) = 0;
        /// Resets fences to its baseline state.
        ///
        /// @param fences Fence used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiResult reset_fences(span<const FenceHandle> fences) = 0;


        /// Creates a query set from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] virtual RhiExpected<QuerySetHandle> create_query_set(const QuerySetDesc &desc) = 0;
        /// Destroys the query set identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_query_set(QuerySetHandle handle) noexcept = 0;


        /// Returns the query set results associated with this `RhiDevice`.
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
        [[nodiscard]] virtual RhiResult get_query_set_results(QuerySetHandle query_set, u32 first, u32 count, span<std::byte> dst, u64 stride, QueryResultFlags flags = QueryResultFlags::Result64Bit) = 0;


        /// Resets query set to its baseline state.
        ///
        /// @param query_set `query_set` value used by the operation.
        /// @param first First position or element included in the operation.
        /// @param count Number of elements or operations to process.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void reset_query_set(QuerySetHandle query_set, u32 first, u32 count) noexcept = 0;


        /// Waits for idle to complete.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void wait_idle() noexcept = 0;

      protected:
        /// Constructs a `RhiDevice` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        RhiDevice() = default;
    };

} // namespace SFT::RHI
