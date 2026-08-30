/// C ABI implementation of the ray tracing surface: acceleration structures, ray tracing
/// pipelines, shader binding tables, opacity micromaps, and `trace_rays`.
///
/// Acceleration structures, ray tracing pipelines, and opacity micromaps are the RHI's own
/// `Handle<Tag>` values passed through unchanged, exactly like `SturdyBuffer` — the device already
/// owns and validates them. `set_ray_tracing_pipeline`/`trace_rays`/`build_acceleration_structures`
/// live directly on `RHI::CommandEncoder` (outside any render/compute pass), not on a pass
/// encoder, so their FFI counterparts take a `SturdyCommandEncoder` the same way
/// `sturdy_rhi_command_encoder_barrier` does.
///
/// D3D12 has no equivalent to `VK_EXT_opacity_micromap`: `opacity_micromap_build_sizes`,
/// `create_opacity_micromap`, and `build_opacity_micromaps` report `STURDY_ERROR_NOT_AVAILABLE`
/// there. Everything else in this file is implemented on both backends.

#include <Foundation/Foundation.hpp>

#include <cstring>
#include <vector>

#include <Engine/Engine.hpp>
#include <RHI/RHI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::set_error;

    namespace RHI = SFT::RHI;

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

    [[nodiscard]] SturdyResult resolve_encoder(SturdyCommandEncoder encoder, RHI::CommandEncoder **out) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(encoder.token, HandleKind::CommandEncoder, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out = static_cast<RHI::CommandEncoder *>(pointer);
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

    [[nodiscard]] RHI::ShaderEntry to_shader_entry(const SturdyShaderEntry &entry) noexcept {
        RHI::ShaderEntry result{};
        result.module = RHI::ShaderModuleHandle{entry.module.id};
        result.entry_point = entry.entry_point != nullptr ? entry.entry_point : "main";
        result.stage = static_cast<RHI::ShaderStage>(entry.stage);
        return result;
    }

    [[nodiscard]] RHI::AccelerationStructureGeometryDesc to_geometry_desc(
        const SturdyAccelerationStructureGeometryDesc &g) noexcept {
        RHI::AccelerationStructureGeometryDesc native{};
        native.type = static_cast<RHI::AccelerationStructureGeometryType>(g.type);
        native.flags = static_cast<RHI::AccelerationStructureGeometryFlags>(g.flags);

        const SturdyAccelerationStructureTrianglesDesc &t = g.triangles;
        native.triangles.vertex_buffer = RHI::BufferHandle{t.vertex_buffer.id};
        native.triangles.vertex_offset = t.vertex_offset;
        native.triangles.vertex_format = static_cast<RHI::VertexFormat>(t.vertex_format);
        native.triangles.vertex_stride = t.vertex_stride;
        native.triangles.max_vertex = t.max_vertex;
        native.triangles.index_buffer = RHI::BufferHandle{t.index_buffer.id};
        native.triangles.index_offset = t.index_offset;
        native.triangles.index_format = static_cast<RHI::IndexFormat>(t.index_format);
        native.triangles.transform_buffer = RHI::BufferHandle{t.transform_buffer.id};
        native.triangles.transform_offset = t.transform_offset;
        native.triangles.opacity_micromap = RHI::OpacityMicromapHandle{t.opacity_micromap.id};
        native.triangles.opacity_micromap_index_buffer = RHI::BufferHandle{t.opacity_micromap_index_buffer.id};
        native.triangles.opacity_micromap_index_offset = t.opacity_micromap_index_offset;
        native.triangles.opacity_micromap_index_format =
            static_cast<RHI::IndexFormat>(t.opacity_micromap_index_format);

        const SturdyAccelerationStructureAabbsDesc &a = g.aabbs;
        native.aabbs.buffer = RHI::BufferHandle{a.buffer.id};
        native.aabbs.offset = a.offset;
        native.aabbs.stride = a.stride;

        const SturdyAccelerationStructureInstancesDesc &i = g.instances;
        native.instances.buffer = RHI::BufferHandle{i.buffer.id};
        native.instances.offset = i.offset;
        native.instances.array_of_pointers = i.array_of_pointers != STURDY_FALSE;

        return native;
    }

    [[nodiscard]] RHI::AccelerationStructureBuildRangeInfo to_build_range(
        const SturdyAccelerationStructureBuildRangeInfo &r) noexcept {
        return RHI::AccelerationStructureBuildRangeInfo{r.primitive_count, r.primitive_offset, r.first_vertex,
                                                        r.transform_offset};
    }

    /// Builds a native build-desc referencing `geometry_storage`/`range_storage`, which the
    /// caller must keep alive (and not reallocate) for as long as the returned desc is used.
    [[nodiscard]] RHI::AccelerationStructureBuildDesc to_build_desc(
        const SturdyAccelerationStructureBuildDesc &src,
        std::vector<RHI::AccelerationStructureGeometryDesc> &geometry_storage,
        std::vector<RHI::AccelerationStructureBuildRangeInfo> &range_storage) noexcept {
        for (uint32_t i = 0; i < src.geometry_count; ++i) {
            geometry_storage.push_back(to_geometry_desc(src.geometries[i]));
        }
        for (uint32_t i = 0; i < src.range_count; ++i) {
            range_storage.push_back(to_build_range(src.ranges[i]));
        }
        RHI::AccelerationStructureBuildDesc native{};
        native.type = static_cast<RHI::AccelerationStructureType>(src.type);
        native.flags = static_cast<RHI::AccelerationStructureBuildFlags>(src.flags);
        native.dst = RHI::AccelerationStructureHandle{src.dst.id};
        native.src = RHI::AccelerationStructureHandle{src.src.id};
        native.scratch_buffer = RHI::BufferHandle{src.scratch_buffer.id};
        native.scratch_offset = src.scratch_offset;
        native.geometries = geometry_storage;
        native.ranges = range_storage;
        return native;
    }

    [[nodiscard]] RHI::OpacityMicromapUsageCount to_usage_count(const SturdyOpacityMicromapUsageCount &u) noexcept {
        return RHI::OpacityMicromapUsageCount{u.count, u.subdivision_level,
                                              static_cast<RHI::OpacityMicromapFormat>(u.format)};
    }

} // namespace

