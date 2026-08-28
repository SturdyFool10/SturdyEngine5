#pragma once


#include <Core/D3D12/RHI/D3D12Common.hpp>
#include <Core/D3D12/RHI/D3D12Descriptors.hpp>
#include <Core/D3D12/RHI/D3D12NativeAccessExtension.hpp>
#include <Core/D3D12/RHI/D3D12ResourcePool.hpp>

#pragma region Imports
#include <dcomp.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/Mutex.hpp>

namespace D3D12MA {
    class Allocator;
    class Allocation;
} // namespace D3D12MA

namespace SFT::D3D12 {

    using std::span;
    using std::unique_ptr;
    using std::vector;

    class D3D12CommandEncoder;
    class D3D12RenderPassEncoder;
    class D3D12ComputePassEncoder;
    class D3D12RenderBundleEncoder;


    struct TableSlot {
        u32 binding = 0;
        u32 shader_register = 0;
        u32 table_offset = 0;
        u32 count = 1;
        rhi::BindingType type = rhi::BindingType::UniformBuffer;
        rhi::ShaderStage visibility = rhi::ShaderStage::None;
        rhi::BindingFlags flags = rhi::BindingFlags::None;
    };


    struct DynamicSlot {
        u32 binding = 0;
        u32 shader_register = 0;
        rhi::BindingType type = rhi::BindingType::UniformBuffer;
        rhi::ShaderStage visibility = rhi::ShaderStage::None;
    };

    struct BindGroupLayoutRecord {
        vector<rhi::BindGroupLayoutEntry> entries;
        vector<TableSlot> resource_slots;
        vector<TableSlot> sampler_slots;


        vector<DynamicSlot> dynamic_slots;
        u32 resource_descriptor_count = 0;
        u32 sampler_descriptor_count = 0;


        u32 variable_slot_index = ~0u;
    };


    struct SetRootParameters {
        i32 resource_table = -1;
        i32 sampler_table = -1;

        vector<i32> dynamic_root_parameters;
    };

    struct PipelineLayoutRecord {
        ComPtr<ID3D12RootSignature> root_signature;
        vector<rhi::BindGroupLayoutHandle> set_layouts;
        vector<SetRootParameters> sets;
        i32 push_constant_root_parameter = -1;


        u32 push_constant_values = 0;


        u64 root_signature_content_hash = 0;
    };

    struct BindGroupRecord {
        rhi::BindGroupLayoutHandle layout{};


        DescriptorRange resources{};
        DescriptorRange samplers{};


        vector<D3D12_GPU_VIRTUAL_ADDRESS> dynamic_addresses;
    };


    struct BufferRecord {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
        u64 size = 0;
        rhi::MemoryLocation memory = rhi::MemoryLocation::DeviceLocal;
        rhi::BufferUsage usage = rhi::BufferUsage::None;


        void *mapped = nullptr;


        bool gpu_upload_heap = false;

        /// Non-null when this resource was placed by D3D12MA (Sturdy::D3D12MA) rather than created as
        /// its own dedicated CreateCommittedResource allocation; owns the suballocation and must be
        /// Release()d (mirrors VulkanBuffer's VmaAllocation handle -- see VulkanBuffer.hpp).
        D3D12MA::Allocation *allocation = nullptr;
    };

    struct TextureRecord {
        ComPtr<ID3D12Resource> resource;
        rhi::Format format = rhi::Format::Undefined;
        DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
        rhi::TextureDimension dimension = rhi::TextureDimension::Dim2D;
        rhi::Extent3D extent{};
        u32 mip_levels = 1;
        u32 array_layers = 1;
        rhi::SampleCount samples = rhi::SampleCount::X1;
        rhi::TextureUsage usage = rhi::TextureUsage::None;


        bool is_swapchain_image = false;


        vector<D3D12_RESOURCE_STATES> legacy_states;

        /// Non-null when this resource was placed by D3D12MA rather than created as its own dedicated
        /// CreateCommittedResource allocation. Always null for `is_swapchain_image` textures, since
        /// those come from DXGI's own swapchain buffers, not this device's allocator.
        D3D12MA::Allocation *allocation = nullptr;
    };

    struct TextureViewRecord {
        rhi::TextureHandle texture{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        rhi::Format rhi_format = rhi::Format::Undefined;
        u32 base_mip_level = 0;
        u32 mip_level_count = 1;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;


        DescriptorRange srv{};
        DescriptorRange uav{};
        DescriptorRange rtv{};
        DescriptorRange dsv{};
    };

    struct SamplerRecord {
        DescriptorRange descriptor{};
    };

