#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

#include <algorithm>
#include <vector>

namespace SFT::Core::WebGpu {

    /// Creates a bind group layout.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BindGroupLayoutHandle> WebGpuDevice::create_bind_group_layout(
        const rhi::BindGroupLayoutDesc &desc) {
        std::vector<WGPUBindGroupLayoutEntry> entries;
        entries.reserve(desc.entries.size());

        for (const rhi::BindGroupLayoutEntry &entry : desc.entries) {
            if (entry.count != 1) {
                // WebGPU has no descriptor arrays: one binding number is one resource. A binding
                // array has to be expressed as separate bindings, which is a shader-authoring
                // change, not something this layer can paper over.
                return std::unexpected(unsupported_by_webgpu("A bind group layout entry with count != 1 (descriptor arrays)"));
            }

            WGPUBindGroupLayoutEntry out{};
            out.binding = entry.binding;
            out.visibility = to_wgpu(entry.visibility);
            if (out.visibility == WGPUShaderStage_None) {
                return std::unexpected(unsupported_by_webgpu(
                    "A bind group layout entry visible only to geometry, tessellation, mesh, or ray-tracing stages"));
            }

            switch (entry.type) {
                case rhi::BindingType::UniformBuffer:
                    out.buffer.type = WGPUBufferBindingType_Uniform;
                    out.buffer.hasDynamicOffset = entry.has_dynamic_offset ? 1u : 0u;
                    break;
                case rhi::BindingType::StorageBuffer:
                    out.buffer.type = WGPUBufferBindingType_Storage;
                    out.buffer.hasDynamicOffset = entry.has_dynamic_offset ? 1u : 0u;
                    break;
                case rhi::BindingType::ReadOnlyStorageBuffer:
                    out.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                    out.buffer.hasDynamicOffset = entry.has_dynamic_offset ? 1u : 0u;
                    break;
                case rhi::BindingType::SampledTexture:
                    // WGSL's texture_depth_2d and texture_2d<f32> are distinct types WebGPU
                    // validates strictly ("Texture class Sampled doesn't match the shader Depth");
                    // Vulkan/D3D12 make no such distinction at the descriptor level, so
                    // entry.sampled_texture_is_depth (see Renderer/ReflectionBinding.cpp) only
                    // matters here.
                    out.texture.sampleType = entry.sampled_texture_is_depth ? WGPUTextureSampleType_Depth
                                                                             : WGPUTextureSampleType_Float;
                    out.texture.viewDimension = WGPUTextureViewDimension_2D;
                    // Same story for multisampled vs non-multisampled: this was never set at all
                    // (always defaulting to false), so any MSAA texture bound as a plain sampled
                    // texture (e.g. reading an MSAA depth buffer for a custom resolve/reconstruction
                    // pass) failed WebGPU validation ("Texture class Sampled ... doesn't match the
                    // shader Sampled ... multi: true") the moment a real multisampled view was bound.
                    out.texture.multisampled = entry.sampled_texture_is_multisampled;
                    break;
                case rhi::BindingType::StorageTexture: {
                    // The format/access here used to be hardcoded to RGBA8Unorm/WriteOnly
                    // regardless of what the shader actually declared -- harmless on Vulkan/D3D12
                    // (neither needs a format at layout-creation time; format compatibility is
                    // enforced by the shader binary itself), but WGSL storage texture types are
                    // format-parameterized (`texture_storage_2d<rgba32float, write>`), so a
                    // mismatch here is a real WebGPU validation error the moment a pass uses any
                    // storage format/access other than the one that was hardcoded. entry.
                    // storage_format/storage_access now carry the shader's real declaration (see
                    // Renderer/ReflectionBinding.cpp).
                    const WGPUTextureFormat storage_format = to_wgpu(entry.storage_format);
                    if (storage_format == WGPUTextureFormat_Undefined) {
                        return std::unexpected(unsupported_by_webgpu(
                            "A storage-texture binding with no reflected (or no WebGPU-representable) format"));
                    }
                    out.storageTexture.format = storage_format;
                    switch (entry.storage_access) {
                        case rhi::StorageTextureAccess::WriteOnly:
                            out.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
                            break;
                        case rhi::StorageTextureAccess::ReadOnly:
                            out.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
                            break;
                        case rhi::StorageTextureAccess::ReadWrite:
                            out.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
                            break;
                    }
                    out.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
                    break;
                }
                case rhi::BindingType::Sampler:
                    // WGSL has two distinct sampler binding types, `sampler`/`sampler_comparison`;
                    // WebGPU rejects a mismatch against what the shader actually declared
                    // ("Comparison flag doesn't match the shader"), unlike Vulkan/D3D12 where a
                    // comparison sampler is just a regular sampler object with compareEnable set,
                    // unrelated to the descriptor/binding layout. entry.sampler_is_comparison
                    // carries the shader's real declaration (see Renderer/ReflectionBinding.cpp).
                    out.sampler.type = entry.sampler_is_comparison ? WGPUSamplerBindingType_Comparison
                                                                    : WGPUSamplerBindingType_Filtering;
                    break;
                case rhi::BindingType::CombinedImageSampler: {
                    // WebGPU has no combined image+sampler descriptor type: WGSL always takes a
                    // texture and a sampler as separate bindings. Slang's WGSL target already
                    // reflects this as two descriptor ranges (see
                    // Core/src/Core/Slang/ShaderImpl.cpp's parse_binding_range and
                    // Renderer/ReflectionBinding.cpp, which carry the second slot through as
                    // entry.paired_binding), so this one RHI entry compiles to two real
                    // WGPUBindGroupLayoutEntry slots below instead of being rejected.
                    if (entry.paired_binding == std::numeric_limits<u32>::max()) {
                        return std::unexpected(unsupported_by_webgpu(
                            "A combined image/sampler binding with no reflected paired sampler slot"));
                    }
                    out.texture.sampleType = WGPUTextureSampleType_Float;
                    out.texture.viewDimension = WGPUTextureViewDimension_2D;
                    entries.push_back(out);

                    WGPUBindGroupLayoutEntry sampler_out{};
                    sampler_out.binding = entry.paired_binding;
                    sampler_out.visibility = out.visibility;
                    sampler_out.sampler.type = WGPUSamplerBindingType_Filtering;
                    entries.push_back(sampler_out);
                    continue;
                }
                case rhi::BindingType::AccelerationStructure:
                    return std::unexpected(unsupported_by_webgpu("An acceleration-structure binding"));
                case rhi::BindingType::InputAttachment:
                    // Subpass input attachments are a Vulkan render-pass concept; WebGPU has no
                    // subpasses at all, so a render graph targeting it must use ordinary sampled
                    // textures between passes.
                    return std::unexpected(unsupported_by_webgpu("An input-attachment binding (subpasses)"));
            }
            entries.push_back(out);
        }

        WGPUBindGroupLayoutDescriptor layout_desc{};
        layout_desc.label = wgpu_string(desc.label);
        layout_desc.entryCount = entries.size();
        layout_desc.entries = entries.data();

        WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(device_, &layout_desc);
        if (layout == nullptr) {
            return std::unexpected(webgpu_error("create_bind_group_layout"));
        }
        return bind_group_layouts_.insert(BindGroupLayoutRecord{
            .layout = layout,
            .entries = std::vector<rhi::BindGroupLayoutEntry>(desc.entries.begin(), desc.entries.end()),
        });
    }

    /// Destroys a bind group layout.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_bind_group_layout(rhi::BindGroupLayoutHandle handle) noexcept {
        bind_group_layouts_.erase(handle,
                                  [](BindGroupLayoutRecord &record) { wgpuBindGroupLayoutRelease(record.layout); });
    }

    /// Creates a bind group.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BindGroupHandle> WebGpuDevice::create_bind_group(const rhi::BindGroupDesc &desc) {
        BindGroupLayoutRecord *layout = bind_group_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return std::unexpected(webgpu_error("create_bind_group", "unknown bind group layout handle"));
        }

        std::vector<WGPUBindGroupEntry> entries;
        entries.reserve(desc.entries.size());
        for (const rhi::BindGroupEntry &entry : desc.entries) {
            // A combined-image-sampler RHI entry carries both a texture view and a sampler under
            // one binding number (matching Vulkan/D3D12's contract, see RendererMaterial.cpp), but
            // was compiled into two real WGPUBindGroupLayoutEntry slots by create_bind_group_layout
            // above. A single WGPUBindGroupEntry may only ever populate one of buffer/sampler/
            // textureView -- setting both on one entry is invalid, so this has to become two
            // entries here too, at the same two binding numbers the layout used.
            const auto layout_entry = std::ranges::find(layout->entries, entry.binding, &rhi::BindGroupLayoutEntry::binding);
            const bool is_combined = layout_entry != layout->entries.end() &&
                                     layout_entry->type == rhi::BindingType::CombinedImageSampler;
            if (is_combined) {
                if (entry.texture_view.value == 0 || entry.sampler.value == 0) {
                    return std::unexpected(webgpu_error(
                        "create_bind_group", "a combined image/sampler binding requires both a texture view and a sampler"));
                }
                WGPUTextureView texture_view = lookup_texture_view(entry.texture_view);
                if (texture_view == nullptr) {
                    return std::unexpected(webgpu_error("create_bind_group", "unknown texture view handle"));
                }
                WGPUSampler *sampler = samplers_.find(entry.sampler);
                if (sampler == nullptr) {
                    return std::unexpected(webgpu_error("create_bind_group", "unknown sampler handle"));
                }

                WGPUBindGroupEntry texture_out{};
                texture_out.binding = entry.binding;
                texture_out.textureView = texture_view;
                entries.push_back(texture_out);

                WGPUBindGroupEntry sampler_out{};
                sampler_out.binding = layout_entry->paired_binding;
                sampler_out.sampler = *sampler;
                entries.push_back(sampler_out);
                continue;
            }

            WGPUBindGroupEntry out{};
            out.binding = entry.binding;
            if (entry.buffer.value != 0) {
                BufferEntry *buffer = buffers_.find(entry.buffer);
                if (buffer == nullptr) {
                    return std::unexpected(webgpu_error("create_bind_group", "unknown buffer handle"));
                }
                out.buffer = buffer->buffer;
                out.offset = entry.offset;
                // A zero size in the RHI means "the rest of the buffer", which WebGPU spells as
                // WGPU_WHOLE_SIZE rather than 0 (0 would bind nothing).
                out.size = entry.size != 0 ? entry.size : (buffer->size - entry.offset);
            }
            if (entry.texture_view.value != 0) {
                out.textureView = lookup_texture_view(entry.texture_view);
                if (out.textureView == nullptr) {
                    return std::unexpected(webgpu_error("create_bind_group", "unknown texture view handle"));
                }
            }
            if (entry.sampler.value != 0) {
                WGPUSampler *sampler = samplers_.find(entry.sampler);
                if (sampler == nullptr) {
                    return std::unexpected(webgpu_error("create_bind_group", "unknown sampler handle"));
                }
                out.sampler = *sampler;
            }
            if (entry.acceleration_structure.value != 0) {
                return std::unexpected(unsupported_by_webgpu("Binding an acceleration structure"));
            }
            entries.push_back(out);
        }

        WGPUBindGroupDescriptor group_desc{};
        group_desc.label = wgpu_string(desc.label);
        group_desc.layout = layout->layout;
        group_desc.entryCount = entries.size();
        group_desc.entries = entries.data();

        WGPUBindGroup group = wgpuDeviceCreateBindGroup(device_, &group_desc);
        if (group == nullptr) {
            return std::unexpected(webgpu_error("create_bind_group"));
        }
        return bind_groups_.insert(std::move(group));
    }

    /// Destroys a bind group.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_bind_group(rhi::BindGroupHandle handle) noexcept {
        bind_groups_.erase(handle, [](WGPUBindGroup &g) { wgpuBindGroupRelease(g); });
    }

    /// Resolves a bind group handle to the Dawn bind group behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUBindGroup WebGpuDevice::lookup_bind_group(rhi::BindGroupHandle handle) noexcept {
        WGPUBindGroup *group = bind_groups_.find(handle);
        return group != nullptr ? *group : nullptr;
    }

    /// Creates a pipeline layout.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::PipelineLayoutHandle> WebGpuDevice::create_pipeline_layout(
        const rhi::PipelineLayoutDesc &desc) {
        std::vector<WGPUBindGroupLayout> layouts;
        layouts.reserve(desc.bind_group_layouts.size() + 1);
        for (rhi::BindGroupLayoutHandle handle : desc.bind_group_layouts) {
            BindGroupLayoutRecord *layout = bind_group_layouts_.find(handle);
            if (layout == nullptr) {
                return std::unexpected(webgpu_error("create_pipeline_layout", "unknown bind group layout handle"));
            }
            layouts.push_back(layout->layout);
        }

        // WebGPU has no push constants; the shader library's SFT_EMULATE_PUSH_CONSTANTS path
        // declares the same block as a dynamic-offset uniform buffer at a reserved group instead,
        // and set_push_constants binds it (see WebGpuDevicePushConstants.cpp). The layout has to
        // agree, which means padding out any groups the caller left unused below the reserved index
        // -- WebGPU has no notion of a gap in a pipeline layout, so those become empty groups.
        if (!desc.push_constant_ranges.empty()) {
            if (layouts.size() > push_constant_group_index) {
                return std::unexpected(webgpu_error(
                    "create_pipeline_layout",
                    "a pipeline using push constants cannot also declare a bind group at the index "
                    "the WebGPU backend reserves to emulate them"));
            }
            WGPUBindGroupLayout empty = empty_bind_group_layout();
            WGPUBindGroupLayout push_constants = push_constant_bind_group_layout();
            if (empty == nullptr || push_constants == nullptr) {
                return std::unexpected(
                    webgpu_error("create_pipeline_layout", "could not create the push-constant group layout"));
            }
            layouts.resize(push_constant_group_index, empty);
            layouts.push_back(push_constants);
        }

        WGPUPipelineLayoutDescriptor layout_desc{};
        layout_desc.label = wgpu_string(desc.label);
        layout_desc.bindGroupLayoutCount = layouts.size();
        layout_desc.bindGroupLayouts = layouts.data();

        WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(device_, &layout_desc);
        if (layout == nullptr) {
            return std::unexpected(webgpu_error("create_pipeline_layout"));
        }
        return pipeline_layouts_.insert(std::move(layout));
    }

    /// Destroys a pipeline layout.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_pipeline_layout(rhi::PipelineLayoutHandle handle) noexcept {
        pipeline_layouts_.erase(handle, [](WGPUPipelineLayout &l) { wgpuPipelineLayoutRelease(l); });
    }

} // namespace SFT::Core::WebGpu
