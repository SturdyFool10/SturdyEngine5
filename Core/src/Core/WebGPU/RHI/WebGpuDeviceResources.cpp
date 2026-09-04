#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

#include <algorithm>
#include <cstring>

namespace SFT::Core::WebGpu {

    /// Creates a buffer.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BufferHandle> WebGpuDevice::create_buffer(const rhi::BufferDesc &desc) {
        const auto has = [&desc](rhi::BufferUsage bit) {
            return (static_cast<u32>(desc.usage) & static_cast<u32>(bit)) != 0;
        };
        if (has(rhi::BufferUsage::ShaderBindingTable) || has(rhi::BufferUsage::AccelerationStructure) ||
            has(rhi::BufferUsage::AccelerationStructureInput) ||
            has(rhi::BufferUsage::AccelerationStructureScratch)) {
            return std::unexpected(unsupported_by_webgpu("Ray-tracing buffer usage"));
        }

        WGPUBufferUsage usage = to_wgpu(desc.usage);
        // WebGPU splits host visibility into two mapping usages that are mutually exclusive with
        // nearly everything else: MapWrite may only be joined by CopySrc, and MapRead only by
        // CopyDst. The RHI places no such restriction -- its HostUpload buffers are routinely also
        // uniform, storage, vertex or index buffers -- so requesting the mapping usage outright
        // fails creation for exactly the buffers that need it most.
        //
        // HostUpload therefore never asks for MapWrite. It gets CopyDst so both `write_buffer` and
        // the shadow-copy flush in `unmap_buffer` can reach it through the queue, and CopySrc so it
        // can still act as a staging source the way the other backends allow.
        //
        // HostReadback keeps the direct MapRead path when nothing else needs the buffer, which is
        // the common case; when something does, `map_buffer` routes through a companion buffer.
        switch (desc.memory) {
            case rhi::MemoryLocation::HostUpload:
                usage |= WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
                break;
            case rhi::MemoryLocation::HostReadback: {
                constexpr WGPUBufferUsage map_read_compatible = WGPUBufferUsage_CopyDst;
                if ((usage & ~map_read_compatible) == 0) {
                    usage |= WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
                } else {
                    usage |= WGPUBufferUsage_CopySrc;
                }
                break;
            }
            case rhi::MemoryLocation::DeviceLocal:
                break;
        }

        WGPUBufferDescriptor buffer_desc{};
        buffer_desc.label = wgpu_string(desc.label);
        // WebGPU requires buffer sizes to be a multiple of 4; rounding up is invisible to a caller
        // that only ever addresses the size it asked for.
        buffer_desc.size = (desc.size + 3u) & ~u64{3u};
        buffer_desc.usage = usage;
        buffer_desc.mappedAtCreation = 0;