    struct ShaderModuleRecord {

        vector<std::byte> bytecode;
    };

    struct RenderPipelineRecord {
        ComPtr<ID3D12PipelineState> pipeline;
        rhi::PipelineLayoutHandle layout{};


        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;


        vector<u32> vertex_strides;
        bool is_mesh_pipeline = false;
    };

    struct ComputePipelineRecord {
        ComPtr<ID3D12PipelineState> pipeline;
        rhi::PipelineLayoutHandle layout{};
    };

    struct RayTracingPipelineRecord {
        ComPtr<ID3D12StateObject> state_object;
        ComPtr<ID3D12StateObjectProperties> properties;
        rhi::PipelineLayoutHandle layout{};


        vector<std::wstring> group_exports;
    };

    struct AccelerationStructureRecord {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
        u64 size = 0;
        rhi::AccelerationStructureType type = rhi::AccelerationStructureType::BottomLevel;
    };


    struct CommandBufferRecord {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ShaderVisibleDescriptorHeap resource_heap;
        ShaderVisibleDescriptorHeap sampler_heap;


        vector<ShaderVisibleDescriptorHeap> retired_heaps;


        vector<ComPtr<ID3D12Resource>> transient_uploads;


        vector<DescriptorRange> transient_resource_descriptors;
        vector<DescriptorRange> transient_rtv_descriptors;
        vector<DescriptorRange> transient_dsv_descriptors;
        rhi::QueueLane queue{};
        D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };

    struct RenderBundleRecord {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;


        vector<rhi::BindGroupHandle> referenced_bind_groups;

        /// Descriptor-table ranges this bundle's bind groups uploaded into the device's persistent
        /// bundle allocators (bundle_resource_descriptors_/bundle_sampler_descriptors_). Released by
        /// destroy_render_bundle() -- see PersistentShaderVisibleDescriptorAllocator's own doc comment
        /// for why bundles cannot use the per-frame ring allocator every other draw path uses.
        vector<DescriptorRange> resource_table_ranges;
        vector<DescriptorRange> sampler_table_ranges;

        /// D3D12 bundle command lists cannot call RSSetViewports/RSSetScissorRects at all (illegal on
        /// D3D12_COMMAND_LIST_TYPE_BUNDLE); the last value set_viewport()/set_scissor() was given
        /// during recording is captured here instead and applied by the parent pass's
        /// execute_bundles() on its own (DIRECT) command list immediately before ExecuteBundle, so a
        /// bundle that only ever sets one static viewport/scissor (the overwhelmingly common case)
        /// still behaves as requested. A bundle that changes viewport/scissor mid-recording only keeps
        /// the last value; there is no D3D12-legal way to vary it within a single bundle.
        std::optional<D3D12_VIEWPORT> viewport;
        std::optional<D3D12_RECT> scissor;

        /// SetSamplePositions is, like RSSetViewports/RSSetScissorRects, uncertain-to-illegal inside a
        /// D3D12 bundle (it is rasterizer/pixel-pattern state, not per-draw pipeline state), so the
        /// same capture-and-hoist treatment applies: the last value set_sample_locations() was given
        /// during recording is captured here and applied by execute_bundles() on its own DIRECT list
        /// immediately before ExecuteBundle.
        struct SampleLocations {
            u32 samples_per_pixel = 0;
            u32 grid_width = 1;
            u32 grid_height = 1;
            vector<D3D12_SAMPLE_POSITION> positions;
        };
        std::optional<SampleLocations> sample_locations;
    };

    struct SemaphoreRecord {


        ComPtr<ID3D12Fence> fence;
    };


    struct FenceRecord {
        ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        u64 next_value = 0;
        u64 wait_value = 0;


        bool signaled_without_gpu = false;
    };

    struct QuerySetRecord {
        ComPtr<ID3D12QueryHeap> heap;
        rhi::QueryType type = rhi::QueryType::Timestamp;
        u32 count = 0;
        rhi::PipelineStatistic statistics = rhi::PipelineStatistic::None;


        ComPtr<ID3D12Resource> readback;
        u64 readback_bytes = 0;
    };

    struct SurfaceRecord {
        HWND hwnd = nullptr;
        rhi::SurfaceDesc desc{};


        std::string stored_label;
    };

    struct SwapchainRecord {
        ComPtr<IDXGISwapChain4> swapchain;
        rhi::SurfaceHandle surface{};
        vector<rhi::TextureHandle> textures;
        vector<rhi::TextureViewHandle> views;
        u32 image_count = 0;
        u32 current_image = ~0u;
        u32 sync_interval = 1;
        UINT present_flags = 0;
        rhi::PresentationResolution presentation_resolution{};


