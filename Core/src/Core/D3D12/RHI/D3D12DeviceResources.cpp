

#include <Core/D3D12/RHI/D3D12Device.hpp>

#pragma region Imports
#include <Core/D3D12/RHI/D3D12Convert.hpp>

#include <D3D12MemAlloc.h>

#include <algorithm>
#include <cstring>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {


        /// Performs the initial state for heap operation for `D3D12` using the supplied arguments.
        ///
        /// @param heap `heap` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_RESOURCE_STATES initial_state_for_heap(D3D12_HEAP_TYPE heap) noexcept {
            switch (heap) {
                case D3D12_HEAP_TYPE_UPLOAD:
                    return D3D12_RESOURCE_STATE_GENERIC_READ;
                case D3D12_HEAP_TYPE_READBACK:
                    return D3D12_RESOURCE_STATE_COPY_DEST;
                default:
                    return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        /// Performs the texture array layers operation for `D3D12` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 texture_array_layers(const rhi::TextureDesc &desc) noexcept {


            return desc.dimension == rhi::TextureDimension::Dim3D ? 1u : std::max(1u, desc.extent.depth_or_layers);
        }

        /// Resolves count into the concrete value used by the engine.
        ///
        /// @param requested `requested` value used by the operation.
        /// @param base `base` value used by the operation.
        /// @param total `total` value used by the operation.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 resolve_count(u32 requested, u32 base, u32 total) noexcept {
            if (requested == rhi::all_remaining) {
                return base < total ? total - base : 1u;
            }
            return std::max(1u, requested);
        }

    } // namespace


    /// Creates a buffer from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BufferHandle> D3D12Device::create_buffer(const rhi::BufferDesc &desc) {
        ZoneScopedN("D3D12Device::create_buffer");
        if (device_ == nullptr) {
            return device_not_ready<rhi::BufferHandle>("create_buffer");
        }
        if (desc.size == 0) {
            return invalid_argument("create_buffer: size must be non-zero.");
        }

        u64 size = desc.size;
        if (rhi::has_any(desc.usage, rhi::BufferUsage::Uniform)) {


            size = align_up(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        }

        D3D12_HEAP_TYPE heap_type = to_d3d12_heap_type(desc.memory);


        const bool use_gpu_upload_heap = heap_type == D3D12_HEAP_TYPE_DEFAULT && gpu_upload_heap_supported_ &&
                                          rhi::has_any(desc.usage, rhi::BufferUsage::TransferDst);
        if (use_gpu_upload_heap) {
            heap_type = D3D12_HEAP_TYPE_GPU_UPLOAD;
        }
        const CD3DX12_HEAP_PROPERTIES heap_properties(heap_type);


        D3D12_RESOURCE_FLAGS resource_flags = to_d3d12_resource_flags(desc.usage);
        if (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) {
            resource_flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        const CD3DX12_RESOURCE_DESC resource_desc = CD3DX12_RESOURCE_DESC::Buffer(size, resource_flags);

        BufferRecord record{};
        record.size = size;
        record.memory = desc.memory;
        record.usage = desc.usage;
        record.gpu_upload_heap = use_gpu_upload_heap;

        // D3D12MA doesn't need to be involved for GPU_UPLOAD-heap buffers: that heap type already
        // gets its own opportunistic-placement decision above (use_gpu_upload_heap), and D3D12MA's
        // suballocation strategy is for the ordinary DEFAULT/UPLOAD/READBACK heaps this device
        // otherwise sends straight to CreateCommittedResource, one dedicated allocation per resource
        // (see the D3D12MA integration note on d3d12ma_allocator_'s own declaration).
        if (d3d12ma_allocator_ != nullptr && !use_gpu_upload_heap) {
            const D3D12MA::ALLOCATION_DESC allocation_desc{
                .Flags = D3D12MA::ALLOCATION_FLAG_NONE,
                .HeapType = heap_type,
                .ExtraHeapFlags = D3D12_HEAP_FLAG_NONE,
                .CustomPool = nullptr,
                .pPrivateData = nullptr,
            };
            if (const HRESULT hr = d3d12ma_allocator_->CreateResource(
                    &allocation_desc,
                    &resource_desc,
                    initial_state_for_heap(heap_type),
                    nullptr,
                    &record.allocation,
                    IID_PPV_ARGS(&record.resource));
                FAILED(hr)) {
                return hresult_error(hr, "create_buffer (D3D12MA::CreateResource)");
            }
        } else if (const HRESULT hr = device_->CreateCommittedResource(
                       &heap_properties,
                       D3D12_HEAP_FLAG_NONE,
                       &resource_desc,
                       initial_state_for_heap(heap_type),
                       nullptr,
                       IID_PPV_ARGS(&record.resource));
                   FAILED(hr)) {
            return hresult_error(hr, "create_buffer (CreateCommittedResource)");
        }
        set_debug_name(record.resource.Get(), desc.label);
        record.gpu_address = record.resource->GetGPUVirtualAddress();
        return buffers_.insert(std::move(record));
    }

    /// Destroys the buffer identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_buffer(rhi::BufferHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_buffer");
        if (auto record = buffers_.extract(handle)) {
            if (record->mapped != nullptr && record->resource != nullptr) {
                record->resource->Unmap(0, nullptr);
            }
            if (record->allocation != nullptr) {
                record->allocation->Release();
            }
        }
    }

    /// Performs the buffer device address operation for `D3D12` using the supplied arguments.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<u64> D3D12Device::buffer_device_address(rhi::BufferHandle buffer) const {
        const BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return invalid_argument("buffer_device_address: unknown buffer handle.");
        }
        return static_cast<u64>(record->gpu_address);
    }

    /// Maps buffer for access.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<span<std::byte>> D3D12Device::map_buffer(rhi::BufferHandle buffer) {
        ZoneScopedN("D3D12Device::map_buffer");
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return invalid_argument("map_buffer: unknown buffer handle.");
        }
        if (record->memory == rhi::MemoryLocation::DeviceLocal) {
            return invalid_argument("map_buffer: buffer is DeviceLocal memory, which is not host-mappable.");
        }
        if (record->mapped == nullptr) {


            const CD3DX12_RANGE read_range(0, record->memory == rhi::MemoryLocation::HostReadback ? static_cast<SIZE_T>(record->size) : 0);
            if (const HRESULT hr = record->resource->Map(0, &read_range, &record->mapped); FAILED(hr)) {
                return hresult_error(hr, "map_buffer (Map)");
            }
        }
        return span<std::byte>(static_cast<std::byte *>(record->mapped), static_cast<usize>(record->size));
    }

    /// Unmaps buffer.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::unmap_buffer(rhi::BufferHandle buffer) noexcept {
        ZoneScopedN("D3D12Device::unmap_buffer");
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr || record->mapped == nullptr) {
            return;
        }


        record->resource->Unmap(0, nullptr);
        record->mapped = nullptr;
    }

    /// Writes buffer to the associated destination.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) {
        ZoneScopedN("D3D12Device::write_buffer");
        if (data.empty()) {
            return {};
        }
        BufferRecord *record = buffers_.find(buffer);
        if (record == nullptr) {
            return invalid_argument("write_buffer: unknown buffer handle.");
        }
        if (offset + data.size() > record->size) {
            return invalid_argument("write_buffer: the write extends past the end of the buffer.");
        }

        if (record->memory == rhi::MemoryLocation::DeviceLocal) {
            if (record->gpu_upload_heap) {
                return write_via_gpu_upload_heap(*record, offset, data);
            }
            return upload_via_staging(record->resource.Get(), offset, data);
        }

        auto mapped = map_buffer(buffer);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        std::memcpy(mapped->data() + offset, data.data(), data.size());
        return {};
    }

    /// Uploads via staging using the supplied arguments and current state.
    ///
    /// @param destination Destination value or resource.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::upload_via_staging(ID3D12Resource *destination, u64 offset, span<const std::byte> data) {
        ZoneScopedN("D3D12Device::upload_via_staging");
        if (device_ == nullptr || destination == nullptr) {
            return operation_failed("upload_via_staging: device resources are not ready.");
        }

        const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC staging_desc = CD3DX12_RESOURCE_DESC::Buffer(data.size());
        ComPtr<ID3D12Resource> staging;
        if (const HRESULT hr = device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &staging_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
            FAILED(hr)) {
            return hresult_error(hr, "upload_via_staging (CreateCommittedResource)");
        }
        set_debug_name(staging.Get(), "Sturdy staging upload");

        void *mapped = nullptr;
        const CD3DX12_RANGE no_read(0, 0);
        if (const HRESULT hr = staging->Map(0, &no_read, &mapped); FAILED(hr)) {
            return hresult_error(hr, "upload_via_staging (Map)");
        }
        std::memcpy(mapped, data.data(), data.size());
        staging->Unmap(0, nullptr);


        const rhi::QueueClass queue_class = copy_queue_ != nullptr ? rhi::QueueClass::Transfer
                                                                   : rhi::QueueClass::Graphics;
        auto command = acquire_command_buffer(rhi::QueueLane{queue_class, 0});
        if (!command) {
            return std::unexpected(command.error());
        }

        command->list->CopyBufferRegion(destination, offset, staging.Get(), 0, data.size());
        if (const HRESULT hr = command->list->Close(); FAILED(hr)) {
            return_command_buffer(std::move(*command));
            return hresult_error(hr, "upload_via_staging (Close)");
        }

        rhi::RhiResult executed = execute_and_wait(command->list.Get(), queue_class);


        return_command_buffer(std::move(*command));
        return executed;
    }

    /// Writes via GPU upload heap to the associated destination.
    ///
    /// @param record `record` value used by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::write_via_gpu_upload_heap(BufferRecord &record, u64 offset, span<const std::byte> data) {
        ZoneScopedN("D3D12Device::write_via_gpu_upload_heap");


        void *mapped = nullptr;
        const CD3DX12_RANGE no_read(0, 0);
        if (const HRESULT hr = record.resource->Map(0, &no_read, &mapped); FAILED(hr)) {
            return hresult_error(hr, "write_via_gpu_upload_heap (Map)");
        }
        std::memcpy(static_cast<std::byte *>(mapped) + offset, data.data(), data.size());
        record.resource->Unmap(0, nullptr);
        return {};
    }


    /// Creates a texture from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::TextureHandle> D3D12Device::create_texture(const rhi::TextureDesc &desc) {
        ZoneScopedN("D3D12Device::create_texture");
        if (device_ == nullptr) {
            return device_not_ready<rhi::TextureHandle>("create_texture");
        }
        const DXGI_FORMAT resource_format = to_dxgi_resource_format(desc.format, desc.usage);
        if (resource_format == DXGI_FORMAT_UNKNOWN) {
            return invalid_argument("create_texture: the requested format has no DXGI equivalent.");
        }

        const u32 layers = texture_array_layers(desc);
        D3D12_RESOURCE_DESC resource_desc{
            .Dimension = to_d3d12(desc.dimension),
            .Alignment = 0,
            .Width = desc.extent.width,
            .Height = desc.dimension == rhi::TextureDimension::Dim1D ? 1u : std::max(1u, desc.extent.height),
            .DepthOrArraySize = static_cast<UINT16>(desc.dimension == rhi::TextureDimension::Dim3D
                                                        ? std::max(1u, desc.extent.depth_or_layers)
                                                        : layers),
            .MipLevels = static_cast<UINT16>(std::max(1u, desc.mip_levels)),
            .Format = resource_format,
            .SampleDesc = {.Count = static_cast<UINT>(desc.samples), .Quality = 0},
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = to_d3d12_resource_flags(desc.usage, desc.format),
        };


        D3D12_CLEAR_VALUE clear_value{};
        const bool wants_clear_value =
            rhi::has_any(desc.usage, rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::DepthStencilAttachment);
        if (wants_clear_value) {
            clear_value.Format = to_dxgi_view_format(desc.format);
            if (rhi::format_is_depth_stencil(desc.format)) {
                clear_value.DepthStencil.Depth = 1.0f;
                clear_value.DepthStencil.Stencil = 0;
            }
        }

        const CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        TextureRecord record{};
        if (d3d12ma_allocator_ != nullptr) {
            const D3D12MA::ALLOCATION_DESC allocation_desc{
                .Flags = D3D12MA::ALLOCATION_FLAG_NONE,
                .HeapType = D3D12_HEAP_TYPE_DEFAULT,
                .ExtraHeapFlags = D3D12_HEAP_FLAG_NONE,
                .CustomPool = nullptr,
                .pPrivateData = nullptr,
            };
            if (const HRESULT hr = d3d12ma_allocator_->CreateResource(
                    &allocation_desc,
                    &resource_desc,
                    D3D12_RESOURCE_STATE_COMMON,
                    wants_clear_value ? &clear_value : nullptr,
                    &record.allocation,
                    IID_PPV_ARGS(&record.resource));
                FAILED(hr)) {
                return hresult_error(hr, "create_texture (D3D12MA::CreateResource)");
            }
        } else if (const HRESULT hr = device_->CreateCommittedResource(
                       &heap_properties,
                       D3D12_HEAP_FLAG_NONE,
                       &resource_desc,
                       D3D12_RESOURCE_STATE_COMMON,
                       wants_clear_value ? &clear_value : nullptr,
                       IID_PPV_ARGS(&record.resource));
                   FAILED(hr)) {
            return hresult_error(hr, "create_texture (CreateCommittedResource)");
        }
        set_debug_name(record.resource.Get(), desc.label);

        record.format = desc.format;
        record.resource_format = resource_format;
        record.dimension = desc.dimension;
        record.extent = desc.extent;
        record.mip_levels = std::max(1u, desc.mip_levels);
        record.array_layers = layers;
        record.samples = desc.samples;
        record.usage = desc.usage;
        if (!enhanced_barriers_) {
            record.legacy_states.assign(static_cast<usize>(record.mip_levels) * layers,
                                        D3D12_RESOURCE_STATE_COMMON);
        }


        (void)desc.concurrent_queue_classes;
        return textures_.insert(std::move(record));
    }

    /// Destroys the texture identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_texture(rhi::TextureHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_texture");
        if (auto record = textures_.extract(handle); record && record->allocation != nullptr) {
            record->allocation->Release();
        }
    }


    /// Creates a texture view from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::TextureViewHandle> D3D12Device::create_texture_view(const rhi::TextureViewDesc &desc) {
        ZoneScopedN("D3D12Device::create_texture_view");
        TextureRecord *texture = textures_.find(desc.texture);
        if (texture == nullptr) {
            return invalid_argument("create_texture_view: unknown texture handle.");
        }

        const rhi::Format view_format = desc.format == rhi::Format::Undefined ? texture->format : desc.format;
        const bool is_depth = rhi::format_is_depth_stencil(view_format);

        TextureViewRecord record{};
        record.texture = desc.texture;
        record.rhi_format = view_format;
        record.format = to_dxgi_view_format(view_format);
        record.base_mip_level = desc.base_mip_level;
        record.mip_level_count = resolve_count(desc.mip_level_count, desc.base_mip_level, texture->mip_levels);
        record.base_array_layer = desc.base_array_layer;
        record.array_layer_count = resolve_count(desc.array_layer_count, desc.base_array_layer, texture->array_layers);

        const bool is_array = record.array_layer_count > 1 ||
                              desc.view_type == rhi::TextureViewType::View2DArray ||
                              desc.view_type == rhi::TextureViewType::ViewCubeArray;
        const bool is_cube = desc.view_type == rhi::TextureViewType::ViewCube ||
                             desc.view_type == rhi::TextureViewType::ViewCubeArray;
        const bool is_3d = desc.view_type == rhi::TextureViewType::View3D;
        const bool multisampled = texture->samples != rhi::SampleCount::X1;


        struct Rollback {
            D3D12Device &device;
            TextureViewRecord &record;
            bool committed = false;
            ~Rollback() {
                if (!committed) {
                    device.release_view_descriptors(record);
                }
            }
        } rollback{*this, record};

        if (rhi::has_any(texture->usage, rhi::TextureUsage::Sampled)) {
            auto range = cpu_resource_descriptors_.allocate(1);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.srv = *range;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = is_depth ? to_dxgi_depth_srv_format(view_format) : record.format;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (is_cube) {
                if (desc.view_type == rhi::TextureViewType::ViewCubeArray) {
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                    srv.TextureCubeArray = {.MostDetailedMip = record.base_mip_level,
                                            .MipLevels = record.mip_level_count,
                                            .First2DArrayFace = record.base_array_layer,
                                            .NumCubes = record.array_layer_count / 6,
                                            .ResourceMinLODClamp = 0.0f};
                } else {
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    srv.TextureCube = {.MostDetailedMip = record.base_mip_level,
                                       .MipLevels = record.mip_level_count,
                                       .ResourceMinLODClamp = 0.0f};
                }
            } else if (is_3d) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srv.Texture3D = {.MostDetailedMip = record.base_mip_level,
                                 .MipLevels = record.mip_level_count,
                                 .ResourceMinLODClamp = 0.0f};
            } else if (multisampled) {
                srv.ViewDimension = is_array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
                srv.Texture2DMSArray = {.FirstArraySlice = record.base_array_layer,
                                        .ArraySize = record.array_layer_count};
            } else if (is_array) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv.Texture2DArray = {.MostDetailedMip = record.base_mip_level,
                                      .MipLevels = record.mip_level_count,
                                      .FirstArraySlice = record.base_array_layer,
                                      .ArraySize = record.array_layer_count,
                                      .PlaneSlice = 0,
                                      .ResourceMinLODClamp = 0.0f};
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D = {.MostDetailedMip = record.base_mip_level,
                                 .MipLevels = record.mip_level_count,
                                 .PlaneSlice = 0,
                                 .ResourceMinLODClamp = 0.0f};
            }
            device_->CreateShaderResourceView(texture->resource.Get(), &srv, cpu_resource_descriptors_.cpu_handle(record.srv, 0));
        }

        if (rhi::has_any(texture->usage, rhi::TextureUsage::Storage)) {
            auto range = cpu_resource_descriptors_.allocate(1);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.uav = *range;

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = record.format;
            if (is_3d) {
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uav.Texture3D = {.MipSlice = record.base_mip_level,
                                 .FirstWSlice = 0,
                                 .WSize = std::max(1u, texture->extent.depth_or_layers)};
            } else if (is_array) {
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav.Texture2DArray = {.MipSlice = record.base_mip_level,
                                      .FirstArraySlice = record.base_array_layer,
                                      .ArraySize = record.array_layer_count,
                                      .PlaneSlice = 0};
            } else {
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D = {.MipSlice = record.base_mip_level, .PlaneSlice = 0};
            }
            device_->CreateUnorderedAccessView(texture->resource.Get(), nullptr, &uav, cpu_resource_descriptors_.cpu_handle(record.uav, 0));
        }

        if (rhi::has_any(texture->usage, rhi::TextureUsage::ColorAttachment) && !is_depth) {
            auto range = cpu_rtv_descriptors_.allocate(1);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.rtv = *range;

            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = record.format;
            if (is_3d) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtv.Texture3D = {.MipSlice = record.base_mip_level,
                                 .FirstWSlice = 0,
                                 .WSize = std::max(1u, texture->extent.depth_or_layers)};
            } else if (multisampled) {
                rtv.ViewDimension = is_array ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
                rtv.Texture2DMSArray = {.FirstArraySlice = record.base_array_layer,
                                        .ArraySize = record.array_layer_count};
            } else if (is_array) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray = {.MipSlice = record.base_mip_level,
                                      .FirstArraySlice = record.base_array_layer,
                                      .ArraySize = record.array_layer_count,
                                      .PlaneSlice = 0};
            } else {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtv.Texture2D = {.MipSlice = record.base_mip_level, .PlaneSlice = 0};
            }
            device_->CreateRenderTargetView(texture->resource.Get(), &rtv, cpu_rtv_descriptors_.cpu_handle(record.rtv, 0));
        }

        if (is_depth && rhi::has_any(texture->usage, rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::TransientAttachment)) {
            auto range = cpu_dsv_descriptors_.allocate(1);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.dsv = *range;

            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = record.format;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            if (multisampled) {
                dsv.ViewDimension = is_array ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
                dsv.Texture2DMSArray = {.FirstArraySlice = record.base_array_layer,
                                        .ArraySize = record.array_layer_count};
            } else if (is_array) {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Texture2DArray = {.MipSlice = record.base_mip_level,
                                      .FirstArraySlice = record.base_array_layer,
                                      .ArraySize = record.array_layer_count};
            } else {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsv.Texture2D = {.MipSlice = record.base_mip_level};
            }
            device_->CreateDepthStencilView(texture->resource.Get(), &dsv, cpu_dsv_descriptors_.cpu_handle(record.dsv, 0));
        }

        rollback.committed = true;
        return texture_views_.insert(std::move(record));
    }

    /// Releases view descriptors using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::release_view_descriptors(TextureViewRecord &record) noexcept {
        cpu_resource_descriptors_.release(record.srv);
        cpu_resource_descriptors_.release(record.uav);
        cpu_rtv_descriptors_.release(record.rtv);
        cpu_dsv_descriptors_.release(record.dsv);
        record.srv = {};
        record.uav = {};
        record.rtv = {};
        record.dsv = {};
    }

    /// Destroys the texture view identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_texture_view(rhi::TextureViewHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_texture_view");
        if (auto record = texture_views_.extract(handle)) {
            release_view_descriptors(*record);
        }
    }


    /// Creates a sampler from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SamplerHandle> D3D12Device::create_sampler(const rhi::SamplerDesc &desc) {
        ZoneScopedN("D3D12Device::create_sampler");
        auto range = cpu_sampler_descriptors_.allocate(1);
        if (!range) {
            return std::unexpected(range.error());
        }

        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = to_d3d12_filter(desc);
        sampler.AddressU = to_d3d12(desc.address_u);
        sampler.AddressV = to_d3d12(desc.address_v);
        sampler.AddressW = to_d3d12(desc.address_w);
        sampler.MipLODBias = desc.mip_lod_bias;


        sampler.MaxAnisotropy = static_cast<UINT>(std::clamp(desc.max_anisotropy, 1.0f, 16.0f));
        sampler.ComparisonFunc = desc.compare_enable ? to_d3d12(desc.compare) : D3D12_COMPARISON_FUNC_NEVER;
        sampler.MinLOD = desc.min_lod;
        sampler.MaxLOD = desc.max_lod;
        fill_border_color(desc.border_color, sampler.BorderColor);

        device_->CreateSampler(&sampler, cpu_sampler_descriptors_.cpu_handle(*range, 0));
        return samplers_.insert(SamplerRecord{*range});
    }

    /// Destroys the sampler identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_sampler(rhi::SamplerHandle handle) noexcept {
        if (auto record = samplers_.extract(handle)) {
            cpu_sampler_descriptors_.release(record->descriptor);
        }
    }


    /// Creates a shader module from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::ShaderModuleHandle> D3D12Device::create_shader_module(const rhi::ShaderModuleDesc &desc) {
        ZoneScopedN("D3D12Device::create_shader_module");
        if (desc.language != rhi::ShaderLanguage::Dxil) {


            return unsupported("create_shader_module: the D3D12 backend requires ShaderLanguage::Dxil.");
        }
        if (desc.code.empty()) {
            return invalid_argument("create_shader_module: empty shader bytecode.");
        }
        ShaderModuleRecord record{};
        record.bytecode.assign(desc.code.begin(), desc.code.end());
        return shader_modules_.insert(std::move(record));
    }

    /// Destroys the shader module identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_shader_module(rhi::ShaderModuleHandle handle) noexcept {
        shader_modules_.erase(handle);
    }

} // namespace SFT::D3D12
