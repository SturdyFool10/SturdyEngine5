#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#pragma endregion

#include "Error.hpp"
#include "Types.hpp"
#include "Handles.hpp"
#include "Features.hpp"
#include "Extensions.hpp"
#include "Queues.hpp"
#include "Resources.hpp"
#include "Shader.hpp"
#include "Binding.hpp"
#include "Pipeline.hpp"
#include "RayTracing.hpp"
#include "Queries.hpp"
#include "Execution.hpp"
#include "Command.hpp"
#include "Swapchain.hpp"

using std::span;
using std::string;
using std::unique_ptr;

namespace SFT::RHI {

    // Which concrete graphics API a device is driving. Reported by the device; also the switch a
    // higher layer reads to pick a backend factory. Every value exists in the enum regardless of which
    // backends are compiled in — availability is a runtime question answered by the backend registry
    // (see :Backend), not by the presence of the enumerator.
    enum class BackendType : u32 {
        Vulkan,
        D3D12,
        Metal,
        WebGpu,
    };

    [[nodiscard]] constexpr const char *backend_type_name(BackendType backend) noexcept {
        switch (backend) {
            case BackendType::Vulkan: return "Vulkan";
            case BackendType::D3D12: return "D3D12";
            case BackendType::Metal: return "Metal";
            case BackendType::WebGpu: return "WebGPU";
        }
        return "<unknown>";
    }

    // The broad category of an adapter, mirroring VkPhysicalDeviceType / DXGI adapter flags. `Cpu` is
    // a pure software rasterizer (lavapipe, SwiftShader, WARP) — the kind a selector usually excludes
    // by default. Selection filters and power-preference scoring key off this (see :Selection).
    enum class DeviceType : u32 {
        Other,         // present but uncategorized
        IntegratedGpu, // shares system memory with the CPU (laptops, APUs) — the low-power choice
        DiscreteGpu,   // dedicated GPU with its own VRAM — the high-performance choice
        VirtualGpu,    // a virtualized/paravirtual adapter (VMs, cloud)
        Cpu,           // software rasterizer, no real GPU
    };

    [[nodiscard]] constexpr const char *device_type_name(DeviceType type) noexcept {
        switch (type) {
            case DeviceType::Other: return "Other";
            case DeviceType::IntegratedGpu: return "Integrated GPU";
            case DeviceType::DiscreteGpu: return "Discrete GPU";
            case DeviceType::VirtualGpu: return "Virtual GPU";
            case DeviceType::Cpu: return "CPU (software)";
        }
        return "<unknown>";
    }

    // Backend-agnostic description of the adapter/GPU — plain strings/ints, no API types, so any
    // layer can log or display it. Same shape and intent as Core::GpuInfo, redeclared here to keep
    // the RHI self-contained.
    struct AdapterInfo {
        string name;
        string vendor;
        string driver_version;
        string api_version;
        BackendType backend = BackendType::Vulkan;
        DeviceType device_type = DeviceType::Other;
        u32 vendor_id = 0;
        u32 device_id = 0;
        // Retained convenience mirror of `device_type == DeviceType::DiscreteGpu`; the backend sets both.
        bool is_discrete = false;
    };

    // Hard numeric limits the caller must respect. Populated by the backend from the device; the
    // fields most descriptors actually bump against.
    struct DeviceLimits {
        u32 max_texture_dimension_2d = 0;
        u32 max_texture_array_layers = 0;
        u32 max_bind_groups = 0;
        u32 max_push_constants_size = 0;
        u32 max_vertex_buffers = 0;
        u32 max_vertex_attributes = 0;
        u32 max_color_attachments = 0;
        u32 max_compute_workgroup_size_x = 0;
        u32 max_compute_workgroup_size_y = 0;
        u32 max_compute_workgroup_size_z = 0;
        u64 min_uniform_buffer_offset_alignment = 0;
        u64 min_storage_buffer_offset_alignment = 0;
        // Nanoseconds a single timestamp-query tick represents — multiply an (end − start) tick delta
        // by this to get a pass's GPU time. 0 means the device can't timestamp. `timestamp_valid_bits`
        // is how many low bits of a written timestamp are meaningful (mask before subtracting).
        f32 timestamp_period_ns = 0.0f;
        u32 timestamp_valid_bits = 0;
    };

