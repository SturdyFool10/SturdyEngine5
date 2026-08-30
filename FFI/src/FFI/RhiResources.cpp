/// C ABI implementation of the RHI resource surface: buffers, textures, samplers, shader
/// modules, bind groups, pipelines, and command encoding.
///
/// Resource handles (buffer/texture/...) are the RHI's own `Handle<Tag>` values passed through
/// unchanged — the device already owns and validates them, so there is nothing for this layer to
/// add. Command/render-pass/compute-pass encoders are different: `RhiDevice::create_command_encoder`
/// and friends hand back a `unique_ptr`, so this layer owns that storage and mints a token for it,
/// the same pattern `Gltf.cpp` uses for an imported scene.
///
/// Every description enum below (`SturdyFormat`, `SturdyVertexFormat`, `SturdyBlendFactor`, ...)
/// is declared in the same order as its `RHI::` counterpart on purpose, so the conversion is a
/// `static_cast` rather than a hand-written switch. Bitmask types (`SturdyBufferUsage`,
/// `SturdyShaderStage`, `SturdyPipelineStage`, ...) reuse the same bit positions for the same
/// reason. `sturdy_rhi_backend`'s `BackendType`/`DeviceType`/`QueueClass` are the exception —
/// those already have hand-written `translate_*` functions elsewhere because they are not new to
/// this pass — and this file keeps its own local copy of the queue-class translator rather than
/// sharing one across translation units, following `Rhi.cpp`/`Native.cpp`'s existing precedent.

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <Engine/Engine.hpp>
#include <RHI/RHI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::u64;
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    namespace RHI = SFT::RHI;

    // ─── Device resolution ──────────────────────────────────────────────────────

    [[nodiscard]] SturdyResult resolve_device(SturdyEngine engine, RHI::RhiDevice **out_device) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::RhiDevice *device = resolved_engine->rhi_device();
        if (device == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no active RHI device");
        }
        *out_device = device;
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult translate_rhi_error(const RHI::RhiError &error) noexcept {
        switch (error.code) {
        case RHI::RhiErrorCode::OutOfMemory:
            return set_error(STURDY_ERROR_OUT_OF_MEMORY, error.message);
        case RHI::RhiErrorCode::DeviceLost:
            return set_error(STURDY_ERROR_DEVICE_LOST, error.message);
        case RHI::RhiErrorCode::InvalidArgument:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, error.message);
        case RHI::RhiErrorCode::Unsupported:
        case RHI::RhiErrorCode::NotReady:
            return set_error(STURDY_ERROR_NOT_AVAILABLE, error.message);
        case RHI::RhiErrorCode::OperationFailed:
        case RHI::RhiErrorCode::SurfaceLost:
        case RHI::RhiErrorCode::FullScreenExclusiveLost:
        default:
            return set_error(STURDY_ERROR_INTERNAL, error.message);
        }
    }

    [[nodiscard]] bool translate_queue_class(SturdyQueueClass queue_class, RHI::QueueClass *out_queue) noexcept {
        switch (queue_class) {
        case STURDY_QUEUE_CLASS_GRAPHICS:
            *out_queue = RHI::QueueClass::Graphics;
            return true;
        case STURDY_QUEUE_CLASS_COMPUTE:
            *out_queue = RHI::QueueClass::Compute;
            return true;
        case STURDY_QUEUE_CLASS_TRANSFER:
            *out_queue = RHI::QueueClass::Transfer;
            return true;
        case STURDY_QUEUE_CLASS_SPARSE:
            *out_queue = RHI::QueueClass::Sparse;
            return true;
        case STURDY_QUEUE_CLASS_VIDEO_DECODE:
            *out_queue = RHI::QueueClass::VideoDecode;
            return true;
        case STURDY_QUEUE_CLASS_VIDEO_ENCODE:
            *out_queue = RHI::QueueClass::VideoEncode;
            return true;
        case STURDY_QUEUE_CLASS_FORCE_U32:
        default:
            return false;
        }
    }

    // ─── Owned encoder/pass storage ─────────────────────────────────────────────

    std::mutex g_encoder_mutex;
    std::map<u64, std::unique_ptr<RHI::CommandEncoder>> g_encoders;
    std::mutex g_render_pass_mutex;
    std::map<u64, std::unique_ptr<RHI::RenderPassEncoder>> g_render_passes;
    std::mutex g_compute_pass_mutex;
    std::map<u64, std::unique_ptr<RHI::ComputePassEncoder>> g_compute_passes;
    std::mutex g_render_bundle_encoder_mutex;
    std::map<u64, std::unique_ptr<RHI::RenderBundleEncoder>> g_render_bundle_encoders;

    [[nodiscard]] SturdyResult resolve_encoder(SturdyCommandEncoder encoder, RHI::CommandEncoder **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(encoder.token, HandleKind::CommandEncoder, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<RHI::CommandEncoder *>(pointer);
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult resolve_render_pass(SturdyRenderPassEncoder pass, RHI::RenderPassEncoder **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(pass.token, HandleKind::RenderPassEncoder, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<RHI::RenderPassEncoder *>(pointer);
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult resolve_compute_pass(SturdyComputePassEncoder pass, RHI::ComputePassEncoder **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(pass.token, HandleKind::ComputePassEncoder, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<RHI::ComputePassEncoder *>(pointer);
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult resolve_render_bundle_encoder(SturdyRenderBundleEncoder encoder,
                                                             RHI::RenderBundleEncoder **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(encoder.token, HandleKind::RenderBundleEncoder, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<RHI::RenderBundleEncoder *>(pointer);
        return STURDY_OK;
    }

    // ─── Small conversions ──────────────────────────────────────────────────────

    [[nodiscard]] RHI::ClearColor to_clear_color(const float color[4]) noexcept {
        return RHI::ClearColor{color[0], color[1], color[2], color[3]};
    }

    [[nodiscard]] RHI::Rect2D to_rect(const SturdyRect2D &rect) noexcept {
        return RHI::Rect2D{rect.x, rect.y, rect.width, rect.height};
    }

    [[nodiscard]] RHI::Viewport to_viewport(const SturdyViewport &viewport) noexcept {
        return RHI::Viewport{viewport.x, viewport.y, viewport.width, viewport.height,
                             viewport.min_depth, viewport.max_depth};
    }

    [[nodiscard]] RHI::TextureSubresourceRange to_subresource_range(const SturdyTextureSubresourceRange &range) noexcept {
        return RHI::TextureSubresourceRange{range.base_mip_level, range.mip_level_count,
                                            range.base_array_layer, range.array_layer_count};
    }

    [[nodiscard]] RHI::ShaderEntry to_shader_entry(const SturdyShaderEntry &entry) noexcept {
        RHI::ShaderEntry result{};
        result.module = RHI::ShaderModuleHandle{entry.module.id};
        result.entry_point = entry.entry_point != nullptr ? entry.entry_point : "main";
        result.stage = static_cast<RHI::ShaderStage>(entry.stage);
        return result;
    }

} // namespace

extern "C" {

// ─── Buffers ────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_buffer_desc_init(SturdyBufferDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdyBufferDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyBufferDesc));
        desc->memory = STURDY_MEMORY_LOCATION_DEVICE_LOCAL;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_buffer(SturdyEngine engine, const SturdyBufferDesc *desc,
                                                       SturdyBuffer *out_buffer) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_buffer == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::BufferDesc native{};
        native.size = desc->size;
        native.usage = static_cast<RHI::BufferUsage>(desc->usage);
        native.memory = static_cast<RHI::MemoryLocation>(desc->memory);
        native.label = desc->label;
        auto created = device->create_buffer(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_buffer->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_buffer(SturdyEngine engine, SturdyBuffer buffer) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_buffer(RHI::BufferHandle{buffer.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_write_buffer(SturdyEngine engine, SturdyBuffer buffer, uint64_t offset,
                                                      const void *data, size_t data_size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && data_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null when data_size is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto bytes = std::span<const std::byte>{static_cast<const std::byte *>(data), data_size};
        const auto result = device->write_buffer(RHI::BufferHandle{buffer.id}, offset, bytes);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_map_buffer(SturdyEngine engine, SturdyBuffer buffer, void **out_ptr,
                                                    size_t *out_size) {
    return guarded([&]() -> SturdyResult {
        if (out_ptr == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        auto mapped = device->map_buffer(RHI::BufferHandle{buffer.id});
        if (!mapped) {
            return translate_rhi_error(mapped.error());
        }
        *out_ptr = mapped->data();
        if (out_size != nullptr) {
            *out_size = mapped->size();
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_unmap_buffer(SturdyEngine engine, SturdyBuffer buffer) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->unmap_buffer(RHI::BufferHandle{buffer.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_buffer_device_address(SturdyEngine engine, SturdyBuffer buffer,
                                                               uint64_t *out_address) {
    return guarded([&]() -> SturdyResult {
        if (out_address == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto address = device->buffer_device_address(RHI::BufferHandle{buffer.id});
        if (!address) {
            return translate_rhi_error(address.error());
        }
        *out_address = *address;
        return STURDY_OK;
    });
}

// ─── Textures / views / samplers ────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_texture_desc_init(SturdyTextureDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdyTextureDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyTextureDesc));
        desc->dimension = STURDY_TEXTURE_DIMENSION_2D;
        desc->width = 1;
        desc->height = 1;
        desc->depth_or_layers = 1;
        desc->mip_levels = 1;
        desc->samples = 1;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_texture(SturdyEngine engine, const SturdyTextureDesc *desc,
                                                        SturdyTexture *out_texture) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_texture == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::TextureDesc native{};
        native.dimension = static_cast<RHI::TextureDimension>(desc->dimension);
        native.format = static_cast<RHI::Format>(desc->format);
        native.extent = RHI::Extent3D{desc->width, desc->height, desc->depth_or_layers};
        native.mip_levels = desc->mip_levels;
        native.samples = static_cast<RHI::SampleCount>(desc->samples);
        native.usage = static_cast<RHI::TextureUsage>(desc->usage);
        native.label = desc->label;
        auto created = device->create_texture(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_texture->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_texture(SturdyEngine engine, SturdyTexture texture) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_texture(RHI::TextureHandle{texture.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_texture_view(SturdyEngine engine, const SturdyTextureViewDesc *desc,
                                                             SturdyTextureView *out_view) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_view == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::TextureViewDesc native{};
        native.texture = RHI::TextureHandle{desc->texture.id};
        native.view_type = static_cast<RHI::TextureViewType>(desc->view_type);
        native.format = static_cast<RHI::Format>(desc->format);
        native.base_mip_level = desc->base_mip_level;
        native.mip_level_count = desc->mip_level_count;
        native.base_array_layer = desc->base_array_layer;
        native.array_layer_count = desc->array_layer_count;
        native.label = desc->label;
        auto created = device->create_texture_view(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_view->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_texture_view(SturdyEngine engine, SturdyTextureView view) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_texture_view(RHI::TextureViewHandle{view.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_sampler_desc_init(SturdySamplerDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdySamplerDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdySamplerDesc));
        desc->min_filter = STURDY_FILTER_LINEAR;
        desc->mag_filter = STURDY_FILTER_LINEAR;
        desc->mipmap_mode = STURDY_MIPMAP_MODE_LINEAR;
        desc->address_u = STURDY_ADDRESS_MODE_REPEAT;
        desc->address_v = STURDY_ADDRESS_MODE_REPEAT;
        desc->address_w = STURDY_ADDRESS_MODE_REPEAT;
        desc->max_lod = 1000.0f;
        desc->compare = STURDY_COMPARE_OP_NEVER;
        desc->border_color = STURDY_BORDER_COLOR_TRANSPARENT_BLACK;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_sampler(SturdyEngine engine, const SturdySamplerDesc *desc,
                                                        SturdySampler *out_sampler) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_sampler == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::SamplerDesc native{};
        native.min_filter = static_cast<RHI::Filter>(desc->min_filter);
        native.mag_filter = static_cast<RHI::Filter>(desc->mag_filter);
        native.mipmap_mode = static_cast<RHI::MipmapMode>(desc->mipmap_mode);
        native.address_u = static_cast<RHI::AddressMode>(desc->address_u);
        native.address_v = static_cast<RHI::AddressMode>(desc->address_v);
        native.address_w = static_cast<RHI::AddressMode>(desc->address_w);
        native.mip_lod_bias = desc->mip_lod_bias;
        native.min_lod = desc->min_lod;
        native.max_lod = desc->max_lod;
        native.max_anisotropy = desc->max_anisotropy;
        native.compare_enable = desc->compare_enable != STURDY_FALSE;
        native.compare = static_cast<RHI::CompareOp>(desc->compare);
        native.border_color = static_cast<RHI::BorderColor>(desc->border_color);
        native.label = desc->label;
        auto created = device->create_sampler(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_sampler->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_sampler(SturdyEngine engine, SturdySampler sampler) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_sampler(RHI::SamplerHandle{sampler.id});
        return STURDY_OK;
    });
}

// ─── Shader modules ─────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_shader_module(SturdyEngine engine, const SturdyShaderModuleDesc *desc,
                                                              SturdyShaderModule *out_module) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_module == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->code == nullptr && desc->code_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "code must not be null when code_size is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::ShaderModuleDesc native{};
        native.language = static_cast<RHI::ShaderLanguage>(desc->language);
        native.code = std::span<const std::byte>{static_cast<const std::byte *>(desc->code), desc->code_size};
        native.label = desc->label;
        auto created = device->create_shader_module(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_module->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_shader_module(SturdyEngine engine, SturdyShaderModule module) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_shader_module(RHI::ShaderModuleHandle{module.id});
        return STURDY_OK;
    });
}

// ─── Bind groups ────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_bind_group_layout(SturdyEngine engine,
                                                                  const SturdyBindGroupLayoutDesc *desc,
                                                                  SturdyBindGroupLayout *out_layout) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_layout == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->entries == nullptr && desc->entry_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "entries must not be null when entry_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::BindGroupLayoutEntry> entries;
        entries.reserve(desc->entry_count);
        for (uint32_t i = 0; i < desc->entry_count; ++i) {
            const SturdyBindGroupLayoutEntry &e = desc->entries[i];
            RHI::BindGroupLayoutEntry native{};
            native.binding = e.binding;
            native.shader_register = e.shader_register;
            native.type = static_cast<RHI::BindingType>(e.type);
            native.visibility = static_cast<RHI::ShaderStage>(e.visibility);
            native.count = e.count;
            native.has_dynamic_offset = e.has_dynamic_offset != STURDY_FALSE;
            native.flags = static_cast<RHI::BindingFlags>(e.flags);
            native.input_attachment_index = e.input_attachment_index;
            entries.push_back(native);
        }
        RHI::BindGroupLayoutDesc native{};
        native.entries = entries;
        native.label = desc->label;
        auto created = device->create_bind_group_layout(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_layout->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_bind_group_layout(SturdyEngine engine, SturdyBindGroupLayout layout) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_bind_group_layout(RHI::BindGroupLayoutHandle{layout.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_bind_group(SturdyEngine engine, const SturdyBindGroupDesc *desc,
                                                           SturdyBindGroup *out_group) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_group == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->entries == nullptr && desc->entry_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "entries must not be null when entry_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::BindGroupEntry> entries;
        entries.reserve(desc->entry_count);
        for (uint32_t i = 0; i < desc->entry_count; ++i) {
            const SturdyBindGroupEntry &e = desc->entries[i];
            RHI::BindGroupEntry native{};
            native.binding = e.binding;
            native.array_element = e.array_element;
            native.buffer = RHI::BufferHandle{e.buffer.id};
            native.offset = e.offset;
            native.size = e.size;
            native.structure_stride = e.structure_stride;
            native.texture_view = RHI::TextureViewHandle{e.texture_view.id};
            native.sampler = RHI::SamplerHandle{e.sampler.id};
            entries.push_back(native);
        }
        RHI::BindGroupDesc native{};
        native.layout = RHI::BindGroupLayoutHandle{desc->layout.id};
        native.entries = entries;
        native.variable_descriptor_count = desc->variable_descriptor_count;
        native.label = desc->label;
        auto created = device->create_bind_group(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_group->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_bind_group(SturdyEngine engine, SturdyBindGroup group) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_bind_group(RHI::BindGroupHandle{group.id});
        return STURDY_OK;
    });
}

// ─── Pipeline layouts ───────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_pipeline_layout(SturdyEngine engine,
                                                                const SturdyPipelineLayoutDesc *desc,
                                                                SturdyPipelineLayout *out_layout) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_layout == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::BindGroupLayoutHandle> layouts;
        layouts.reserve(desc->bind_group_layout_count);
        for (uint32_t i = 0; i < desc->bind_group_layout_count; ++i) {
            layouts.push_back(RHI::BindGroupLayoutHandle{desc->bind_group_layouts[i].id});
        }
        std::vector<RHI::PushConstantRange> ranges;
        ranges.reserve(desc->push_constant_range_count);
        for (uint32_t i = 0; i < desc->push_constant_range_count; ++i) {
            const SturdyPushConstantRange &r = desc->push_constant_ranges[i];
            RHI::PushConstantRange native{};
            native.stages = static_cast<RHI::ShaderStage>(r.stages);
            native.offset = r.offset;
            native.size = r.size;
            native.shader_register = r.shader_register;
            native.register_space = r.register_space;
            ranges.push_back(native);
        }
        RHI::PipelineLayoutDesc native{};
        native.bind_group_layouts = layouts;
        native.push_constant_ranges = ranges;
        native.label = desc->label;
        auto created = device->create_pipeline_layout(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_layout->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_pipeline_layout(SturdyEngine engine, SturdyPipelineLayout layout) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_pipeline_layout(RHI::PipelineLayoutHandle{layout.id});
        return STURDY_OK;
    });
}

// ─── Pipelines ──────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pipeline_desc_init(SturdyRenderPipelineDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdyRenderPipelineDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyRenderPipelineDesc));
        desc->topology = STURDY_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        desc->rasterization.polygon_mode = STURDY_POLYGON_MODE_FILL;
        desc->rasterization.cull_mode = STURDY_CULL_MODE_BACK;
        desc->rasterization.front_face = STURDY_FRONT_FACE_COUNTER_CLOCKWISE;
        desc->rasterization.line_width = 1.0f;
        desc->multisample.samples = 1;
        desc->multisample.sample_mask = ~0u;
        desc->depth_stencil.format = STURDY_FORMAT_UNDEFINED;
        desc->depth_stencil.depth_compare = STURDY_COMPARE_OP_LESS;
        desc->depth_stencil.stencil_read_mask = 0xFF;
        desc->depth_stencil.stencil_write_mask = 0xFF;
        desc->depth_stencil.stencil_front.fail_op = STURDY_STENCIL_OP_KEEP;
        desc->depth_stencil.stencil_front.depth_fail_op = STURDY_STENCIL_OP_KEEP;
        desc->depth_stencil.stencil_front.pass_op = STURDY_STENCIL_OP_KEEP;
        desc->depth_stencil.stencil_front.compare = STURDY_COMPARE_OP_ALWAYS;
        desc->depth_stencil.stencil_back = desc->depth_stencil.stencil_front;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_render_pipeline(SturdyEngine engine,
                                                                const SturdyRenderPipelineDesc *desc,
                                                                SturdyRenderPipeline *out_pipeline) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_pipeline == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // Attribute storage must outlive the vertex_buffers span built below.
        std::vector<std::vector<RHI::VertexAttribute>> attribute_storage;
        attribute_storage.reserve(desc->vertex_buffer_count);
        std::vector<RHI::VertexBufferLayout> vertex_buffers;
        vertex_buffers.reserve(desc->vertex_buffer_count);
        for (uint32_t i = 0; i < desc->vertex_buffer_count; ++i) {
            const SturdyVertexBufferLayout &layout = desc->vertex_buffers[i];
            std::vector<RHI::VertexAttribute> attributes;
            attributes.reserve(layout.attribute_count);
            for (uint32_t j = 0; j < layout.attribute_count; ++j) {
                const SturdyVertexAttribute &a = layout.attributes[j];
                RHI::VertexAttribute native{};
                native.format = static_cast<RHI::VertexFormat>(a.format);
                native.offset = a.offset;
                native.shader_location = a.shader_location;
                native.semantic_name = a.semantic_name != nullptr ? a.semantic_name : "TEXCOORD";
                native.semantic_index = a.semantic_index;
                attributes.push_back(native);
            }
            attribute_storage.push_back(std::move(attributes));
            RHI::VertexBufferLayout native_layout{};
            native_layout.stride = layout.stride;
            native_layout.step_mode = static_cast<RHI::VertexStepMode>(layout.step_mode);
            native_layout.attributes = attribute_storage.back();
            vertex_buffers.push_back(native_layout);
        }

        std::vector<RHI::ColorTargetState> color_targets;
        color_targets.reserve(desc->color_target_count);
        for (uint32_t i = 0; i < desc->color_target_count; ++i) {
            const SturdyColorTargetState &c = desc->color_targets[i];
            RHI::ColorTargetState native{};
            native.format = static_cast<RHI::Format>(c.format);
            native.blend_enable = c.blend_enable != STURDY_FALSE;
            native.color.src_factor = static_cast<RHI::BlendFactor>(c.color_src_factor);
            native.color.dst_factor = static_cast<RHI::BlendFactor>(c.color_dst_factor);
            native.color.op = static_cast<RHI::BlendOp>(c.color_op);
            native.alpha.src_factor = static_cast<RHI::BlendFactor>(c.alpha_src_factor);
            native.alpha.dst_factor = static_cast<RHI::BlendFactor>(c.alpha_dst_factor);
            native.alpha.op = static_cast<RHI::BlendOp>(c.alpha_op);
            native.write_mask = static_cast<RHI::ColorWriteMask>(c.write_mask);
            color_targets.push_back(native);
        }

        RHI::RenderPipelineDesc native{};
        native.layout = RHI::PipelineLayoutHandle{desc->layout.id};
        native.vertex = to_shader_entry(desc->vertex);
        native.fragment = to_shader_entry(desc->fragment);
        native.task = to_shader_entry(desc->task);
        native.mesh = to_shader_entry(desc->mesh);
        native.vertex_buffers = vertex_buffers;
        native.topology = static_cast<RHI::PrimitiveTopology>(desc->topology);

        const SturdyRasterizationState &r = desc->rasterization;
        native.rasterization.polygon_mode = static_cast<RHI::PolygonMode>(r.polygon_mode);
        native.rasterization.cull_mode = static_cast<RHI::CullMode>(r.cull_mode);
        native.rasterization.front_face = static_cast<RHI::FrontFace>(r.front_face);
        native.rasterization.depth_clamp_enable = r.depth_clamp_enable != STURDY_FALSE;
        native.rasterization.depth_bias_constant = r.depth_bias_constant;
        native.rasterization.depth_bias_slope_scale = r.depth_bias_slope_scale;
        native.rasterization.depth_bias_clamp = r.depth_bias_clamp;
        native.rasterization.line_width = r.line_width;

        const SturdyMultisampleState &m = desc->multisample;
        native.multisample.samples = static_cast<RHI::SampleCount>(m.samples);
        native.multisample.sample_mask = m.sample_mask;
        native.multisample.alpha_to_coverage_enable = m.alpha_to_coverage_enable != STURDY_FALSE;

        const SturdyDepthStencilState &d = desc->depth_stencil;
        native.depth_stencil.format = static_cast<RHI::Format>(d.format);
        native.depth_stencil.depth_test_enable = d.depth_test_enable != STURDY_FALSE;
        native.depth_stencil.depth_write_enable = d.depth_write_enable != STURDY_FALSE;
        native.depth_stencil.depth_compare = static_cast<RHI::CompareOp>(d.depth_compare);
        native.depth_stencil.stencil_test_enable = d.stencil_test_enable != STURDY_FALSE;
        native.depth_stencil.stencil_front.fail_op = static_cast<RHI::StencilOp>(d.stencil_front.fail_op);
        native.depth_stencil.stencil_front.depth_fail_op = static_cast<RHI::StencilOp>(d.stencil_front.depth_fail_op);
        native.depth_stencil.stencil_front.pass_op = static_cast<RHI::StencilOp>(d.stencil_front.pass_op);
        native.depth_stencil.stencil_front.compare = static_cast<RHI::CompareOp>(d.stencil_front.compare);
        native.depth_stencil.stencil_back.fail_op = static_cast<RHI::StencilOp>(d.stencil_back.fail_op);
        native.depth_stencil.stencil_back.depth_fail_op = static_cast<RHI::StencilOp>(d.stencil_back.depth_fail_op);
        native.depth_stencil.stencil_back.pass_op = static_cast<RHI::StencilOp>(d.stencil_back.pass_op);
        native.depth_stencil.stencil_back.compare = static_cast<RHI::CompareOp>(d.stencil_back.compare);
        native.depth_stencil.stencil_read_mask = d.stencil_read_mask;
        native.depth_stencil.stencil_write_mask = d.stencil_write_mask;

        native.color_targets = color_targets;
        native.label = desc->label;

        auto created = device->create_render_pipeline(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_pipeline->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_render_pipeline(SturdyEngine engine, SturdyRenderPipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_render_pipeline(RHI::RenderPipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_compute_pipeline(SturdyEngine engine,
                                                                 const SturdyComputePipelineDesc *desc,
                                                                 SturdyComputePipeline *out_pipeline) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_pipeline == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::ComputePipelineDesc native{};
        native.layout = RHI::PipelineLayoutHandle{desc->layout.id};
        native.compute = to_shader_entry(desc->compute);
        native.label = desc->label;
        auto created = device->create_compute_pipeline(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_pipeline->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_compute_pipeline(SturdyEngine engine, SturdyComputePipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_compute_pipeline(RHI::ComputePipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

// ─── Command encoder lifecycle ──────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_desc_init(SturdyCommandEncoderDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdyCommandEncoderDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyCommandEncoderDesc));
        desc->queue_class = STURDY_QUEUE_CLASS_GRAPHICS;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_command_encoder(SturdyEngine engine,
                                                                const SturdyCommandEncoderDesc *desc,
                                                                SturdyCommandEncoder *out_encoder) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_encoder == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::QueueClass queue_class{};
        if (!translate_queue_class(desc->queue_class, &queue_class)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized queue class");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::CommandEncoderDesc native{};
        native.queue = RHI::QueueLane{queue_class, desc->queue_lane_index};
        native.label = desc->label;
        auto created = device->create_command_encoder(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        auto owned = std::move(*created);
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::CommandEncoder, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_encoder_mutex};
            g_encoders.emplace(token, std::move(owned));
        }
        out_encoder->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_release(SturdyCommandEncoder encoder) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(encoder.token);
        const std::lock_guard<std::mutex> lock{g_encoder_mutex};
        g_encoders.erase(encoder.token);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_finish(SturdyCommandEncoder encoder,
                                                                SturdyCommandBuffer *out_buffer) {
    return guarded([&]() -> SturdyResult {
        if (out_buffer == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        auto finished = pointer->finish();

        revoke_handle(encoder.token);
        {
            const std::lock_guard<std::mutex> lock{g_encoder_mutex};
            g_encoders.erase(encoder.token);
        }

        if (!finished) {
            return translate_rhi_error(finished.error());
        }
        out_buffer->id = finished->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_command_buffer(SturdyEngine engine, SturdyCommandBuffer buffer) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_command_buffer(RHI::CommandBufferHandle{buffer.id});
        return STURDY_OK;
    });
}

// ─── Command encoder: transfer / clear / barrier ops ────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_buffer_to_buffer(SturdyCommandEncoder encoder,
                                                                               SturdyBuffer src, SturdyBuffer dst,
                                                                               const SturdyBufferCopy *region) {
    return guarded([&]() -> SturdyResult {
        if (region == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "region must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->copy_buffer_to_buffer(RHI::BufferHandle{src.id}, RHI::BufferHandle{dst.id},
                                       RHI::BufferCopy{region->src_offset, region->dst_offset, region->size});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_buffer_to_texture(
    SturdyCommandEncoder encoder, SturdyBuffer src, SturdyTexture dst, const SturdyBufferTextureCopy *region) {
    return guarded([&]() -> SturdyResult {
        if (region == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "region must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::BufferTextureCopy native{};
        native.buffer_offset = region->buffer_offset;
        native.buffer_row_length = region->buffer_row_length;
        native.buffer_image_height = region->buffer_image_height;
        native.mip_level = region->mip_level;
        native.base_array_layer = region->base_array_layer;
        native.array_layer_count = region->array_layer_count;
        native.texture_offset = RHI::Offset3D{region->texture_x, region->texture_y, region->texture_z};
        native.texture_extent = RHI::Extent3D{region->extent_width, region->extent_height, region->extent_depth_or_layers};
        pointer->copy_buffer_to_texture(RHI::BufferHandle{src.id}, RHI::TextureHandle{dst.id}, native);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_texture_to_buffer(
    SturdyCommandEncoder encoder, SturdyTexture src, SturdyBuffer dst, const SturdyBufferTextureCopy *region) {
    return guarded([&]() -> SturdyResult {
        if (region == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "region must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::BufferTextureCopy native{};
        native.buffer_offset = region->buffer_offset;
        native.buffer_row_length = region->buffer_row_length;
        native.buffer_image_height = region->buffer_image_height;
        native.mip_level = region->mip_level;
        native.base_array_layer = region->base_array_layer;
        native.array_layer_count = region->array_layer_count;
        native.texture_offset = RHI::Offset3D{region->texture_x, region->texture_y, region->texture_z};
        native.texture_extent = RHI::Extent3D{region->extent_width, region->extent_height, region->extent_depth_or_layers};
        pointer->copy_texture_to_buffer(RHI::TextureHandle{src.id}, RHI::BufferHandle{dst.id}, native);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_texture_to_texture(SturdyCommandEncoder encoder,
                                                                                 SturdyTexture src, SturdyTexture dst,
                                                                                 const SturdyTextureCopy *region) {
    return guarded([&]() -> SturdyResult {
        if (region == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "region must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::TextureCopy native{};
        native.src_subresource = RHI::TextureSubresourceLayers{region->src_mip_level, region->src_base_array_layer,
                                                                region->src_array_layer_count};
        native.src_offset = RHI::Offset3D{region->src_x, region->src_y, region->src_z};
        native.dst_subresource = RHI::TextureSubresourceLayers{region->dst_mip_level, region->dst_base_array_layer,
                                                                region->dst_array_layer_count};
        native.dst_offset = RHI::Offset3D{region->dst_x, region->dst_y, region->dst_z};
        native.extent = RHI::Extent3D{region->extent_width, region->extent_height, region->extent_depth_or_layers};
        pointer->copy_texture_to_texture(RHI::TextureHandle{src.id}, RHI::TextureHandle{dst.id}, native);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_fill_buffer(SturdyCommandEncoder encoder, SturdyBuffer buffer,
                                                                     uint64_t offset, uint64_t size, uint32_t value) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->fill_buffer(RHI::BufferHandle{buffer.id}, offset, size, value);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_update_buffer(SturdyCommandEncoder encoder,
                                                                       SturdyBuffer buffer, uint64_t offset,
                                                                       const void *data, size_t data_size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && data_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null when data_size is nonzero");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->update_buffer(RHI::BufferHandle{buffer.id}, offset,
                               std::span<const std::byte>{static_cast<const std::byte *>(data), data_size});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_clear_color_texture(SturdyCommandEncoder encoder,
                                                                             SturdyTexture texture,
                                                                             const float color[4],
                                                                             const SturdyTextureSubresourceRange *range) {
    return guarded([&]() -> SturdyResult {
        if (color == nullptr || range == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "color and range must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->clear_color_texture(RHI::TextureHandle{texture.id}, to_clear_color(color),
                                     to_subresource_range(*range));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_clear_depth_stencil_texture(
    SturdyCommandEncoder encoder, SturdyTexture texture, float depth, uint32_t stencil,
    const SturdyTextureSubresourceRange *range) {
    return guarded([&]() -> SturdyResult {
        if (range == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "range must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->clear_depth_stencil_texture(RHI::TextureHandle{texture.id}, RHI::ClearDepthStencil{depth, stencil},
                                             to_subresource_range(*range));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_barrier(
    SturdyCommandEncoder encoder, uint32_t global_barrier_count, const SturdyGlobalBarrier *global_barriers,
    uint32_t buffer_barrier_count, const SturdyBufferBarrier *buffer_barriers, uint32_t texture_barrier_count,
    const SturdyTextureBarrier *texture_barriers) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::GlobalBarrier> globals;
        globals.reserve(global_barrier_count);
        for (uint32_t i = 0; i < global_barrier_count; ++i) {
            const SturdyGlobalBarrier &g = global_barriers[i];
            globals.push_back(RHI::GlobalBarrier{
                static_cast<RHI::PipelineStage>(g.src_stage), static_cast<RHI::AccessFlags>(g.src_access),
                static_cast<RHI::PipelineStage>(g.dst_stage), static_cast<RHI::AccessFlags>(g.dst_access)});
        }

        std::vector<RHI::BufferBarrier> buffers;
        buffers.reserve(buffer_barrier_count);
        for (uint32_t i = 0; i < buffer_barrier_count; ++i) {
            const SturdyBufferBarrier &b = buffer_barriers[i];
            RHI::BufferBarrier native{};
            native.buffer = RHI::BufferHandle{b.buffer.id};
            native.src_stage = static_cast<RHI::PipelineStage>(b.src_stage);
            native.src_access = static_cast<RHI::AccessFlags>(b.src_access);
            native.dst_stage = static_cast<RHI::PipelineStage>(b.dst_stage);
            native.dst_access = static_cast<RHI::AccessFlags>(b.dst_access);
            native.offset = b.offset;
            native.size = b.size;
            buffers.push_back(native);
        }

        std::vector<RHI::TextureBarrier> textures;
        textures.reserve(texture_barrier_count);
        for (uint32_t i = 0; i < texture_barrier_count; ++i) {
            const SturdyTextureBarrier &t = texture_barriers[i];
            RHI::TextureBarrier native{};
            native.texture = RHI::TextureHandle{t.texture.id};
            native.src_stage = static_cast<RHI::PipelineStage>(t.src_stage);
            native.src_access = static_cast<RHI::AccessFlags>(t.src_access);
            native.dst_stage = static_cast<RHI::PipelineStage>(t.dst_stage);
            native.dst_access = static_cast<RHI::AccessFlags>(t.dst_access);
            native.old_layout = static_cast<RHI::TextureLayout>(t.old_layout);
            native.new_layout = static_cast<RHI::TextureLayout>(t.new_layout);
            native.range = to_subresource_range(t.range);
            textures.push_back(native);
        }

        pointer->barrier(globals, buffers, textures);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_push_debug_group(SturdyCommandEncoder encoder,
                                                                          const char *label) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->push_debug_group(label != nullptr ? label : "");
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_pop_debug_group(SturdyCommandEncoder encoder) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->pop_debug_group();
        return STURDY_OK;
    });
}

// ─── Render pass ────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_desc_init(SturdyRenderPassDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *desc = SturdyRenderPassDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyRenderPassDesc));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_render_pass(SturdyCommandEncoder encoder,
                                                                          const SturdyRenderPassDesc *desc,
                                                                          SturdyRenderPassEncoder *out_pass) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_pass == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->color_attachments == nullptr && desc->color_attachment_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "color_attachments must not be null when color_attachment_count is nonzero");
        }
        RHI::CommandEncoder *encoder_pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &encoder_pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::ColorAttachment> color_attachments;
        color_attachments.reserve(desc->color_attachment_count);
        for (uint32_t i = 0; i < desc->color_attachment_count; ++i) {
            const SturdyColorAttachment &c = desc->color_attachments[i];
            RHI::ColorAttachment native{};
            native.view = RHI::TextureViewHandle{c.view.id};
            native.resolve_view = RHI::TextureViewHandle{c.resolve_view.id};
            native.load_op = static_cast<RHI::LoadOp>(c.load_op);
            native.store_op = static_cast<RHI::StoreOp>(c.store_op);
            native.clear_color = to_clear_color(c.clear_color);
            color_attachments.push_back(native);
        }

        RHI::RenderPassDesc native{};
        native.color_attachments = color_attachments;
        if (desc->depth_stencil.has_view != STURDY_FALSE) {
            const SturdyDepthStencilAttachment &d = desc->depth_stencil;
            native.depth_stencil.view = RHI::TextureViewHandle{d.view.id};
            native.depth_stencil.resolve_view = RHI::TextureViewHandle{d.resolve_view.id};
            native.depth_stencil.depth_load_op = static_cast<RHI::LoadOp>(d.depth_load_op);
            native.depth_stencil.depth_store_op = static_cast<RHI::StoreOp>(d.depth_store_op);
            native.depth_stencil.stencil_load_op = static_cast<RHI::LoadOp>(d.stencil_load_op);
            native.depth_stencil.stencil_store_op = static_cast<RHI::StoreOp>(d.stencil_store_op);
            native.depth_stencil.clear_value = RHI::ClearDepthStencil{d.clear_depth, d.clear_stencil};
        }
        native.render_area = to_rect(desc->render_area);
        native.allow_bundles = desc->allow_bundles != STURDY_FALSE;
        native.label = desc->label;

        auto begun = encoder_pointer->begin_render_pass(native);
        if (!begun) {
            return translate_rhi_error(begun.error());
        }
        auto owned = std::move(*begun);
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::RenderPassEncoder, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_render_pass_mutex};
            g_render_passes.emplace(token, std::move(owned));
        }
        out_pass->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_pipeline(SturdyRenderPassEncoder pass,
                                                                  SturdyRenderPipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_pipeline(RHI::RenderPipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_bind_group(SturdyRenderPassEncoder pass, uint32_t index,
                                                                    SturdyBindGroup bind_group,
                                                                    uint32_t dynamic_offset_count,
                                                                    const uint32_t *dynamic_offsets) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_bind_group(index, RHI::BindGroupHandle{bind_group.id},
                                std::span<const uint32_t>{dynamic_offsets, dynamic_offset_count});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_vertex_buffer(SturdyRenderPassEncoder pass, uint32_t slot,
                                                                       SturdyBuffer buffer, uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_vertex_buffer(slot, RHI::BufferHandle{buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_index_buffer(SturdyRenderPassEncoder pass,
                                                                      SturdyBuffer buffer, SturdyIndexFormat format,
                                                                      uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_index_buffer(RHI::BufferHandle{buffer.id}, static_cast<RHI::IndexFormat>(format), offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_push_constants(SturdyRenderPassEncoder pass,
                                                                        SturdyShaderStage stages, uint32_t offset,
                                                                        const void *data, size_t data_size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && data_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null when data_size is nonzero");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_push_constants(static_cast<RHI::ShaderStage>(stages), offset,
                                    std::span<const std::byte>{static_cast<const std::byte *>(data), data_size});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_viewport(SturdyRenderPassEncoder pass,
                                                                  const SturdyViewport *viewport) {
    return guarded([&]() -> SturdyResult {
        if (viewport == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "viewport must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_viewport(to_viewport(*viewport));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_scissor(SturdyRenderPassEncoder pass,
                                                                 const SturdyRect2D *scissor) {
    return guarded([&]() -> SturdyResult {
        if (scissor == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "scissor must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_scissor(to_rect(*scissor));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_blend_constant(SturdyRenderPassEncoder pass,
                                                                        const float color[4]) {
    return guarded([&]() -> SturdyResult {
        if (color == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "color must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_blend_constant(to_clear_color(color));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_stencil_reference(SturdyRenderPassEncoder pass,
                                                                           uint32_t reference) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_stencil_reference(reference);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw(SturdyRenderPassEncoder pass, const SturdyDrawArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw(RHI::DrawArgs{args->vertex_count, args->instance_count, args->first_vertex, args->first_instance});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed(SturdyRenderPassEncoder pass,
                                                                  const SturdyDrawIndexedArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed(RHI::DrawIndexedArgs{args->index_count, args->instance_count, args->first_index,
                                                   args->base_vertex, args->first_instance});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect(SturdyRenderPassEncoder pass,
                                                                    SturdyBuffer indirect_buffer,
                                                                    uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect_multi(SturdyRenderPassEncoder pass,
                                                                          SturdyBuffer indirect_buffer,
                                                                          uint64_t offset,
                                                                          uint32_t draw_count,
                                                                          uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect(RHI::BufferHandle{indirect_buffer.id}, offset, draw_count, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect_count(SturdyRenderPassEncoder pass,
                                                                          SturdyBuffer indirect_buffer,
                                                                          uint64_t indirect_offset,
                                                                          SturdyBuffer count_buffer,
                                                                          uint64_t count_offset,
                                                                          uint32_t max_draws,
                                                                          uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                     RHI::BufferHandle{count_buffer.id}, count_offset, max_draws, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect(SturdyRenderPassEncoder pass,
                                                                            SturdyBuffer indirect_buffer,
                                                                            uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect_multi(SturdyRenderPassEncoder pass,
                                                                                  SturdyBuffer indirect_buffer,
                                                                                  uint64_t offset,
                                                                                  uint32_t draw_count,
                                                                                  uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect(RHI::BufferHandle{indirect_buffer.id}, offset, draw_count, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect_count(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                             RHI::BufferHandle{count_buffer.id}, count_offset, max_draws,
                                             stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks(SturdyRenderPassEncoder pass,
                                                                     const SturdyDrawMeshTasksArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks(
            RHI::DrawMeshTasksArgs{args->group_count_x, args->group_count_y, args->group_count_z});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks_indirect(SturdyRenderPassEncoder pass,
                                                                              SturdyBuffer indirect_buffer,
                                                                              uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks_indirect_count(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                                RHI::BufferHandle{count_buffer.id}, count_offset, max_draws,
                                                stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_end(SturdyRenderPassEncoder pass) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->end();
        revoke_handle(pass.token);
        const std::lock_guard<std::mutex> lock{g_render_pass_mutex};
        g_render_passes.erase(pass.token);
        return STURDY_OK;
    });
}

// ─── Render bundles ─────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_desc_init(SturdyRenderBundleDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        *desc = SturdyRenderBundleDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyRenderBundleDesc));
        desc->samples = 1;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_render_bundle_encoder(SturdyEngine engine,
                                                                      const SturdyRenderBundleDesc *desc,
                                                                      SturdyRenderBundleEncoder *out_encoder) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_encoder == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->color_formats == nullptr && desc->color_format_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "color_formats must not be null when color_format_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::Format> color_formats;
        color_formats.reserve(desc->color_format_count);
        for (uint32_t i = 0; i < desc->color_format_count; ++i) {
            color_formats.push_back(static_cast<RHI::Format>(desc->color_formats[i]));
        }

        RHI::RenderBundleDesc native{};
        native.color_formats = color_formats;
        native.depth_stencil_format = static_cast<RHI::Format>(desc->depth_stencil_format);
        native.samples = static_cast<RHI::SampleCount>(desc->samples);
        native.view_mask = desc->view_mask;
        native.label = desc->label;

        auto created = device->create_render_bundle_encoder(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        auto owned = std::move(*created);
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::RenderBundleEncoder, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_render_bundle_encoder_mutex};
            g_render_bundle_encoders.emplace(token, std::move(owned));
        }
        out_encoder->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_encoder_release(SturdyRenderBundleEncoder encoder) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(encoder.token);
        const std::lock_guard<std::mutex> lock{g_render_bundle_encoder_mutex};
        g_render_bundle_encoders.erase(encoder.token);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_pipeline(SturdyRenderBundleEncoder encoder,
                                                                    SturdyRenderPipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_pipeline(RHI::RenderPipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_bind_group(SturdyRenderBundleEncoder encoder,
                                                                      uint32_t index, SturdyBindGroup bind_group,
                                                                      uint32_t dynamic_offset_count,
                                                                      const uint32_t *dynamic_offsets) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_bind_group(index, RHI::BindGroupHandle{bind_group.id},
                                std::span<const uint32_t>{dynamic_offsets, dynamic_offset_count});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_vertex_buffer(SturdyRenderBundleEncoder encoder,
                                                                         uint32_t slot, SturdyBuffer buffer,
                                                                         uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_vertex_buffer(slot, RHI::BufferHandle{buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_index_buffer(SturdyRenderBundleEncoder encoder,
                                                                        SturdyBuffer buffer,
                                                                        SturdyIndexFormat format,
                                                                        uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_index_buffer(RHI::BufferHandle{buffer.id}, static_cast<RHI::IndexFormat>(format), offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_push_constants(SturdyRenderBundleEncoder encoder,
                                                                          SturdyShaderStage stages,
                                                                          uint32_t offset, const void *data,
                                                                          size_t data_size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && data_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null when data_size is nonzero");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_push_constants(static_cast<RHI::ShaderStage>(stages), offset,
                                    std::span<const std::byte>{static_cast<const std::byte *>(data), data_size});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_viewport(SturdyRenderBundleEncoder encoder,
                                                                    const SturdyViewport *viewport) {
    return guarded([&]() -> SturdyResult {
        if (viewport == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "viewport must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_viewport(to_viewport(*viewport));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_scissor(SturdyRenderBundleEncoder encoder,
                                                                   const SturdyRect2D *scissor) {
    return guarded([&]() -> SturdyResult {
        if (scissor == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "scissor must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_scissor(to_rect(*scissor));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_blend_constant(SturdyRenderBundleEncoder encoder,
                                                                          const float color[4]) {
    return guarded([&]() -> SturdyResult {
        if (color == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "color must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_blend_constant(to_clear_color(color));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_stencil_reference(SturdyRenderBundleEncoder encoder,
                                                                             uint32_t reference) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_stencil_reference(reference);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw(SturdyRenderBundleEncoder encoder,
                                                            const SturdyDrawArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw(RHI::DrawArgs{args->vertex_count, args->instance_count, args->first_vertex,
                                   args->first_instance});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed(SturdyRenderBundleEncoder encoder,
                                                                    const SturdyDrawIndexedArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed(RHI::DrawIndexedArgs{args->index_count, args->instance_count, args->first_index,
                                                   args->base_vertex, args->first_instance});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect(SturdyRenderBundleEncoder encoder,
                                                                     SturdyBuffer indirect_buffer,
                                                                     uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect_multi(SturdyRenderBundleEncoder encoder,
                                                                           SturdyBuffer indirect_buffer,
                                                                           uint64_t offset, uint32_t draw_count,
                                                                           uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect(RHI::BufferHandle{indirect_buffer.id}, offset, draw_count, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                     RHI::BufferHandle{count_buffer.id}, count_offset, max_draws, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect(SturdyRenderBundleEncoder encoder,
                                                                             SturdyBuffer indirect_buffer,
                                                                             uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect_multi(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect(RHI::BufferHandle{indirect_buffer.id}, offset, draw_count, stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_indexed_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                             RHI::BufferHandle{count_buffer.id}, count_offset, max_draws,
                                             stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks(SturdyRenderBundleEncoder encoder,
                                                                       const SturdyDrawMeshTasksArgs *args) {
    return guarded([&]() -> SturdyResult {
        if (args == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "args must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks(
            RHI::DrawMeshTasksArgs{args->group_count_x, args->group_count_y, args->group_count_z});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks_indirect(SturdyRenderBundleEncoder encoder,
                                                                                SturdyBuffer indirect_buffer,
                                                                                uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->draw_mesh_tasks_indirect_count(RHI::BufferHandle{indirect_buffer.id}, indirect_offset,
                                                RHI::BufferHandle{count_buffer.id}, count_offset, max_draws,
                                                stride);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_encoder_finish(SturdyRenderBundleEncoder encoder,
                                                                      SturdyRenderBundle *out_bundle) {
    return guarded([&]() -> SturdyResult {
        if (out_bundle == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RenderBundleEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_bundle_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        auto finished = pointer->finish();
        revoke_handle(encoder.token);
        {
            const std::lock_guard<std::mutex> lock{g_render_bundle_encoder_mutex};
            g_render_bundle_encoders.erase(encoder.token);
        }
        if (!finished) {
            return translate_rhi_error(finished.error());
        }
        out_bundle->id = finished->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_render_bundle(SturdyEngine engine, SturdyRenderBundle bundle) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_render_bundle(RHI::RenderBundleHandle{bundle.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_execute_bundles(SturdyRenderPassEncoder pass,
                                                                     uint32_t bundle_count,
                                                                     const SturdyRenderBundle *bundles) {
    return guarded([&]() -> SturdyResult {
        if (bundles == nullptr && bundle_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "bundles must not be null when bundle_count is nonzero");
        }
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::RenderBundleHandle> handles;
        handles.reserve(bundle_count);
        for (uint32_t i = 0; i < bundle_count; ++i) {
            handles.push_back(RHI::RenderBundleHandle{bundles[i].id});
        }
        pointer->execute_bundles(handles);
        return STURDY_OK;
    });
}

// ─── Compute pass ───────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_compute_pass(SturdyCommandEncoder encoder,
                                                                           const char *label,
                                                                           SturdyComputePassEncoder *out_pass) {
    return guarded([&]() -> SturdyResult {
        if (out_pass == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::CommandEncoder *encoder_pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &encoder_pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::ComputePassDesc native{};
        native.label = label;
        auto begun = encoder_pointer->begin_compute_pass(native);
        if (!begun) {
            return translate_rhi_error(begun.error());
        }
        auto owned = std::move(*begun);
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::ComputePassEncoder, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_compute_pass_mutex};
            g_compute_passes.emplace(token, std::move(owned));
        }
        out_pass->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_pipeline(SturdyComputePassEncoder pass,
                                                                   SturdyComputePipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_pipeline(RHI::ComputePipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_bind_group(SturdyComputePassEncoder pass, uint32_t index,
                                                                     SturdyBindGroup bind_group,
                                                                     uint32_t dynamic_offset_count,
                                                                     const uint32_t *dynamic_offsets) {
    return guarded([&]() -> SturdyResult {
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_bind_group(index, RHI::BindGroupHandle{bind_group.id},
                                std::span<const uint32_t>{dynamic_offsets, dynamic_offset_count});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_push_constants(SturdyComputePassEncoder pass,
                                                                         SturdyShaderStage stages, uint32_t offset,
                                                                         const void *data, size_t data_size) {
    return guarded([&]() -> SturdyResult {
        if (data == nullptr && data_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null when data_size is nonzero");
        }
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_push_constants(static_cast<RHI::ShaderStage>(stages), offset,
                                    std::span<const std::byte>{static_cast<const std::byte *>(data), data_size});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_dispatch(SturdyComputePassEncoder pass, uint32_t group_count_x,
                                                               uint32_t group_count_y, uint32_t group_count_z) {
    return guarded([&]() -> SturdyResult {
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->dispatch(group_count_x, group_count_y, group_count_z);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_dispatch_indirect(SturdyComputePassEncoder pass,
                                                                        SturdyBuffer indirect_buffer,
                                                                        uint64_t offset) {
    return guarded([&]() -> SturdyResult {
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->dispatch_indirect(RHI::BufferHandle{indirect_buffer.id}, offset);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_end(SturdyComputePassEncoder pass) {
    return guarded([&]() -> SturdyResult {
        RHI::ComputePassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_compute_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->end();
        revoke_handle(pass.token);
        const std::lock_guard<std::mutex> lock{g_compute_pass_mutex};
        g_compute_passes.erase(pass.token);
        return STURDY_OK;
    });
}

// ─── Semaphores ─────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_semaphore(SturdyEngine engine, const SturdySemaphoreDesc *desc,
                                                          SturdySemaphore *out_semaphore) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_semaphore == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::SemaphoreDesc native{};
        native.initial_value = desc->initial_value;
        native.label = desc->label;
        auto created = device->create_semaphore(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_semaphore->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_semaphore(SturdyEngine engine, SturdySemaphore semaphore) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_semaphore(RHI::SemaphoreHandle{semaphore.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_semaphore_value(SturdyEngine engine, SturdySemaphore semaphore,
                                                         uint64_t *out_value) {
    return guarded([&]() -> SturdyResult {
        if (out_value == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto value = device->semaphore_value(RHI::SemaphoreHandle{semaphore.id});
        if (!value) {
            return translate_rhi_error(value.error());
        }
        *out_value = *value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_semaphore(SturdyEngine engine, SturdySemaphore semaphore,
                                                        uint64_t value, uint64_t timeout_ns) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto result = device->wait_semaphore(RHI::SemaphoreHandle{semaphore.id}, value, timeout_ns);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_signal_semaphore(SturdyEngine engine, SturdySemaphore semaphore,
                                                          uint64_t value) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto result = device->signal_semaphore(RHI::SemaphoreHandle{semaphore.id}, value);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

// ─── Query sets ─────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_query_set(SturdyEngine engine, const SturdyQuerySetDesc *desc,
                                                          SturdyQuerySet *out_query_set) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_query_set == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::QuerySetDesc native{};
        native.type = static_cast<RHI::QueryType>(desc->type);
        native.count = desc->count;
        native.statistics = static_cast<RHI::PipelineStatistic>(desc->statistics);
        native.label = desc->label;
        auto created = device->create_query_set(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_query_set->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_query_set(SturdyEngine engine, SturdyQuerySet query_set) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_query_set(RHI::QuerySetHandle{query_set.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_get_query_set_results(SturdyEngine engine, SturdyQuerySet query_set,
                                                               uint32_t first, uint32_t count, void *dst,
                                                               size_t dst_size, uint64_t stride,
                                                               SturdyQueryResultFlags flags) {
    return guarded([&]() -> SturdyResult {
        if (dst == nullptr && dst_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "dst must not be null when dst_size is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto result = device->get_query_set_results(
            RHI::QuerySetHandle{query_set.id}, first, count,
            std::span<std::byte>{static_cast<std::byte *>(dst), dst_size}, stride,
            static_cast<RHI::QueryResultFlags>(flags));
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_reset_query_set(SturdyEngine engine, SturdyQuerySet query_set,
                                                         uint32_t first, uint32_t count) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->reset_query_set(RHI::QuerySetHandle{query_set.id}, first, count);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_reset_query_set(SturdyCommandEncoder encoder,
                                                                         SturdyQuerySet query_set, uint32_t first,
                                                                         uint32_t count) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->reset_query_set(RHI::QuerySetHandle{query_set.id}, first, count);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_write_timestamp(SturdyCommandEncoder encoder,
                                                                         SturdyPipelineStage stage,
                                                                         SturdyQuerySet query_set, uint32_t index) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->write_timestamp(static_cast<RHI::PipelineStage>(stage), RHI::QuerySetHandle{query_set.id}, index);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_pipeline_statistics_query(
    SturdyCommandEncoder encoder, SturdyQuerySet query_set, uint32_t index) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->begin_pipeline_statistics_query(RHI::QuerySetHandle{query_set.id}, index);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_end_pipeline_statistics_query(SturdyCommandEncoder encoder) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->end_pipeline_statistics_query();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_resolve_query_set(SturdyCommandEncoder encoder,
                                                                           SturdyQuerySet query_set, uint32_t first,
                                                                           uint32_t count, SturdyBuffer dst,
                                                                           uint64_t dst_offset, uint64_t stride,
                                                                           SturdyQueryResultFlags flags) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->resolve_query_set(RHI::QuerySetHandle{query_set.id}, first, count, RHI::BufferHandle{dst.id},
                                   dst_offset, stride, static_cast<RHI::QueryResultFlags>(flags));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_begin_occlusion_query(SturdyRenderPassEncoder pass,
                                                                          SturdyQuerySet query_set, uint32_t index) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->begin_occlusion_query(RHI::QuerySetHandle{query_set.id}, index);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_end_occlusion_query(SturdyRenderPassEncoder pass) {
    return guarded([&]() -> SturdyResult {
        RHI::RenderPassEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_render_pass(pass, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->end_occlusion_query();
        return STURDY_OK;
    });
}

// ─── Fences ─────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_fence(SturdyEngine engine, const SturdyFenceDesc *desc,
                                                      SturdyFence *out_fence) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_fence == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::FenceDesc native{};
        native.signaled = desc->signaled != STURDY_FALSE;
        native.label = desc->label;
        auto created = device->create_fence(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_fence->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_fence(SturdyEngine engine, SturdyFence fence) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_fence(RHI::FenceHandle{fence.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_fences(SturdyEngine engine, uint32_t fence_count,
                                                     const SturdyFence *fences, SturdyBool wait_all,
                                                     uint64_t timeout_ns, SturdyBool *out_signaled) {
    return guarded([&]() -> SturdyResult {
        if (fences == nullptr && fence_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fences must not be null when fence_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::FenceHandle> native_fences;
        native_fences.reserve(fence_count);
        for (uint32_t i = 0; i < fence_count; ++i) {
            native_fences.push_back(RHI::FenceHandle{fences[i].id});
        }
        const auto result = device->wait_fences(native_fences, wait_all != STURDY_FALSE, timeout_ns);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        if (out_signaled != nullptr) {
            *out_signaled = *result ? STURDY_TRUE : STURDY_FALSE;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_reset_fences(SturdyEngine engine, uint32_t fence_count,
                                                      const SturdyFence *fences) {
    return guarded([&]() -> SturdyResult {
        if (fences == nullptr && fence_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fences must not be null when fence_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::FenceHandle> native_fences;
        native_fences.reserve(fence_count);
        for (uint32_t i = 0; i < fence_count; ++i) {
            native_fences.push_back(RHI::FenceHandle{fences[i].id});
        }
        const auto result = device->reset_fences(native_fences);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

// ─── Submit ─────────────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_submit(SturdyEngine engine, SturdyQueueClass queue_class,
                                               uint32_t queue_lane_index, uint32_t command_buffer_count,
                                               const SturdyCommandBuffer *command_buffers, uint32_t wait_count,
                                               const SturdySemaphoreWait *waits, uint32_t signal_count,
                                               const SturdySemaphoreSignal *signals, SturdyFence fence,
                                               SturdyBool one_shot) {
    return guarded([&]() -> SturdyResult {
        if (command_buffers == nullptr && command_buffer_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "command_buffers must not be null when command_buffer_count is nonzero");
        }
        if (waits == nullptr && wait_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "waits must not be null when wait_count is nonzero");
        }
        if (signals == nullptr && signal_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "signals must not be null when signal_count is nonzero");
        }
        RHI::QueueClass native_queue_class{};
        if (!translate_queue_class(queue_class, &native_queue_class)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized queue class");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::CommandBufferHandle> buffers;
        buffers.reserve(command_buffer_count);
        for (uint32_t i = 0; i < command_buffer_count; ++i) {
            buffers.push_back(RHI::CommandBufferHandle{command_buffers[i].id});
        }

        std::vector<RHI::QueueSemaphoreWait> native_waits;
        native_waits.reserve(wait_count);
        for (uint32_t i = 0; i < wait_count; ++i) {
            native_waits.push_back(RHI::QueueSemaphoreWait{RHI::SemaphoreHandle{waits[i].semaphore.id},
                                                            waits[i].value,
                                                            static_cast<RHI::PipelineStage>(waits[i].stages)});
        }
        std::vector<RHI::QueueSemaphoreSignal> native_signals;
        native_signals.reserve(signal_count);
        for (uint32_t i = 0; i < signal_count; ++i) {
            native_signals.push_back(RHI::QueueSemaphoreSignal{RHI::SemaphoreHandle{signals[i].semaphore.id},
                                                               signals[i].value,
                                                               static_cast<RHI::PipelineStage>(signals[i].stages)});
        }

        RHI::SubmitDesc desc{};
        desc.queue = RHI::QueueLane{native_queue_class, queue_lane_index};
        desc.command_buffers = buffers;
        desc.waits = native_waits;
        desc.signals = native_signals;
        desc.fence = RHI::FenceHandle{fence.id};
        desc.flags = one_shot != STURDY_FALSE ? RHI::SubmitFlags::OneShot : RHI::SubmitFlags::None;
        const auto result = device->submit(desc);
        if (!result) {
            return translate_rhi_error(result.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_idle(SturdyEngine engine) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->wait_idle();
        return STURDY_OK;
    });
}

} // extern "C"
