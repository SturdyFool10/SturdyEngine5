/// C ABI implementation of the RHI introspection surface.
///
/// Reports what device the engine actually got: backend, adapter identity, limits, negotiated
/// features, queue families, enabled extensions. This is what a consumer needs to make its own
/// decisions — pick a quality preset, warn about a weak GPU, or find out whether the native-access
/// extension is live before reaching for raw handles.
///
/// Features are addressed by index and name rather than through a mirrored enum. The engine has
/// several hundred and gains more over time; restating that list in the header would guarantee the
/// two drift, and a name-based lookup means a binding compiled today keeps working against a newer
/// engine that added features it has never heard of.

#include <Foundation/Foundation.hpp>

#include <string_view>

#include <Engine/Engine.hpp>
#include <RHI/RHI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    using SFT::u32;
    using SFT::usize;

    /// Resolves an engine handle to its active RHI device.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_device Receives the borrowed device on success.
    ///
    /// @return `STURDY_OK`; `STURDY_ERROR_NOT_AVAILABLE` when the engine has no device yet, which
    ///         is the case before initialization completes.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_device(SturdyEngine engine, SFT::RHI::RhiDevice **out_device) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::RHI::RhiDevice *device = resolved_engine->rhi_device();
        if (device == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no active RHI device");
        }
        *out_device = device;
        return STURDY_OK;
    }

    /// Translates the engine's backend enumeration to the ABI's.
    ///
    /// @param backend Engine-side value.
    /// @param out_backend Receives the translated value.
    ///
    /// @return Returns `true` when the backend has an ABI spelling; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_backend(SFT::RHI::BackendType backend, SturdyBackend *out_backend) noexcept {
        switch (backend) {
        case SFT::RHI::BackendType::Vulkan:
            *out_backend = STURDY_BACKEND_VULKAN;
            return true;
        case SFT::RHI::BackendType::D3D12:
            *out_backend = STURDY_BACKEND_D3D12;
            return true;
        case SFT::RHI::BackendType::Metal:
        case SFT::RHI::BackendType::WebGpu:
        default:
            // Metal and WebGPU have no ABI spelling yet. Reporting that honestly is better than
            // mapping them onto Vulkan, which would make a caller's backend check silently wrong.
            return false;
        }
    }

    /// Translates the engine's device-type enumeration to the ABI's.
    ///
    /// @param type Engine-side value.
    ///
    /// @return The ABI spelling, defaulting to `STURDY_DEVICE_TYPE_OTHER`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyDeviceType translate_device_type(SFT::RHI::DeviceType type) noexcept {
        switch (type) {
        case SFT::RHI::DeviceType::IntegratedGpu:
            return STURDY_DEVICE_TYPE_INTEGRATED_GPU;
        case SFT::RHI::DeviceType::DiscreteGpu:
            return STURDY_DEVICE_TYPE_DISCRETE_GPU;
        case SFT::RHI::DeviceType::VirtualGpu:
            return STURDY_DEVICE_TYPE_VIRTUAL_GPU;
        case SFT::RHI::DeviceType::Cpu:
            return STURDY_DEVICE_TYPE_CPU;
        case SFT::RHI::DeviceType::Other:
        default:
            return STURDY_DEVICE_TYPE_OTHER;
        }
    }

    /// Translates the engine's queue-class enumeration to the ABI's.
    ///
    /// @param queue Engine-side value.
    ///
    /// @return The ABI spelling, defaulting to `STURDY_QUEUE_CLASS_GRAPHICS`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyQueueClass translate_queue_class(SFT::RHI::QueueClass queue) noexcept {
        switch (queue) {
        case SFT::RHI::QueueClass::Compute:
            return STURDY_QUEUE_CLASS_COMPUTE;
        case SFT::RHI::QueueClass::Transfer:
            return STURDY_QUEUE_CLASS_TRANSFER;
        case SFT::RHI::QueueClass::Sparse:
            return STURDY_QUEUE_CLASS_SPARSE;
        case SFT::RHI::QueueClass::VideoDecode:
            return STURDY_QUEUE_CLASS_VIDEO_DECODE;
        case SFT::RHI::QueueClass::VideoEncode:
            return STURDY_QUEUE_CLASS_VIDEO_ENCODE;
        case SFT::RHI::QueueClass::Graphics:
        default:
            return STURDY_QUEUE_CLASS_GRAPHICS;
        }
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_rhi_backend(SturdyEngine engine, SturdyBackend *out_backend) {
    return guarded([&]() -> SturdyResult {
        if (out_backend == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!translate_backend(device->backend_type(), out_backend)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE,
                             "the active graphics backend has no spelling in this ABI version");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_adapter_info(SturdyEngine engine, SturdyAdapterInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::RHI::AdapterInfo &info = device->adapter_info();
        *out_info = SturdyAdapterInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyAdapterInfo));
        out_info->vendor_id = info.vendor_id;
        out_info->device_id = info.device_id;
        out_info->device_type = translate_device_type(info.device_type);
        out_info->is_discrete = info.is_discrete ? STURDY_TRUE : STURDY_FALSE;
        // An adapter whose backend has no ABI spelling still reports everything else; only the
        // backend field falls back, and sturdy_rhi_backend() is where that is reported as an error.
        if (!translate_backend(info.backend, &out_info->backend)) {
            out_info->backend = STURDY_BACKEND_DEFAULT;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_adapter_string(SturdyEngine engine,
                                                       SturdyAdapterString which,
                                                       char *buffer,
                                                       size_t capacity,
                                                       size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::RHI::AdapterInfo &info = device->adapter_info();
        std::string_view text;
        switch (which) {
        case STURDY_ADAPTER_STRING_NAME:
            text = info.name;
            break;
        case STURDY_ADAPTER_STRING_VENDOR:
            text = info.vendor;
            break;
        case STURDY_ADAPTER_STRING_DRIVER_VERSION:
            text = info.driver_version;
            break;
        case STURDY_ADAPTER_STRING_API_VERSION:
            text = info.api_version;
            break;
        case STURDY_ADAPTER_STRING_PHYSICAL_DEVICE_ID:
            text = info.physical_device_id;
            break;
        case STURDY_ADAPTER_STRING_FORCE_U32:
        default:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized adapter string selector");
        }
        return copy_string_out(text, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_device_limits(SturdyEngine engine, SturdyDeviceLimits *out_limits) {
    return guarded([&]() -> SturdyResult {
        if (out_limits == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::RHI::DeviceLimits &limits = device->limits();
        *out_limits = SturdyDeviceLimits{};
        out_limits->struct_size = static_cast<uint32_t>(sizeof(SturdyDeviceLimits));
        out_limits->max_texture_dimension_2d = limits.max_texture_dimension_2d;
        out_limits->max_texture_array_layers = limits.max_texture_array_layers;
        out_limits->max_bind_groups = limits.max_bind_groups;
        out_limits->max_push_constants_size = limits.max_push_constants_size;
        out_limits->max_vertex_buffers = limits.max_vertex_buffers;
        out_limits->max_vertex_attributes = limits.max_vertex_attributes;
        out_limits->max_color_attachments = limits.max_color_attachments;
        out_limits->max_framebuffer_sample_count = limits.max_framebuffer_sample_count;
        out_limits->framebuffer_sample_counts = limits.framebuffer_sample_counts;
        out_limits->max_compute_workgroup_size_x = limits.max_compute_workgroup_size_x;
        out_limits->max_compute_workgroup_size_y = limits.max_compute_workgroup_size_y;
        out_limits->max_compute_workgroup_size_z = limits.max_compute_workgroup_size_z;
        out_limits->timestamp_valid_bits = limits.timestamp_valid_bits;
        out_limits->min_uniform_buffer_offset_alignment = limits.min_uniform_buffer_offset_alignment;
        out_limits->min_storage_buffer_offset_alignment = limits.min_storage_buffer_offset_alignment;
        out_limits->timestamp_period_ns = limits.timestamp_period_ns;
        out_limits->supports_minimum_depth_resolve =
            limits.supports_minimum_depth_resolve ? STURDY_TRUE : STURDY_FALSE;
        out_limits->supports_bc_texture_compression =
            limits.supports_bc_texture_compression ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_count(uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *out_count = static_cast<uint32_t>(SFT::RHI::feature_count);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_name(uint32_t index,
                                                     char *buffer,
                                                     size_t capacity,
                                                     size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        if (index >= SFT::RHI::feature_count) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "feature index is out of range");
        }
        const char *name = SFT::RHI::feature_name(static_cast<SFT::RHI::Feature>(index));
        return copy_string_out(name != nullptr ? std::string_view{name} : std::string_view{}, buffer,
                               capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_index(const char *name, uint32_t *out_index) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr || out_index == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name and output pointer must not be null");
        }

        const std::string_view requested{name};
        for (usize index = 0; index < SFT::RHI::feature_count; ++index) {
            const char *candidate = SFT::RHI::feature_name(static_cast<SFT::RHI::Feature>(index));
            if (candidate != nullptr && requested == candidate) {
                *out_index = static_cast<uint32_t>(index);
                return STURDY_OK;
            }
        }
        return set_error(STURDY_ERROR_NOT_AVAILABLE, "this engine build has no feature with that name");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_enabled(SturdyEngine engine,
                                                        uint32_t index,
                                                        SturdyBool *out_enabled) {
    return guarded([&]() -> SturdyResult {
        if (out_enabled == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        if (index >= SFT::RHI::feature_count) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "feature index is out of range");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_enabled = device->enabled_features().has(static_cast<SFT::RHI::Feature>(index)) ? STURDY_TRUE
                                                                                            : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_queue_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(device->queue_infos().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_queue_info(SturdyEngine engine,
                                                   uint32_t index,
                                                   SturdyQueueInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto queues = device->queue_infos();
        if (index >= queues.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "queue index is out of range");
        }

        const SFT::RHI::QueueInfo &info = queues[index];
        *out_info = SturdyQueueInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyQueueInfo));
        out_info->queue_class = translate_queue_class(info.queue);
        out_info->capabilities = static_cast<uint32_t>(info.capabilities);
        out_info->lane_count = info.lane_count;
        out_info->physical_group = info.physical_group;
        out_info->likely_parallel_with_graphics =
            info.likely_parallel_with_graphics ? STURDY_TRUE : STURDY_FALSE;
        out_info->dedicated = info.dedicated ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_extension_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(device->enabled_extensions().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_extension_name(SturdyEngine engine,
                                                       uint32_t index,
                                                       char *buffer,
                                                       size_t capacity,
                                                       size_t *out_length,
                                                       uint32_t *out_version) {
    return guarded([&]() -> SturdyResult {
        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto extensions = device->enabled_extensions();
        if (index >= extensions.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "extension index is out of range");
        }

        const SFT::RHI::ExtensionId &extension = extensions[index];
        if (out_version != nullptr) {
            *out_version = extension.version;
        }

        // Joined rather than reported as two fields: an extension is identified by the pair, and a
        // single "namespace.name" string is what a caller can compare or log directly.
        std::string qualified;
        qualified.reserve(extension.name_space.size() + extension.name.size() + 1);
        qualified.append(extension.name_space);
        qualified.push_back('.');
        qualified.append(extension.name);
        return copy_string_out(qualified, buffer, capacity, out_length);
    });
}

} // extern "C"