    // The heart of the RHI: the API-agnostic device. A concrete backend (Core's Vulkan backend
    // today; Metal/D3D12/WebGPU later) implements every method; higher layers describe all GPU work
    // in terms of the descriptors/handles/encoders above and never touch a graphics-API symbol.
    //
    // Ownership model: every `create_*` mints a handle the device owns; the matching `destroy_*`
    // releases it. Handles are plain values (see :Handles), so the caller tracks lifetimes itself —
    // the device does not reference-count them. Destroying a resource still referenced by in-flight
    // GPU work is a caller error; sequence teardown after wait_idle() (or the appropriate
    // frame-fence) just as the raw API would demand.
    //
    // The RHI intentionally provides no factory here — constructing a device is inherently
    // API-specific (instance/adapter/surface plumbing), so each backend exposes its own
    // `create_*_device(...)` entry point that returns a `unique_ptr<RhiDevice>`.
    class RhiDevice {
      public:
        virtual ~RhiDevice() = default;

        RhiDevice(const RhiDevice &) = delete;
        RhiDevice &operator=(const RhiDevice &) = delete;
        RhiDevice(RhiDevice &&) = delete;
        RhiDevice &operator=(RhiDevice &&) = delete;

        // ── Introspection ──
        [[nodiscard]] virtual BackendType backend_type() const noexcept = 0;
        [[nodiscard]] virtual const AdapterInfo &adapter_info() const noexcept = 0;
        [[nodiscard]] virtual const DeviceLimits &limits() const noexcept = 0;

        // Full request/support/enablement report from device creation. This records required features
        // that were satisfied, optional features that were partially enabled, and exact missing sets.
        [[nodiscard]] virtual const FeatureNegotiationReport &feature_negotiation_report() const noexcept = 0;

        // The optional features this device actually turned on — the requested required+optional set
        // intersected with what the adapter supported (see :Adapter's DeviceRequest). An app reads
        // this to branch its render path on what it got, and guards any command/pipeline that needs
        // an optional feature behind `is_enabled(...)` before recording it.
        [[nodiscard]] virtual const FeatureSet &enabled_features() const noexcept = 0;

        // Graded values (max ray recursion depth, VRS tile size, ...) for the enabled features.
        [[nodiscard]] virtual const FeatureProperties &feature_properties() const noexcept = 0;

        // Queue classes/lanes this device exposes after aliasing unsupported classes onto the nearest
        // native queue. Higher-level schedulers use this to decide how much CPU/GPU fan-out to attempt.
        [[nodiscard]] virtual span<const QueueInfo> queue_infos() const noexcept = 0;

        // API/vendor-specific extensions enabled on this device. Use this for capabilities that do not
        // deserve a core `Feature` yet, or that are intentionally backend-specific.
        [[nodiscard]] virtual span<const ExtensionId> enabled_extensions() const noexcept = 0;

        // Convenience over `enabled_features().has(feature)` — the guard at the point of use, e.g.
        // `if (device.is_enabled(Feature::RayTracingPipeline)) { ...trace... } else { ...raster... }`.
        [[nodiscard]] bool is_enabled(Feature feature) const noexcept;

        [[nodiscard]] bool is_extension_enabled(ExtensionId extension) const noexcept;

        // Returns an extension-specific typed interface when `extension` is enabled, otherwise nullptr.
        // This is the RHI-level escape hatch for API/vendor features that are intentionally outside the
        // core feature enum, without requiring high layers to include Vulkan/D3D12/Metal headers.
        [[nodiscard]] virtual RhiDeviceExtension *extension_interface(ExtensionId extension) noexcept = 0;