        rhi::ColorSpace effective_color_space = rhi::ColorSpace::SrgbNonlinear;


        ComPtr<IDCompositionDevice> composition_device;
        ComPtr<IDCompositionTarget> composition_target;
        ComPtr<IDCompositionVisual> composition_visual;


        HANDLE frame_latency_waitable = nullptr;


        bool has_hdr_metadata = false;
        DXGI_HDR_METADATA_HDR10 stored_hdr_metadata{};

        /// True when this swapchain was built via `CreateSwapChainForHwnd` and successfully entered
        /// legacy DXGI exclusive fullscreen (`IDXGISwapChain::SetFullscreenState(TRUE, ...)`).
        /// `destroy_swapchain()` must call `SetFullscreenState(FALSE, nullptr)` before releasing a
        /// swapchain with this set, mirroring the Vulkan backend's `full_screen_exclusive_active`
        /// acquire/release pairing for `VK_EXT_full_screen_exclusive`.
        bool full_screen_exclusive_active = false;

        /// Reports whether composition present holds for this `SwapchainRecord`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_composition_present() const noexcept;
    };


    struct D3D12DeviceCreateInfo {
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter4> adapter;
        ComPtr<ID3D12Device> device;
        rhi::AdapterInfo adapter_info{};
        rhi::DeviceLimits limits{};
        rhi::FeatureNegotiationReport feature_report{};
        rhi::FeatureProperties feature_properties{};
        vector<rhi::QueueInfo> queue_infos;
        vector<rhi::ExtensionId> enabled_extensions;
        bool enhanced_barriers = false;
        bool debug_layer_enabled = false;
        bool allow_tearing = false;


        bool pipeline_library_supported = false;


        bool gpu_upload_heap_supported = false;
    };

    class D3D12Device final : public rhi::RhiDevice {
      public:
        /// Constructs a `D3D12Device` from the supplied initialization values.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit D3D12Device(D3D12DeviceCreateInfo &&info);
        /// Destroys the `D3D12Device` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~D3D12Device() override;


        /// Initializes the `D3D12Device` for use.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult initialize();


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
        /// Performs the extension interface operation for `D3D12Device` using the supplied arguments.
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
            rhi::RayTracingPipelineHandle pipeline,
            u32 first_group,
            u32 group_count,
            span<std::byte> dst) override;