extern "C" {

// ─── Acceleration structures ─────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_desc_init(SturdyAccelerationStructureDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        *desc = SturdyAccelerationStructureDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyAccelerationStructureDesc));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_acceleration_structure(
    SturdyEngine engine, const SturdyAccelerationStructureDesc *desc,
    SturdyAccelerationStructure *out_structure) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_structure == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        RHI::AccelerationStructureDesc native{};
        native.type = static_cast<RHI::AccelerationStructureType>(desc->type);
        native.size = desc->size;
        native.label = desc->label;
        const auto created = device->create_acceleration_structure(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_structure->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_acceleration_structure(SturdyEngine engine,
                                                                        SturdyAccelerationStructure structure) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_acceleration_structure(RHI::AccelerationStructureHandle{structure.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_device_address(
    SturdyEngine engine, SturdyAccelerationStructure structure, uint64_t *out_address) {
    return guarded([&]() -> SturdyResult {
        if (out_address == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto address = device->acceleration_structure_device_address(RHI::AccelerationStructureHandle{
            structure.id});
        if (!address) {
            return translate_rhi_error(address.error());
        }
        *out_address = *address;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_build_sizes(
    SturdyEngine engine, const SturdyAccelerationStructureBuildDesc *desc,
    SturdyAccelerationStructureBuildSizes *out_sizes) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_sizes == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->geometries == nullptr && desc->geometry_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "geometries must not be null when geometry_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::AccelerationStructureGeometryDesc> geometry_storage;
        std::vector<RHI::AccelerationStructureBuildRangeInfo> range_storage;
        geometry_storage.reserve(desc->geometry_count);
        range_storage.reserve(desc->range_count);
        const RHI::AccelerationStructureBuildDesc native = to_build_desc(*desc, geometry_storage, range_storage);

        const auto sizes = device->acceleration_structure_build_sizes(native);
        if (!sizes) {
            return translate_rhi_error(sizes.error());
        }
        out_sizes->acceleration_structure_size = sizes->acceleration_structure_size;
        out_sizes->build_scratch_size = sizes->build_scratch_size;
        out_sizes->update_scratch_size = sizes->update_scratch_size;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_build_acceleration_structures(
    SturdyCommandEncoder encoder, uint32_t build_count, const SturdyAccelerationStructureBuildDesc *builds) {
    return guarded([&]() -> SturdyResult {
        if (builds == nullptr && build_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "builds must not be null when build_count is nonzero");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // Each build's geometries/ranges must stay put for the whole call, so every build's
        // storage is reserved up front (never reallocated) rather than growing a single shared
        // vector that could relocate and dangle an earlier build's span.
        std::vector<std::vector<RHI::AccelerationStructureGeometryDesc>> geometry_storage(build_count);
        std::vector<std::vector<RHI::AccelerationStructureBuildRangeInfo>> range_storage(build_count);
        std::vector<RHI::AccelerationStructureBuildDesc> native_builds;
        native_builds.reserve(build_count);
        for (uint32_t i = 0; i < build_count; ++i) {
            geometry_storage[i].reserve(builds[i].geometry_count);
            range_storage[i].reserve(builds[i].range_count);
            native_builds.push_back(to_build_desc(builds[i], geometry_storage[i], range_storage[i]));
        }
        pointer->build_acceleration_structures(native_builds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_acceleration_structure(
    SturdyCommandEncoder encoder, const SturdyAccelerationStructureCopyDesc *copy) {
    return guarded([&]() -> SturdyResult {
        if (copy == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "copy must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const RHI::AccelerationStructureCopyDesc native{
            RHI::AccelerationStructureHandle{copy->src.id},
            RHI::AccelerationStructureHandle{copy->dst.id},
            static_cast<RHI::AccelerationStructureCopyMode>(copy->mode),
        };
        pointer->copy_acceleration_structure(native);
        return STURDY_OK;
    });
}

uint32_t STURDY_ABI_CALL sturdy_rhi_pack_instance_custom_index_and_mask(uint32_t custom_index, uint8_t mask) {
    return (custom_index & 0x00ffffffu) | (static_cast<uint32_t>(mask) << 24u);
}

uint32_t STURDY_ABI_CALL sturdy_rhi_pack_instance_sbt_offset_and_flags(uint32_t sbt_offset, uint8_t flags) {
    return (sbt_offset & 0x00ffffffu) | (static_cast<uint32_t>(flags) << 24u);
}

// ─── Opacity micromaps ────────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_opacity_micromap_desc_init(SturdyOpacityMicromapDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        *desc = SturdyOpacityMicromapDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyOpacityMicromapDesc));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_opacity_micromap_build_sizes(
    SturdyEngine engine, const SturdyOpacityMicromapDesc *desc, SturdyOpacityMicromapBuildSizes *out_sizes) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_sizes == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->usage_counts == nullptr && desc->usage_count_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "usage_counts must not be null when usage_count_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::OpacityMicromapUsageCount> usage_counts;
        usage_counts.reserve(desc->usage_count_count);
        for (uint32_t i = 0; i < desc->usage_count_count; ++i) {
            usage_counts.push_back(to_usage_count(desc->usage_counts[i]));
        }
        RHI::OpacityMicromapDesc native{};
        native.usage_counts = usage_counts;
        native.label = desc->label;

        const auto sizes = device->opacity_micromap_build_sizes(native);
        if (!sizes) {
            return translate_rhi_error(sizes.error());
        }
        out_sizes->micromap_size = sizes->micromap_size;
        out_sizes->build_scratch_size = sizes->build_scratch_size;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_opacity_micromap(SturdyEngine engine,
                                                                 const SturdyOpacityMicromapDesc *desc,
                                                                 uint64_t size,
                                                                 SturdyOpacityMicromap *out_micromap) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_micromap == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->usage_counts == nullptr && desc->usage_count_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "usage_counts must not be null when usage_count_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        std::vector<RHI::OpacityMicromapUsageCount> usage_counts;
        usage_counts.reserve(desc->usage_count_count);
        for (uint32_t i = 0; i < desc->usage_count_count; ++i) {
            usage_counts.push_back(to_usage_count(desc->usage_counts[i]));
        }
        RHI::OpacityMicromapDesc native{};
        native.usage_counts = usage_counts;
        native.label = desc->label;

        const auto created = device->create_opacity_micromap(native, size);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_micromap->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_opacity_micromap(SturdyEngine engine, SturdyOpacityMicromap micromap) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_opacity_micromap(RHI::OpacityMicromapHandle{micromap.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_build_opacity_micromaps(
    SturdyCommandEncoder encoder, uint32_t build_count, const SturdyOpacityMicromapBuildDesc *builds) {
    return guarded([&]() -> SturdyResult {
        if (builds == nullptr && build_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "builds must not be null when build_count is nonzero");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<std::vector<RHI::OpacityMicromapUsageCount>> usage_storage(build_count);
        std::vector<RHI::OpacityMicromapBuildDesc> native_builds;
        native_builds.reserve(build_count);
        for (uint32_t i = 0; i < build_count; ++i) {
            const SturdyOpacityMicromapBuildDesc &src = builds[i];
            usage_storage[i].reserve(src.usage_count_count);
            for (uint32_t j = 0; j < src.usage_count_count; ++j) {
                usage_storage[i].push_back(to_usage_count(src.usage_counts[j]));
            }
            RHI::OpacityMicromapBuildDesc native{};
            native.dst = RHI::OpacityMicromapHandle{src.dst.id};
            native.scratch_buffer = RHI::BufferHandle{src.scratch_buffer.id};
            native.scratch_offset = src.scratch_offset;
            native.data_buffer = RHI::BufferHandle{src.data_buffer.id};
            native.data_buffer_offset = src.data_buffer_offset;
            native.usage_counts = usage_storage[i];
            native_builds.push_back(native);
        }
        pointer->build_opacity_micromaps(native_builds);
        return STURDY_OK;
    });
}

// ─── Ray tracing pipelines ────────────────────────────────────────────────────

SturdyResult STURDY_ABI_CALL sturdy_rhi_ray_tracing_pipeline_desc_init(SturdyRayTracingPipelineDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        *desc = SturdyRayTracingPipelineDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyRayTracingPipelineDesc));
        desc->max_ray_recursion_depth = 1;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_create_ray_tracing_pipeline(SturdyEngine engine,
                                                                     const SturdyRayTracingPipelineDesc *desc,
                                                                     SturdyRayTracingPipeline *out_pipeline) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_pipeline == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->groups == nullptr && desc->group_count != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "groups must not be null when group_count is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<RHI::RayTracingShaderGroupDesc> groups;
        groups.reserve(desc->group_count);
        for (uint32_t i = 0; i < desc->group_count; ++i) {
            const SturdyRayTracingShaderGroupDesc &g = desc->groups[i];
            RHI::RayTracingShaderGroupDesc native{};
            native.type = static_cast<RHI::RayTracingShaderGroupType>(g.type);
            native.general = to_shader_entry(g.general);
            native.closest_hit = to_shader_entry(g.closest_hit);
            native.any_hit = to_shader_entry(g.any_hit);
            native.intersection = to_shader_entry(g.intersection);
            groups.push_back(native);
        }

        RHI::RayTracingPipelineDesc native{};
        native.layout = RHI::PipelineLayoutHandle{desc->layout.id};
        native.groups = groups;
        native.max_ray_recursion_depth = desc->max_ray_recursion_depth;
        native.label = desc->label;

        const auto created = device->create_ray_tracing_pipeline(native);
        if (!created) {
            return translate_rhi_error(created.error());
        }
        out_pipeline->id = created->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_ray_tracing_pipeline(SturdyEngine engine,
                                                                      SturdyRayTracingPipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        device->destroy_ray_tracing_pipeline(RHI::RayTracingPipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_write_ray_tracing_shader_group_handles(
    SturdyEngine engine, SturdyRayTracingPipeline pipeline, uint32_t first_group, uint32_t group_count, void *dst,
    size_t dst_size) {
    return guarded([&]() -> SturdyResult {
        if (dst == nullptr && dst_size != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "dst must not be null when dst_size is nonzero");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto written = device->write_ray_tracing_shader_group_handles(
            RHI::RayTracingPipelineHandle{pipeline.id}, first_group, group_count,
            std::span<std::byte>{static_cast<std::byte *>(dst), dst_size});
        if (!written) {
            return translate_rhi_error(written.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_ray_tracing_properties(SturdyEngine engine,
                                                                SturdyRayTracingProperties *out_properties) {
    return guarded([&]() -> SturdyResult {
        if (out_properties == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        RHI::RhiDevice *device = nullptr;
        const SturdyResult resolved = resolve_device(engine, &device);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const RHI::RayTracingProperties &props = device->feature_properties().ray_tracing;
        *out_properties = SturdyRayTracingProperties{};
        out_properties->struct_size = static_cast<uint32_t>(sizeof(SturdyRayTracingProperties));
        out_properties->max_ray_recursion_depth = props.max_ray_recursion_depth;
        out_properties->shader_group_handle_size = props.shader_group_handle_size;
        out_properties->shader_group_base_alignment = props.shader_group_base_alignment;
        out_properties->max_ray_hit_attribute_size = props.max_ray_hit_attribute_size;
        out_properties->max_acceleration_structure_geometry_count = props.max_acceleration_structure_geometry_count;
        out_properties->max_acceleration_structure_instance_count =
            props.max_acceleration_structure_instance_count;
        out_properties->min_acceleration_structure_scratch_offset_alignment =
            props.min_acceleration_structure_scratch_offset_alignment;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_set_ray_tracing_pipeline(SturdyCommandEncoder encoder,
                                                                                  SturdyRayTracingPipeline pipeline) {
    return guarded([&]() -> SturdyResult {
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        pointer->set_ray_tracing_pipeline(RHI::RayTracingPipelineHandle{pipeline.id});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_trace_rays(SturdyCommandEncoder encoder,
                                                                    const SturdyTraceRaysDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        RHI::CommandEncoder *pointer = nullptr;
        const SturdyResult resolved = resolve_encoder(encoder, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto to_region = [](const SturdyShaderBindingTableRegion &r) noexcept {
            return RHI::ShaderBindingTableRegion{RHI::BufferHandle{r.buffer.id}, r.offset, r.size, r.stride};
        };
        RHI::TraceRaysDesc native{};
        native.raygen = to_region(desc->raygen);
        native.miss = to_region(desc->miss);
        native.hit = to_region(desc->hit);
        native.callable = to_region(desc->callable);
        native.width = desc->width;
        native.height = desc->height;
        native.depth = desc->depth;
        pointer->trace_rays(native);
        return STURDY_OK;
    });
}

} // extern "C"