        // ── Resource creation / destruction ──
        [[nodiscard]] virtual RhiExpected<BufferHandle> create_buffer(const BufferDesc &desc) = 0;
        virtual void destroy_buffer(BufferHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<TextureHandle> create_texture(const TextureDesc &desc) = 0;
        virtual void destroy_texture(TextureHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<TextureViewHandle> create_texture_view(const TextureViewDesc &desc) = 0;
        virtual void destroy_texture_view(TextureViewHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<SamplerHandle> create_sampler(const SamplerDesc &desc) = 0;
        virtual void destroy_sampler(SamplerHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<ShaderModuleHandle> create_shader_module(const ShaderModuleDesc &desc) = 0;
        virtual void destroy_shader_module(ShaderModuleHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<BindGroupLayoutHandle> create_bind_group_layout(const BindGroupLayoutDesc &desc) = 0;
        virtual void destroy_bind_group_layout(BindGroupLayoutHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<BindGroupHandle> create_bind_group(const BindGroupDesc &desc) = 0;
        virtual void destroy_bind_group(BindGroupHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<PipelineLayoutHandle> create_pipeline_layout(const PipelineLayoutDesc &desc) = 0;
        virtual void destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<RenderPipelineHandle> create_render_pipeline(const RenderPipelineDesc &desc) = 0;
        virtual void destroy_render_pipeline(RenderPipelineHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<ComputePipelineHandle> create_compute_pipeline(const ComputePipelineDesc &desc) = 0;
        virtual void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<RayTracingPipelineHandle> create_ray_tracing_pipeline(
            const RayTracingPipelineDesc &desc) = 0;
        virtual void destroy_ray_tracing_pipeline(RayTracingPipelineHandle handle) noexcept = 0;
        [[nodiscard]] virtual RhiResult write_ray_tracing_shader_group_handles(
            RayTracingPipelineHandle pipeline,
            u32 first_group,
            u32 group_count,
            span<std::byte> dst) = 0;

        [[nodiscard]] virtual RhiExpected<AccelerationStructureBuildSizes> acceleration_structure_build_sizes(
            const AccelerationStructureBuildDesc &desc) const = 0;
        [[nodiscard]] virtual RhiExpected<AccelerationStructureHandle> create_acceleration_structure(
            const AccelerationStructureDesc &desc) = 0;
        virtual void destroy_acceleration_structure(AccelerationStructureHandle handle) noexcept = 0;

        // ── GPU virtual addresses ──
        // The device address of `buffer` (requires Feature::BufferDeviceAddress). The buffer must have
        // been created with BufferUsage carrying the address intent. Needed to feed pointers to shaders
        // (bindless-by-pointer) and to reference geometry/scratch by address in AS builds.
        [[nodiscard]] virtual RhiExpected<u64> buffer_device_address(BufferHandle buffer) const = 0;
        // The device address of an acceleration structure (requires Feature::AccelerationStructures) —
        // what a TLAS instance record stores to reference its BLAS.
        [[nodiscard]] virtual RhiExpected<u64> acceleration_structure_device_address(
            AccelerationStructureHandle handle) const = 0;

        // ── Data upload ──
        // Writes `data` into `buffer` at `offset`. For a HostUpload buffer this is a direct mapped
        // write; for a DeviceLocal buffer the backend stages and copies internally. Convenience for
        // the common "get bytes into a buffer" case without the caller hand-rolling staging.
        [[nodiscard]] virtual RhiResult write_buffer(BufferHandle buffer, u64 offset, span<const std::byte> data) = 0;

        // Maps a host-visible buffer (HostUpload / HostReadback memory) for direct CPU access, yielding
        // a writable byte span over the whole buffer; `unmap_buffer` releases it. This is the readback
        // path a DeviceLocal buffer has no equivalent for — copy GPU results into a HostReadback buffer,
        // then map it to read them (screenshots, query/statistics readback, GPU→CPU feedback). Mapping a
        // DeviceLocal buffer fails with InvalidArgument. Persistent mapping is allowed; the caller must
        // not read/write ranges racing with in-flight GPU work.
        [[nodiscard]] virtual RhiExpected<span<std::byte>> map_buffer(BufferHandle buffer) = 0;
        virtual void unmap_buffer(BufferHandle buffer) noexcept = 0;

        // ── Command recording / submission ──
        // Safe CPU parallelism rule: the device may be called concurrently to create independent
        // encoders/bundles; each returned encoder is single-threaded. Finished command buffers are
        // immutable handles and may be submitted from any thread, with per-queue serialization handled
        // by the backend.
        [[nodiscard]] RhiExpected<unique_ptr<CommandEncoder>> create_command_encoder();
        [[nodiscard]] virtual RhiExpected<unique_ptr<CommandEncoder>> create_command_encoder(
            const CommandEncoderDesc &desc) = 0;
        virtual void destroy_command_buffer(CommandBufferHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<unique_ptr<RenderBundleEncoder>> create_render_bundle_encoder(
            const RenderBundleDesc &desc) = 0;
        virtual void destroy_render_bundle(RenderBundleHandle handle) noexcept = 0;

        [[nodiscard]] RhiResult submit(span<const CommandBufferHandle> command_buffers);
        [[nodiscard]] virtual RhiResult submit(const SubmitDesc &desc) = 0;

        // ── Presentation ──
        [[nodiscard]] virtual RhiExpected<SurfaceHandle> create_surface(const SurfaceDesc &desc) = 0;
        virtual void destroy_surface(SurfaceHandle handle) noexcept = 0;

        [[nodiscard]] virtual RhiExpected<SwapchainHandle> create_swapchain(const SwapchainDesc &desc) = 0;
        virtual void destroy_swapchain(SwapchainHandle handle) noexcept = 0;

        // Acquires the next image to render into. A SurfaceLost/out-of-date result signals the
        // caller to recreate the swapchain.
        [[nodiscard]] virtual RhiExpected<SurfaceTexture> acquire_next_texture(SwapchainHandle swapchain) = 0;
        // Presents a previously acquired image. `suboptimal` in the result mirrors acquire — present
        // succeeded but the swapchain should be rebuilt soon.
        [[nodiscard]] virtual RhiExpected<bool> present(const PresentDesc &desc) = 0;

        // ── Synchronization ──
        [[nodiscard]] virtual RhiExpected<SemaphoreHandle> create_semaphore(const SemaphoreDesc &desc) = 0;
        virtual void destroy_semaphore(SemaphoreHandle handle) noexcept = 0;
        [[nodiscard]] virtual RhiExpected<u64> semaphore_value(SemaphoreHandle handle) const = 0;
        [[nodiscard]] virtual RhiResult wait_semaphore(SemaphoreHandle handle,
                                                       u64 value,
                                                       u64 timeout_ns = wait_forever) = 0;
        [[nodiscard]] virtual RhiResult signal_semaphore(SemaphoreHandle handle, u64 value) = 0;

        [[nodiscard]] virtual RhiExpected<FenceHandle> create_fence(const FenceDesc &desc) = 0;
        virtual void destroy_fence(FenceHandle handle) noexcept = 0;
        [[nodiscard]] virtual RhiResult wait_fences(span<const FenceHandle> fences,
                                                    bool wait_all = true,
                                                    u64 timeout_ns = wait_forever) = 0;
        [[nodiscard]] virtual RhiResult reset_fences(span<const FenceHandle> fences) = 0;

        // ── Queries ──
        // A pool of query slots the GPU writes results into (see :Queries). Recorded against by the
        // command encoder (reset/write_timestamp/begin/end/resolve); results are fetched either on the
        // GPU via CommandEncoder::resolve_query_set or on the host via get_query_set_results below.
        [[nodiscard]] virtual RhiExpected<QuerySetHandle> create_query_set(const QuerySetDesc &desc) = 0;
        virtual void destroy_query_set(QuerySetHandle handle) noexcept = 0;
        // Reads results for slots `[first, first+count)` into `dst`, `stride` bytes apart. With
        // QueryResultFlags::Wait it blocks until they're available; without it, InvalidArgument-free
        // partial reads follow the Partial/WithAvailability flags. The host counterpart of
        // resolve_query_set for when results are consumed CPU-side (per-pass GPU timing, occlusion).
        [[nodiscard]] virtual RhiResult get_query_set_results(QuerySetHandle query_set, u32 first, u32 count,
                                                              span<std::byte> dst, u64 stride,
                                                              QueryResultFlags flags = QueryResultFlags::Result64Bit) = 0;

        // Reset a query set's slots on the host without a command buffer (requires Feature::HostQueryReset).
        virtual void reset_query_set(QuerySetHandle query_set, u32 first, u32 count) noexcept = 0;

        // Blocks until the device is idle. The heavy hammer — use before teardown or a resource
        // reload, not per frame.
        virtual void wait_idle() noexcept = 0;

      protected:
        RhiDevice() = default;
    };

} // namespace SFT::RHI