        /// Performs the acceleration structure build sizes operation for `D3D12Device` using the supplied arguments.
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
        /// Reports opacity micromap build sizes for `D3D12Device` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Always returns RhiErrorCode::Unsupported -- see the .cpp definition's doc comment
        /// for why D3D12 opacity micromap support is not implemented.
        [[nodiscard]] rhi::RhiExpected<rhi::OpacityMicromapBuildSizes> opacity_micromap_build_sizes(
            const rhi::OpacityMicromapDesc &desc) const override;
        /// Creates an opacity micromap from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param size Backing-storage size, from a prior opacity_micromap_build_sizes() call.
        ///
        /// @return Always returns RhiErrorCode::Unsupported -- see the .cpp definition's doc comment.
        [[nodiscard]] rhi::RhiExpected<rhi::OpacityMicromapHandle> create_opacity_micromap(
            const rhi::OpacityMicromapDesc &desc, u64 size) override;
        /// Destroys the opacity micromap identified by the supplied parameters.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @note No-op: D3D12 never successfully creates an opacity micromap handle (see
        /// create_opacity_micromap()'s doc comment), so there is nothing to destroy.
        /// @note This function does not throw exceptions.
        void destroy_opacity_micromap(rhi::OpacityMicromapHandle handle) noexcept override;
        /// Performs the buffer device address operation for `D3D12Device` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<u64> buffer_device_address(rhi::BufferHandle buffer) const override;
        /// Performs the acceleration structure device address operation for `D3D12Device` using the supplied arguments.
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
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> create_command_encoder(
            const rhi::CommandEncoderDesc &desc) override;
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
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> create_render_bundle_encoder(
            const rhi::RenderBundleDesc &desc) override;
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
            rhi::SwapchainHandle handle,
            const rhi::HdrContentLightLevelUpdate &update) override;
        /// Acquires next texture.
        ///
        /// @param swapchain Swapchain used or affected by the operation.
        /// @param frame_slot_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceTexture> acquire_next_texture(rhi::SwapchainHandle swapchain,
                                                                                 u32 frame_slot_index) override;
        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param queue_lock_wait_ms Queue used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<rhi::PresentOutcome> present(const rhi::PresentDesc &desc,
                                                                    f64 *queue_lock_wait_ms = nullptr) override;


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
        /// Performs the semaphore value operation for `D3D12Device` using the supplied arguments.
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
        /// Returns the query set results associated with this `D3D12Device`.
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
        [[nodiscard]] rhi::RhiResult get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count, span<std::byte> dst, u64 stride, rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
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


        /// Finds buffer for build in the available state.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const BufferRecord *find_buffer_for_build(rhi::BufferHandle handle) const noexcept;

      private:
        friend class D3D12CommandEncoder;
        friend class D3D12RenderPassEncoder;
        friend class D3D12ComputePassEncoder;
        friend class D3D12RenderBundleEncoder;


        /// Performs the queue for lane operation for `D3D12Device` using the supplied arguments.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12CommandQueue *queue_for_lane(rhi::QueueLane lane) const noexcept;
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
        [[nodiscard]] rhi::RhiResult upload_via_staging(ID3D12Resource *destination, u64 offset, span<const std::byte> data);


        /// Writes via GPU upload heap to the associated destination.
        ///
        /// @param record `record` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult write_via_gpu_upload_heap(BufferRecord &record, u64 offset, span<const std::byte> data);


        /// Executes and wait.
        ///
        /// @param list `list` value used by the operation.
        /// @param queue Queue used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult execute_and_wait(ID3D12GraphicsCommandList *list, rhi::QueueClass queue);


        /// Performs the checkout command buffer operation for `D3D12Device` using the supplied arguments.
        ///
        /// @param list_type Type value to inspect, select, or convert.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<CommandBufferRecord> checkout_command_buffer(D3D12_COMMAND_LIST_TYPE list_type) noexcept;
        /// Performs the return command buffer operation for `D3D12Device` using the supplied arguments.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void return_command_buffer(CommandBufferRecord &&record) noexcept;


        /// Acquires command buffer.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<CommandBufferRecord> acquire_command_buffer(rhi::QueueLane lane);

        /// Creates a shader visible heap from the supplied parameters.
        ///
        /// @param type Type value to inspect, select, or convert.
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<ShaderVisibleDescriptorHeap> create_shader_visible_heap(
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            u32 capacity);


        /// Releases view descriptors using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void release_view_descriptors(TextureViewRecord &record) noexcept;
        /// Releases bind group descriptors using the supplied arguments and current state.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void release_bind_group_descriptors(BindGroupRecord &record) noexcept;


        /// Creates a swapchain textures from the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param usage Usage flags or category applied to the resource.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult create_swapchain_textures(SwapchainRecord &record, rhi::Format format, u32 width, u32 height, rhi::TextureUsage usage);
        /// Destroys the swapchain textures identified by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_swapchain_textures(SwapchainRecord &record) noexcept;


        ComPtr<IDXGIFactory6> factory_;
        ComPtr<IDXGIAdapter4> adapter_;
        ComPtr<ID3D12Device> device_;


        ComPtr<ID3D12Device5> device5_;
        ComPtr<ID3D12Device8> device8_;
        ComPtr<ID3D12Device10> device10_;


        Async::Mutex<ComPtr<ID3D12PipelineLibrary1>> pipeline_library_;

        ComPtr<ID3D12CommandQueue> graphics_queue_;


        ComPtr<ID3D12CommandQueue> compute_queue_;
        ComPtr<ID3D12CommandQueue> copy_queue_;


        struct BlockingFence {
            ComPtr<ID3D12Fence> fence;
            HANDLE event = nullptr;
            u64 next_value = 0;
        };
        Async::Mutex<BlockingFence> graphics_blocking_fence_;
        Async::Mutex<BlockingFence> compute_blocking_fence_;
        Async::Mutex<BlockingFence> copy_blocking_fence_;


        Async::Mutex<int> present_lock_;


        CpuDescriptorAllocator cpu_resource_descriptors_;
        CpuDescriptorAllocator cpu_sampler_descriptors_;
        CpuDescriptorAllocator cpu_rtv_descriptors_;
        CpuDescriptorAllocator cpu_dsv_descriptors_;

        /// Backs render-bundle bind-group uploads (persistent, not reset per frame). See
        /// PersistentShaderVisibleDescriptorAllocator's own doc comment for why bundles cannot share
        /// the per-frame ring heaps below.
        PersistentShaderVisibleDescriptorAllocator bundle_resource_descriptors_;
        PersistentShaderVisibleDescriptorAllocator bundle_sampler_descriptors_;


        rhi::AdapterInfo adapter_info_{};
        rhi::DeviceLimits limits_{};
        rhi::FeatureNegotiationReport feature_report_{};
        rhi::FeatureSet enabled_features_{};
        rhi::FeatureProperties feature_properties_{};
        vector<rhi::QueueInfo> queue_infos_;
        vector<rhi::ExtensionId> enabled_extensions_;
        // Engaged only when enabled_extensions_ carries D3D12NativeAccessExtension::id(), which is
        // what makes extension_interface() publish it. Holds borrowed pointers into this device's
        // own ComPtrs, so it must be constructed after those are live and never outlive them.
        std::optional<D3D12NativeAccessExtension> native_access_extension_;
        bool enhanced_barriers_ = false;
        bool debug_layer_enabled_ = false;
        bool allow_tearing_ = false;


        bool pipeline_library_supported_ = false;


        bool gpu_upload_heap_supported_ = false;

        /// Suballocates buffer/texture resources onto shared heaps via CreatePlacedResource instead
        /// of every resource getting its own dedicated CreateCommittedResource heap -- the D3D12
        /// counterpart to Sturdy::VMA on the Vulkan side (see VulkanAllocator.hpp). Created in
        /// initialize(), released in the destructor. Null (and every allocation falls back to
        /// CreateCommittedResource) if construction failed, so this is always null-checked before use.
        D3D12MA::Allocator *d3d12ma_allocator_ = nullptr;


        D3D12ResourcePool<rhi::BufferHandle, BufferRecord> buffers_;
        D3D12ResourcePool<rhi::TextureHandle, TextureRecord> textures_;
        D3D12ResourcePool<rhi::TextureViewHandle, TextureViewRecord> texture_views_;
        D3D12ResourcePool<rhi::SamplerHandle, SamplerRecord> samplers_;
        D3D12ResourcePool<rhi::ShaderModuleHandle, ShaderModuleRecord> shader_modules_;
        D3D12ResourcePool<rhi::BindGroupLayoutHandle, BindGroupLayoutRecord> bind_group_layouts_;
        D3D12ResourcePool<rhi::BindGroupHandle, BindGroupRecord> bind_groups_;
        D3D12ResourcePool<rhi::PipelineLayoutHandle, PipelineLayoutRecord> pipeline_layouts_;
        D3D12ResourcePool<rhi::RenderPipelineHandle, RenderPipelineRecord> render_pipelines_;
        D3D12ResourcePool<rhi::ComputePipelineHandle, ComputePipelineRecord> compute_pipelines_;
        D3D12ResourcePool<rhi::RayTracingPipelineHandle, RayTracingPipelineRecord> ray_tracing_pipelines_;
        D3D12ResourcePool<rhi::AccelerationStructureHandle, AccelerationStructureRecord> acceleration_structures_;
        D3D12ResourcePool<rhi::CommandBufferHandle, CommandBufferRecord> command_buffers_;
        D3D12ResourcePool<rhi::RenderBundleHandle, RenderBundleRecord> render_bundles_;
        D3D12ResourcePool<rhi::SemaphoreHandle, SemaphoreRecord> semaphores_;
        D3D12ResourcePool<rhi::FenceHandle, FenceRecord> fences_;
        D3D12ResourcePool<rhi::QuerySetHandle, QuerySetRecord> query_sets_;
        D3D12ResourcePool<rhi::SurfaceHandle, SurfaceRecord> surfaces_;
        D3D12ResourcePool<rhi::SwapchainHandle, SwapchainRecord> swapchains_;


        Async::Mutex<std::unordered_map<u32, vector<CommandBufferRecord>>> command_buffer_free_list_;


        enum class IndirectKind : u32 { Draw,
                                        DrawIndexed,
                                        Dispatch,
                                        DispatchMesh,
                                        DispatchRays };
        /// Performs the indirect signature operation for `D3D12Device` using the supplied arguments.
        ///
        /// @param kind `kind` value used by the operation.
        /// @param stride `stride` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<ID3D12CommandSignature *> indirect_signature(IndirectKind kind, u32 stride);
        Async::Mutex<std::unordered_map<u64, ComPtr<ID3D12CommandSignature>>> indirect_signatures_;
    };


    /// Builds acceleration structure inputs.
    ///
    /// @param device Device used or affected by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param geometry_storage `geometry_storage` value used by the operation.
    /// @param inputs `inputs` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] rhi::RhiResult build_acceleration_structure_inputs(
        const D3D12Device &device,
        const rhi::AccelerationStructureBuildDesc &desc,
        vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometry_storage,
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs);

} // namespace SFT::D3D12
