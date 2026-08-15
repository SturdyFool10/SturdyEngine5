// DXR: ray tracing state objects, shader-identifier export, and acceleration structures.
//
// The RHI's ray-tracing vocabulary is Vulkan-shaped (a pipeline built from shader *groups*, whose
// opaque handles are copied into a shader binding table). DXR's is a state object built from
// *subobjects*, whose per-export shader identifiers are fetched by name. The two map cleanly, and
// this file is where the translation lives:
//
//   RayTracingShaderGroupDesc  ->  a DXIL library subobject per stage plus, for hit groups, a
//                                  D3D12_HIT_GROUP_DESC naming the closest-hit/any-hit/intersection
//                                  exports.
//   shader group handle        ->  ID3D12StateObjectProperties::GetShaderIdentifier(export name).
//                                  Both are 32 bytes (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES equals
//                                  the shaderGroupHandleSize this backend reports), so a caller's SBT
//                                  packing code is identical on either backend.
//
// Each group gets a generated, guaranteed-unique export name (`sturdy_group_<n>`), because DXIL
// libraries frequently share entry-point names across modules and DXR requires every export in a
// state object to be uniquely named.
#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {

        [[nodiscard]] std::wstring export_name(const char *prefix, usize group_index) {
            std::wstring name;
            for (const char *cursor = prefix; *cursor != '\0'; ++cursor) {
                name.push_back(static_cast<wchar_t>(*cursor));
            }
            for (wchar_t character : std::to_wstring(group_index)) {
                name.push_back(character);
            }
            return name;
        }

        // Vertex position format for a BLAS triangle geometry. DXR accepts only a small set here, so an
        // unsupported one is reported rather than silently reinterpreted.
        [[nodiscard]] bool to_dxr_vertex_format(rhi::VertexFormat format, DXGI_FORMAT &out) noexcept {
            switch (format) {
                case rhi::VertexFormat::Float32x3: out = DXGI_FORMAT_R32G32B32_FLOAT; return true;
                case rhi::VertexFormat::Float32x2: out = DXGI_FORMAT_R32G32_FLOAT; return true;
                case rhi::VertexFormat::Float16x2: out = DXGI_FORMAT_R16G16_FLOAT; return true;
                case rhi::VertexFormat::Float16x4: out = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
                case rhi::VertexFormat::Uint16x2Unorm: out = DXGI_FORMAT_R16G16_UNORM; return true;
                case rhi::VertexFormat::Uint16x4Unorm: out = DXGI_FORMAT_R16G16B16A16_UNORM; return true;
                default: return false;
            }
        }

    } // namespace

    rhi::RhiExpected<rhi::RayTracingPipelineHandle> D3D12Device::create_ray_tracing_pipeline(
        const rhi::RayTracingPipelineDesc &desc) {
        ZoneScopedN("D3D12Device::create_ray_tracing_pipeline");

        if (device5_ == nullptr || !enabled_features_.has(rhi::Feature::RayTracingPipeline)) {
            return unsupported("create_ray_tracing_pipeline: ray tracing pipelines are not enabled on this device.");
        }
        const PipelineLayoutRecord *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return invalid_argument("create_ray_tracing_pipeline: unknown pipeline layout handle.");
        }

        RayTracingPipelineRecord record{};
        record.layout = desc.layout;

        CD3DX12_STATE_OBJECT_DESC state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
        // Every subobject the helper hands out points into storage owned by `state_object`, so all of
        // these stay alive until CreateStateObject is called below; the export-name strings, however,
        // are ours and must outlive the call too — hence the retained vector rather than temporaries.
        vector<std::wstring> stage_exports;
        stage_exports.reserve(desc.groups.size() * 4);

        // Adds one DXIL library exporting `entry`'s single entry point under a fresh unique name.
        const auto add_stage = [&](const rhi::ShaderEntry &entry, const char *prefix,
                                   usize index) -> rhi::RhiExpected<const std::wstring *> {
            if (!entry.module.is_valid()) {
                return static_cast<const std::wstring *>(nullptr);
            }
            const ShaderModuleRecord *module = shader_modules_.find(entry.module);
            if (module == nullptr) {
                return invalid_argument("create_ray_tracing_pipeline: a shader group names an unknown module.");
            }
            stage_exports.push_back(export_name(prefix, index));
            const std::wstring &name = stage_exports.back();

            auto *library = state_object.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
            const D3D12_SHADER_BYTECODE bytecode{module->bytecode.data(), module->bytecode.size()};
            library->SetDXILLibrary(&bytecode);
            // Renaming on export is what makes the generated unique names work: the library's own
            // entry point keeps whatever name Slang gave it, and the state object sees `name`.
            library->DefineExport(name.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE);
            return &name;
        };

        for (usize index = 0; index < desc.groups.size(); ++index) {
            const rhi::RayTracingShaderGroupDesc &group = desc.groups[index];

            if (group.type == rhi::RayTracingShaderGroupType::General) {
                auto general = add_stage(group.general, "sturdy_general_", index);
                if (!general) {
                    return std::unexpected(general.error());
                }
                if (*general == nullptr) {
                    return invalid_argument("create_ray_tracing_pipeline: a General group has no entry point.");
                }
                // A general group *is* its export — raygen/miss/callable are addressed by the entry
                // point's own identifier, with no hit-group wrapper.
                record.group_exports.push_back(**general);
                continue;
            }

            auto closest_hit = add_stage(group.closest_hit, "sturdy_closesthit_", index);
            if (!closest_hit) {
                return std::unexpected(closest_hit.error());
            }
            auto any_hit = add_stage(group.any_hit, "sturdy_anyhit_", index);
            if (!any_hit) {
                return std::unexpected(any_hit.error());
            }
            auto intersection = add_stage(group.intersection, "sturdy_intersection_", index);
            if (!intersection) {
                return std::unexpected(intersection.error());
            }

            std::wstring hit_group = export_name("sturdy_group_", index);
            auto *subobject = state_object.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
            subobject->SetHitGroupType(group.type == rhi::RayTracingShaderGroupType::ProceduralHitGroup
                                           ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
                                           : D3D12_HIT_GROUP_TYPE_TRIANGLES);
            if (*closest_hit != nullptr) {
                subobject->SetClosestHitShaderImport((*closest_hit)->c_str());
            }
            if (*any_hit != nullptr) {
                subobject->SetAnyHitShaderImport((*any_hit)->c_str());
            }
            if (*intersection != nullptr) {
                subobject->SetIntersectionShaderImport((*intersection)->c_str());
            }
            record.group_exports.push_back(hit_group);
            subobject->SetHitGroupExport(record.group_exports.back().c_str());
        }

        auto *shader_config = state_object.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
        // The RHI carries no payload/attribute size fields, and DXR requires both up front. The maxima
        // are used rather than a guess: over-declaring costs some per-ray scratch space, whereas
        // under-declaring is undefined behaviour the caller has no way to detect or correct.
        shader_config->Config(D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES * 4,
                              D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES);

        auto *pipeline_config = state_object.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
        pipeline_config->Config(std::max(1u, desc.max_ray_recursion_depth));

        // The global root signature: DXR's equivalent of the pipeline layout every other pipeline type
        // takes directly. Local root signatures (per-SBT-record arguments) are not expressible in the
        // RHI's vocabulary and so are deliberately not created.
        auto *global_root_signature = state_object.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
        global_root_signature->SetRootSignature(layout->root_signature.Get());

        if (const HRESULT hr = device5_->CreateStateObject(state_object, IID_PPV_ARGS(&record.state_object));
            FAILED(hr)) {
            return hresult_error(hr, "create_ray_tracing_pipeline (CreateStateObject)");
        }
        if (const HRESULT hr = record.state_object.As(&record.properties); FAILED(hr)) {
            return hresult_error(hr, "create_ray_tracing_pipeline (ID3D12StateObjectProperties)");
        }
        set_debug_name(record.state_object.Get(), desc.label);
        return ray_tracing_pipelines_.insert(std::move(record));
    }

    void D3D12Device::destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept {
        ray_tracing_pipelines_.erase(handle);
    }

    rhi::RhiResult D3D12Device::write_ray_tracing_shader_group_handles(rhi::RayTracingPipelineHandle pipeline,
                                                                       u32 first_group, u32 group_count,
                                                                       span<std::byte> dst) {
        ZoneScopedN("D3D12Device::write_ray_tracing_shader_group_handles");
        const RayTracingPipelineRecord *record = ray_tracing_pipelines_.find(pipeline);
        if (record == nullptr) {
            return invalid_argument("write_ray_tracing_shader_group_handles: unknown ray tracing pipeline handle.");
        }
        if (first_group + group_count > record->group_exports.size()) {
            return invalid_argument("write_ray_tracing_shader_group_handles: the requested group range exceeds the "
                                    "pipeline's group count.");
        }
        const usize handle_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        if (dst.size() < static_cast<usize>(group_count) * handle_size) {
            return invalid_argument("write_ray_tracing_shader_group_handles: the destination span is too small.");
        }

        for (u32 index = 0; index < group_count; ++index) {
            const void *identifier =
                record->properties->GetShaderIdentifier(record->group_exports[first_group + index].c_str());
            if (identifier == nullptr) {
                return operation_failed("write_ray_tracing_shader_group_handles: the state object has no identifier "
                                        "for shader group " + std::to_string(first_group + index) + ".");
            }
            std::memcpy(dst.data() + static_cast<usize>(index) * handle_size, identifier, handle_size);
        }
        return {};
    }

    // ─── Acceleration structures ─────────────────────────────────────────────────

    rhi::RhiResult build_acceleration_structure_inputs(
        const D3D12Device &device, const rhi::AccelerationStructureBuildDesc &desc,
        vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometry_storage,
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs) {
        inputs = {};
        inputs.Type = desc.type == rhi::AccelerationStructureType::TopLevel
                          ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
                          : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = to_d3d12(desc.flags);
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        if (desc.type == rhi::AccelerationStructureType::TopLevel) {
            // A TLAS takes instance records by GPU address rather than a geometry array. The RHI's
            // AccelerationStructureInstance is a 64-byte record laid out to match
            // D3D12_RAYTRACING_INSTANCE_DESC bit-for-bit (row-major 3x4 transform, packed 24-bit index
            // + 8-bit mask, packed 24-bit SBT offset + 8-bit flags, then the BLAS address), which is
            // why the buffer is handed straight to D3D12 with no repacking.
            static_assert(sizeof(rhi::AccelerationStructureInstance) == sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            u32 instance_count = 0;
            for (const rhi::AccelerationStructureBuildRangeInfo &range : desc.ranges) {
                instance_count += range.primitive_count;
            }
            inputs.NumDescs = instance_count;
            if (!desc.geometries.empty()) {
                const rhi::AccelerationStructureInstancesDesc &instances = desc.geometries.front().instances;
                if (instances.array_of_pointers) {
                    return unsupported("build_acceleration_structures: D3D12 top-level builds do not accept an "
                                       "array-of-pointers instance layout.");
                }
                if (const BufferRecord *buffer = device.find_buffer_for_build(instances.buffer)) {
                    inputs.InstanceDescs = buffer->gpu_address + instances.offset;
                } else {
                    return invalid_argument("build_acceleration_structures: the instance buffer handle is unknown.");
                }
            }
            return {};
        }

        geometry_storage.clear();
        geometry_storage.reserve(desc.geometries.size());
        for (usize index = 0; index < desc.geometries.size(); ++index) {
            const rhi::AccelerationStructureGeometryDesc &geometry = desc.geometries[index];
            // Vulkan splits a build into a geometry description plus a parallel range array supplying
            // the primitive counts/offsets; D3D12 folds both into one structure, so the two are merged
            // here. A missing range entry means zero primitives, matching Vulkan's own requirement that
            // the arrays be the same length.
            const rhi::AccelerationStructureBuildRangeInfo range =
                index < desc.ranges.size() ? desc.ranges[index] : rhi::AccelerationStructureBuildRangeInfo{};

            D3D12_RAYTRACING_GEOMETRY_DESC out{};
            out.Flags = to_d3d12(geometry.flags);

            if (geometry.type == rhi::AccelerationStructureGeometryType::Aabbs) {
                out.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                const BufferRecord *buffer = device.find_buffer_for_build(geometry.aabbs.buffer);
                if (buffer == nullptr) {
                    return invalid_argument("build_acceleration_structures: an AABB geometry names an unknown buffer.");
                }
                out.AABBs.AABBCount = range.primitive_count;
                out.AABBs.AABBs = {.StartAddress = buffer->gpu_address + geometry.aabbs.offset + range.primitive_offset,
                                   .StrideInBytes = geometry.aabbs.stride};
            } else {
                out.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                const rhi::AccelerationStructureTrianglesDesc &triangles = geometry.triangles;
                const BufferRecord *vertex_buffer = device.find_buffer_for_build(triangles.vertex_buffer);
                if (vertex_buffer == nullptr) {
                    return invalid_argument(
                        "build_acceleration_structures: a triangle geometry names an unknown vertex buffer.");
                }
                DXGI_FORMAT vertex_format = DXGI_FORMAT_UNKNOWN;
                if (!to_dxr_vertex_format(triangles.vertex_format, vertex_format)) {
                    return unsupported("build_acceleration_structures: the geometry's vertex format is not a valid "
                                       "DXR position format.");
                }
                out.Triangles.VertexFormat = vertex_format;
                out.Triangles.VertexBuffer = {
                    .StartAddress = vertex_buffer->gpu_address + triangles.vertex_offset +
                                    static_cast<u64>(range.first_vertex) * triangles.vertex_stride,
                    .StrideInBytes = triangles.vertex_stride,
                };
                out.Triangles.VertexCount = triangles.max_vertex + 1;

                if (triangles.index_buffer.is_valid()) {
                    const BufferRecord *index_buffer = device.find_buffer_for_build(triangles.index_buffer);
                    if (index_buffer == nullptr) {
                        return invalid_argument(
                            "build_acceleration_structures: a triangle geometry names an unknown index buffer.");
                    }
                    out.Triangles.IndexFormat = to_dxgi(triangles.index_format);
                    out.Triangles.IndexCount = range.primitive_count * 3;
                    out.Triangles.IndexBuffer =
                        index_buffer->gpu_address + triangles.index_offset + range.primitive_offset;
                } else {
                    out.Triangles.VertexCount = range.primitive_count * 3;
                }

                if (triangles.transform_buffer.is_valid()) {
                    const BufferRecord *transform_buffer = device.find_buffer_for_build(triangles.transform_buffer);
                    if (transform_buffer == nullptr) {
                        return invalid_argument(
                            "build_acceleration_structures: a triangle geometry names an unknown transform buffer.");
                    }
                    out.Triangles.Transform3x4 =
                        transform_buffer->gpu_address + triangles.transform_offset + range.transform_offset;
                }
            }
            geometry_storage.push_back(out);
        }
        inputs.NumDescs = static_cast<UINT>(geometry_storage.size());
        inputs.pGeometryDescs = geometry_storage.empty() ? nullptr : geometry_storage.data();
        return {};
    }

    rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> D3D12Device::acceleration_structure_build_sizes(
        const rhi::AccelerationStructureBuildDesc &desc) const {
        ZoneScopedN("D3D12Device::acceleration_structure_build_sizes");
        if (device5_ == nullptr || !enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            return unsupported("acceleration_structure_build_sizes: acceleration structures are not enabled on this "
                               "device.");
        }

        vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_storage;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        if (auto built = build_acceleration_structure_inputs(*this, desc, geometry_storage, inputs); !built) {
            return std::unexpected(built.error());
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        return rhi::AccelerationStructureBuildSizes{
            .acceleration_structure_size = info.ResultDataMaxSizeInBytes,
            .build_scratch_size = info.ScratchDataSizeInBytes,
            .update_scratch_size = info.UpdateScratchDataSizeInBytes,
        };
    }

    rhi::RhiExpected<rhi::AccelerationStructureHandle> D3D12Device::create_acceleration_structure(
        const rhi::AccelerationStructureDesc &desc) {
        ZoneScopedN("D3D12Device::create_acceleration_structure");
        if (device5_ == nullptr || !enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            return unsupported("create_acceleration_structure: acceleration structures are not enabled on this device.");
        }
        if (desc.size == 0) {
            return invalid_argument("create_acceleration_structure: size must be non-zero.");
        }

        // An acceleration structure is an ordinary buffer that must permit UAV access and must be
        // created directly in the RAYTRACING_ACCELERATION_STRUCTURE state — it can never be
        // transitioned into that state afterwards, which is the one non-obvious rule here.
        const CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resource_desc =
            CD3DX12_RESOURCE_DESC::Buffer(desc.size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        AccelerationStructureRecord record{};
        record.size = desc.size;
        record.type = desc.type;
        if (const HRESULT hr = device_->CreateCommittedResource(
                &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&record.resource));
            FAILED(hr)) {
            return hresult_error(hr, "create_acceleration_structure (CreateCommittedResource)");
        }
        set_debug_name(record.resource.Get(), desc.label);
        record.gpu_address = record.resource->GetGPUVirtualAddress();
        return acceleration_structures_.insert(std::move(record));
    }

    void D3D12Device::destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept {
        acceleration_structures_.erase(handle);
    }

    rhi::RhiExpected<u64> D3D12Device::acceleration_structure_device_address(
        rhi::AccelerationStructureHandle handle) const {
        const AccelerationStructureRecord *record = acceleration_structures_.find(handle);
        if (record == nullptr) {
            return invalid_argument("acceleration_structure_device_address: unknown acceleration structure handle.");
        }
        return static_cast<u64>(record->gpu_address);
    }

} // namespace SFT::D3D12