        WGPUBuffer buffer = wgpuDeviceCreateBuffer(device_, &buffer_desc);
        if (buffer == nullptr) {
            return std::unexpected(webgpu_error("create_buffer"));
        }
        return buffers_.insert(BufferEntry{
            .buffer = buffer,
            .size = buffer_desc.size,
            .memory = desc.memory,
        });
    }

    /// Destroys a buffer.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_buffer(rhi::BufferHandle handle) noexcept {
        buffers_.erase(handle, [](BufferEntry &entry) {
            if (entry.readback != nullptr) {
                wgpuBufferDestroy(entry.readback);
                wgpuBufferRelease(entry.readback);
            }
            if (entry.buffer != nullptr) {
                wgpuBufferDestroy(entry.buffer);
                wgpuBufferRelease(entry.buffer);
            }
        });
    }

    /// Resolves a buffer handle to the Dawn buffer behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUBuffer WebGpuDevice::lookup_buffer(rhi::BufferHandle handle) noexcept {
        BufferEntry *entry = buffers_.find(handle);
        return entry != nullptr ? entry->buffer : nullptr;
    }

    /// Writes `data` into `buffer` at `offset` through the queue.
    ///
    /// @param buffer `buffer` value used by the operation.
    /// @param offset `offset` value used by the operation.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult WebGpuDevice::write_buffer(rhi::BufferHandle buffer, u64 offset,
                                              span<const std::byte> data) {
        BufferEntry *entry = buffers_.find(buffer);
        if (entry == nullptr) {
            return std::unexpected(webgpu_error("write_buffer", "unknown buffer handle"));
        }
        if (offset + data.size() > entry->size) {
            return std::unexpected(webgpu_error("write_buffer", "write runs past the end of the buffer"));
        }
        if (data.empty()) {
            return {};
        }
        // Keep the host shadow in step so a later map_buffer reports what this write put there
        // rather than the stale contents it flushed last time.
        if (!entry->shadow.empty()) {
            std::memcpy(entry->shadow.data() + offset, data.data(), data.size());
        }
        // wgpuQueueWriteBuffer requires a 4-byte-aligned size; the RHI does not, so an odd tail is
        // padded through a small staging copy rather than rejected.
        const usize aligned_size = (data.size() + 3u) & ~usize{3u};
        if (aligned_size == data.size()) {
            wgpuQueueWriteBuffer(queue_, entry->buffer, offset, data.data(), data.size());
            return {};
        }
        vector<std::byte> padded(aligned_size, std::byte{0});
        std::memcpy(padded.data(), data.data(), data.size());
        wgpuQueueWriteBuffer(queue_, entry->buffer, offset, padded.data(), padded.size());
        return {};
    }

    /// Maps a buffer for host access.
    ///
    /// @param buffer `buffer` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<span<std::byte>> WebGpuDevice::map_buffer(rhi::BufferHandle buffer) {
        BufferEntry *entry = buffers_.find(buffer);
        if (entry == nullptr) {
            return std::unexpected(webgpu_error("map_buffer", "unknown buffer handle"));
        }
        if (entry->memory == rhi::MemoryLocation::DeviceLocal) {
            return std::unexpected(webgpu_error(
                "map_buffer", "a DeviceLocal buffer is not host-visible in WebGPU"));
        }

        if (entry->memory == rhi::MemoryLocation::HostUpload) {
            // No HostUpload buffer carries MapWrite (see create_buffer), so writes land in a host
            // shadow that unmap_buffer pushes to the GPU. The shadow persists across map/unmap so a
            // caller that rewrites only part of the buffer keeps the rest of what it wrote before,
            // which is what mapping the real allocation would have given it.
            if (entry->shadow.size() != entry->size) {
                entry->shadow.resize(static_cast<usize>(entry->size), std::byte{0});
            }
            entry->shadow_mapped = true;
            return span<std::byte>{entry->shadow.data(), entry->shadow.size()};
        }

        WGPUBuffer target = entry->buffer;
        const bool needs_companion =
            (wgpuBufferGetUsage(entry->buffer) & WGPUBufferUsage_MapRead) == 0;
        if (needs_companion) {
            // The buffer's other usages barred MapRead, so the contents are staged through a
            // companion that carries nothing but MapRead|CopyDst. WebGPU has a single in-order
            // queue, so this copy is guaranteed to observe everything already submitted -- which is
            // exactly the state a caller reaching map_buffer has waited for.
            if (entry->readback == nullptr) {
                WGPUBufferDescriptor readback_desc{};
                readback_desc.label = wgpu_string("readback companion");
                readback_desc.size = entry->size;
                readback_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
                entry->readback = wgpuDeviceCreateBuffer(device_, &readback_desc);
                if (entry->readback == nullptr) {
                    return std::unexpected(
                        webgpu_error("map_buffer", "could not create the readback companion buffer"));
                }
            }

            WGPUCommandEncoderDescriptor encoder_desc{};
            encoder_desc.label = wgpu_string("readback copy");
            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encoder_desc);
            if (encoder == nullptr) {
                return std::unexpected(webgpu_error("map_buffer", "could not record the readback copy"));
            }
            wgpuCommandEncoderCopyBufferToBuffer(encoder, entry->buffer, 0, entry->readback, 0,
                                                 entry->size);
            WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuCommandEncoderRelease(encoder);
            if (commands == nullptr) {
                return std::unexpected(webgpu_error("map_buffer", "could not finish the readback copy"));
            }
            wgpuQueueSubmit(queue_, 1, &commands);
            wgpuCommandBufferRelease(commands);
            target = entry->readback;
        }

        // WebGPU mapping is asynchronous even for a buffer that is already resident, so the future
        // is resolved here to keep the RHI's synchronous contract.
        struct MapState {
            bool ok = false;
        } state;
        WGPUBufferMapCallbackInfo info{};
        info.mode = WGPUCallbackMode_WaitAnyOnly;
        info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *user_data, void *) {
            static_cast<MapState *>(user_data)->ok = status == WGPUMapAsyncStatus_Success;
        };
        info.userdata1 = &state;

        const WGPUFuture future =
            wgpuBufferMapAsync(target, WGPUMapMode_Read, 0, static_cast<usize>(entry->size), info);
        if (!wait_for(future) || !state.ok) {
            return std::unexpected(webgpu_error("map_buffer", "the map request did not complete"));
        }

        void *mapped = const_cast<void *>(
            wgpuBufferGetConstMappedRange(target, 0, static_cast<usize>(entry->size)));
        if (mapped == nullptr) {
            return std::unexpected(webgpu_error("map_buffer", "no mapped range was returned"));
        }
        entry->mapped = mapped;
        return span<std::byte>{static_cast<std::byte *>(mapped), static_cast<usize>(entry->size)};
    }

    /// Unmaps a previously mapped buffer.
    ///
    /// @param buffer `buffer` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::unmap_buffer(rhi::BufferHandle buffer) noexcept {
        BufferEntry *entry = buffers_.find(buffer);
        if (entry == nullptr) {
            return;
        }
        if (entry->shadow_mapped) {
            // The whole shadow goes across, because the RHI's map/unmap contract hands the caller a
            // plain span and never reports which parts of it were touched.
            wgpuQueueWriteBuffer(queue_, entry->buffer, 0, entry->shadow.data(), entry->shadow.size());
            entry->shadow_mapped = false;
            return;
        }
        if (entry->mapped == nullptr) {
            return;
        }
        wgpuBufferUnmap(entry->readback != nullptr ? entry->readback : entry->buffer);
        entry->mapped = nullptr;
    }

    /// Creates a texture.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::TextureHandle> WebGpuDevice::create_texture(const rhi::TextureDesc &desc) {
        const WGPUTextureFormat format = to_wgpu(desc.format);
        if (format == WGPUTextureFormat_Undefined) {
            return std::unexpected(webgpu_error("create_texture", "the requested format has no WebGPU equivalent"));
        }

        WGPUTextureDescriptor texture_desc{};
        texture_desc.label = wgpu_string(desc.label);
        texture_desc.usage = to_wgpu(desc.usage);
        // WebGPU allows storage bindings on a much shorter, API-fixed list of formats than Vulkan
        // does. Callers that ask for Storage across a whole class of render targets -- the renderer
        // requests it uniformly for every deferred colour target -- would otherwise fail creation
        // outright on the two-channel 16-bit G-buffer formats. Dropping the usage is the honest
        // translation: the texture is still exactly what was asked for everywhere else, and a
        // shader that really does bind it as a storage texture fails at bind-group creation with a
        // message naming that binding, rather than the target silently never existing.
        if ((texture_desc.usage & WGPUTextureUsage_StorageBinding) != 0 &&
            !format_supports_storage_binding(desc.format)) {
            texture_desc.usage &= ~static_cast<WGPUTextureUsage>(WGPUTextureUsage_StorageBinding);
        }
        texture_desc.dimension = to_wgpu(desc.dimension);
        texture_desc.size = WGPUExtent3D{
            .width = desc.extent.width,
            .height = std::max(desc.extent.height, 1u),
            .depthOrArrayLayers = std::max(desc.extent.depth_or_layers, 1u),
        };
        texture_desc.format = format;
        texture_desc.mipLevelCount = std::max(desc.mip_levels, 1u);
        texture_desc.sampleCount = static_cast<u32>(desc.samples);
        // WebGPU has no concurrent-queue sharing concept: there is one queue, so the RHI's
        // concurrent_queue_classes has nothing to express here.
        texture_desc.viewFormatCount = 0;
        texture_desc.viewFormats = nullptr;

        WGPUTexture texture = wgpuDeviceCreateTexture(device_, &texture_desc);
        if (texture == nullptr) {
            return std::unexpected(webgpu_error("create_texture"));
        }
        return textures_.insert(TextureEntry{
            .texture = texture,
            .format = desc.format,
            .extent = desc.extent,
            .mip_levels = texture_desc.mipLevelCount,
            .owned_by_surface = false,
        });
    }

    /// Destroys a texture.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_texture(rhi::TextureHandle handle) noexcept {
        textures_.erase(handle, [](TextureEntry &entry) {
            if (entry.texture == nullptr) {
                return;
            }
            // A surface-owned texture is released but never destroyed: the reference is this
            // backend's to drop, the storage behind it is the surface's to keep.
            if (!entry.owned_by_surface) {
                wgpuTextureDestroy(entry.texture);
            }
            wgpuTextureRelease(entry.texture);
        });
    }

    /// Resolves a texture handle to the Dawn texture behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUTexture WebGpuDevice::lookup_texture(rhi::TextureHandle handle) noexcept {
        TextureEntry *entry = textures_.find(handle);
        return entry != nullptr ? entry->texture : nullptr;
    }

    /// Resolves a texture handle to the format and extent it was created with.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param out Receives the layout when the handle is known; left untouched otherwise.
    ///
    /// @return Returns `true` when the handle named a live texture.
    /// @note This function does not throw exceptions.
    bool WebGpuDevice::lookup_texture_layout(rhi::TextureHandle handle, TextureLayout &out) noexcept {
        TextureEntry *entry = textures_.find(handle);
        if (entry == nullptr) {
            return false;
        }
        out = TextureLayout{.format = entry->format, .extent = entry->extent};
        return true;
    }

    /// Creates a texture view.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::TextureViewHandle> WebGpuDevice::create_texture_view(
        const rhi::TextureViewDesc &desc) {
        TextureEntry *entry = textures_.find(desc.texture);
        if (entry == nullptr) {
            return std::unexpected(webgpu_error("create_texture_view", "unknown texture handle"));
        }

        WGPUTextureViewDescriptor view_desc{};
        view_desc.label = wgpu_string(desc.label);
        // An Undefined format in the RHI means "same as the texture", which is also what WebGPU
        // reads Undefined as, so it passes through unchanged.
        view_desc.format = to_wgpu(desc.format == rhi::Format::Undefined ? entry->format : desc.format);
        view_desc.dimension = to_wgpu(desc.view_type);
        view_desc.baseMipLevel = desc.base_mip_level;
        view_desc.mipLevelCount = desc.mip_level_count == rhi::all_remaining
                                      ? entry->mip_levels - desc.base_mip_level
                                      : desc.mip_level_count;
        view_desc.baseArrayLayer = desc.base_array_layer;
        view_desc.arrayLayerCount =
            desc.array_layer_count == rhi::all_remaining
                ? std::max(entry->extent.depth_or_layers, 1u) - desc.base_array_layer
                : desc.array_layer_count;
        view_desc.aspect = WGPUTextureAspect_All;

        WGPUTextureView view = wgpuTextureCreateView(entry->texture, &view_desc);
        if (view == nullptr) {
            return std::unexpected(webgpu_error("create_texture_view"));
        }
        return texture_views_.insert(std::move(view));
    }

    /// Destroys a texture view.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_texture_view(rhi::TextureViewHandle handle) noexcept {
        texture_views_.erase(handle, [](WGPUTextureView &view) { wgpuTextureViewRelease(view); });
    }

    /// Resolves a texture view handle to the Dawn texture view behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUTextureView WebGpuDevice::lookup_texture_view(rhi::TextureViewHandle handle) noexcept {
        WGPUTextureView *view = texture_views_.find(handle);
        return view != nullptr ? *view : nullptr;
    }

    /// Creates a sampler.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SamplerHandle> WebGpuDevice::create_sampler(const rhi::SamplerDesc &desc) {
        WGPUSamplerDescriptor sampler_desc{};
        sampler_desc.label = wgpu_string(desc.label);
        sampler_desc.addressModeU = to_wgpu(desc.address_u);
        sampler_desc.addressModeV = to_wgpu(desc.address_v);
        sampler_desc.addressModeW = to_wgpu(desc.address_w);
        sampler_desc.magFilter = to_wgpu(desc.mag_filter);
        sampler_desc.minFilter = to_wgpu(desc.min_filter);
        sampler_desc.mipmapFilter = to_wgpu(desc.mipmap_mode);
        sampler_desc.lodMinClamp = desc.min_lod;
        sampler_desc.lodMaxClamp = desc.max_lod;
        sampler_desc.compare = desc.compare_enable ? to_wgpu(desc.compare) : WGPUCompareFunction_Undefined;
        // WebGPU takes anisotropy as a 16-bit count with 1 meaning off, where the RHI uses 0 for
        // off; and it has no LOD bias at all (Dawn rejects one), so mip_lod_bias is dropped.
        sampler_desc.maxAnisotropy =
            static_cast<u16>(std::clamp(static_cast<u32>(desc.max_anisotropy), 1u, 16u));

        WGPUSampler sampler = wgpuDeviceCreateSampler(device_, &sampler_desc);
        if (sampler == nullptr) {
            return std::unexpected(webgpu_error("create_sampler"));
        }
        return samplers_.insert(std::move(sampler));
    }

    /// Destroys a sampler.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_sampler(rhi::SamplerHandle handle) noexcept {
        samplers_.erase(handle, [](WGPUSampler &sampler) { wgpuSamplerRelease(sampler); });
    }

    /// Creates a shader module from WGSL source.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::ShaderModuleHandle> WebGpuDevice::create_shader_module(
        const rhi::ShaderModuleDesc &desc) {
        if (desc.language != rhi::ShaderLanguage::Wgsl) {
            return std::unexpected(webgpu_error(
                "create_shader_module",
                "WebGPU accepts only WGSL; compile this module with the Wgsl shader target"));
        }

        // The RHI carries shader code as bytes; for WGSL those bytes are source text, which Dawn
        // takes as a string view rather than a length-prefixed blob.
        WGPUShaderSourceWGSL wgsl{};
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = WGPUStringView{reinterpret_cast<const char *>(desc.code.data()), desc.code.size()};

        WGPUShaderModuleDescriptor module_desc{};
        module_desc.nextInChain = &wgsl.chain;
        module_desc.label = wgpu_string(desc.label);

        WGPUShaderModule module = wgpuDeviceCreateShaderModule(device_, &module_desc);
        if (module == nullptr) {
            return std::unexpected(webgpu_error("create_shader_module"));
        }
        return shader_modules_.insert(std::move(module));
    }

    /// Destroys a shader module.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_shader_module(rhi::ShaderModuleHandle handle) noexcept {
        shader_modules_.erase(handle, [](WGPUShaderModule &module) { wgpuShaderModuleRelease(module); });
    }

} // namespace SFT::Core::WebGpu
