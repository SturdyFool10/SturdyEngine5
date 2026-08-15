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
#include <limits>
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

        [[nodiscard]] bool utf8_to_utf16(const char *text, std::wstring &out) {
            if (text == nullptr || *text == '\0') {
                return false;
            }
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
            if (size <= 1) {
                return false;
            }
            out.resize(static_cast<usize>(size));
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out.data(), size) == 0) {
                out.clear();
                return false;
            }
            out.pop_back();
            return true;
        }

        [[nodiscard]] bool is_single_stage(rhi::ShaderStage stage) noexcept {
            const u32 value = static_cast<u32>(stage);
            const u32 known_stages = static_cast<u32>(rhi::ShaderStage::All);
            return value != 0 && (value & (value - 1)) == 0 && (value & ~known_stages) == 0;
        }

        [[nodiscard]] bool is_valid_general_stage(rhi::ShaderStage stage) noexcept {
            return stage == rhi::ShaderStage::RayGeneration || stage == rhi::ShaderStage::Miss ||
                   stage == rhi::ShaderStage::Callable;
        }

        [[nodiscard]] bool checked_add(u64 left, u64 right, u64 &out) noexcept {
            if (right > std::numeric_limits<u64>::max() - left) {
                return false;
            }
            out = left + right;
            return true;
        }

        [[nodiscard]] bool checked_multiply(u64 left, u64 right, u64 &out) noexcept {
            if (left != 0 && right > std::numeric_limits<u64>::max() / left) {
                return false;
            }
            out = left * right;
            return true;
        }

        [[nodiscard]] bool strided_data_fits(u64 buffer_size, u64 offset, u64 element_count, u64 stride, u64 element_size) noexcept {
            if (offset > buffer_size) {
                return false;
            }
            if (element_count == 0) {
                return true;
            }
            const u64 remaining = buffer_size - offset;
            if (stride == 0 || element_size > remaining) {
                return false;
            }
            return element_count - 1 <= (remaining - element_size) / stride;
        }

        // Vertex position format for a BLAS triangle geometry. DXR accepts only a small set here, so an
        // unsupported one is reported rather than silently reinterpreted. The byte metadata is also
        // used to validate the address, stride, and source-buffer range before passing them to DXR.
        [[nodiscard]] bool to_dxr_vertex_format(rhi::VertexFormat format, DXGI_FORMAT &out, u32 &size, u32 &alignment) noexcept {
            switch (format) {
                case rhi::VertexFormat::Float32x3:
                    out = DXGI_FORMAT_R32G32B32_FLOAT;
                    size = 12;
                    alignment = 4;
                    return true;
                case rhi::VertexFormat::Float32x2:
                    out = DXGI_FORMAT_R32G32_FLOAT;
                    size = 8;
                    alignment = 4;
                    return true;
                case rhi::VertexFormat::Float16x2:
                    out = DXGI_FORMAT_R16G16_FLOAT;
                    size = 4;
                    alignment = 2;
                    return true;
                case rhi::VertexFormat::Float16x4:
                    out = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    size = 8;
                    alignment = 2;
                    return true;
                case rhi::VertexFormat::Uint16x2Unorm:
                    out = DXGI_FORMAT_R16G16_UNORM;
                    size = 4;
                    alignment = 2;
                    return true;
                case rhi::VertexFormat::Uint16x4Unorm:
                    out = DXGI_FORMAT_R16G16B16A16_UNORM;
                    size = 8;
                    alignment = 2;
                    return true;
                default:
                    return false;
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

        if (desc.groups.empty()) {
            return invalid_argument("create_ray_tracing_pipeline: at least one shader group is required.");
        }

        RayTracingPipelineRecord record{};
        record.layout = desc.layout;
        // Hit-group subobjects retain pointers to these strings until CreateStateObject. Reserving the
        // final size prevents vector growth from invalidating an earlier group's export pointer.
        record.group_exports.reserve(desc.groups.size());

        CD3DX12_STATE_OBJECT_DESC state_object(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
        // Every subobject the helper hands out points into storage owned by `state_object`, so all of
        // these stay alive until CreateStateObject is called below; the export-name strings, however,
        // are ours and must outlive the call too — hence the retained vector rather than temporaries.
        vector<std::wstring> stage_exports;
        vector<std::wstring> source_exports;
        stage_exports.reserve(desc.groups.size() * 3);
        source_exports.reserve(desc.groups.size() * 3);

        // Adds one DXIL library exporting `entry`'s actual entry point under a fresh unique name.
        const auto add_stage = [&](const rhi::ShaderEntry &entry, rhi::ShaderStage implied_stage, const char *prefix, usize index) -> rhi::RhiExpected<const std::wstring *> {
            if (!entry.module.is_valid()) {
                return static_cast<const std::wstring *>(nullptr);
            }

            const rhi::ShaderStage stage = entry.stage == rhi::ShaderStage::None ? implied_stage : entry.stage;
            if (!is_single_stage(stage)) {
                return invalid_argument("create_ray_tracing_pipeline: shader entry must name exactly one stage.");
            }
            if (implied_stage != rhi::ShaderStage::None && stage != implied_stage) {
                return invalid_argument(
                    "create_ray_tracing_pipeline: shader entry stage does not match its group field.");
            }

            const ShaderModuleRecord *module = shader_modules_.find(entry.module);
            if (module == nullptr) {
                return invalid_argument("create_ray_tracing_pipeline: a shader group names an unknown module.");
            }
            std::wstring source_export;
            if (!utf8_to_utf16(entry.entry_point, source_export)) {
                return invalid_argument(
                    "create_ray_tracing_pipeline: shader entry point must be non-empty valid UTF-8.");
            }

            stage_exports.push_back(export_name(prefix, index));
            source_exports.push_back(std::move(source_export));
            const std::wstring &name = stage_exports.back();

            auto *library = state_object.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
            const D3D12_SHADER_BYTECODE bytecode{module->bytecode.data(), module->bytecode.size()};
            library->SetDXILLibrary(&bytecode);
            // DXR's two-name export form renames the library's UTF-16 source symbol to the generated
            // state-object-wide unique name. Both strings remain alive through CreateStateObject.
            library->DefineExport(name.c_str(), source_exports.back().c_str(), D3D12_EXPORT_FLAG_NONE);
            return &name;
        };

        for (usize index = 0; index < desc.groups.size(); ++index) {
            const rhi::RayTracingShaderGroupDesc &group = desc.groups[index];
            if (group.type != rhi::RayTracingShaderGroupType::General &&
                group.type != rhi::RayTracingShaderGroupType::TrianglesHitGroup &&
                group.type != rhi::RayTracingShaderGroupType::ProceduralHitGroup) {
                return invalid_argument("create_ray_tracing_pipeline: shader group has an invalid type.");
            }

            if (group.type == rhi::RayTracingShaderGroupType::General) {
                if (!group.general.module.is_valid()) {
                    return invalid_argument(
                        "create_ray_tracing_pipeline: general shader group requires a general shader.");
                }
                if (!is_valid_general_stage(group.general.stage)) {
                    return invalid_argument("create_ray_tracing_pipeline: general shader stage must be RayGeneration, "
                                            "Miss, or Callable.");
                }
                if (group.closest_hit.module.is_valid() || group.any_hit.module.is_valid() ||
                    group.intersection.module.is_valid()) {
                    return invalid_argument(
                        "create_ray_tracing_pipeline: general shader group cannot contain hit shaders.");
                }

                auto general = add_stage(group.general, rhi::ShaderStage::None, "sturdy_general_", index);
                if (!general) {
                    return std::unexpected(general.error());
                }
                // A general group *is* its export — raygen/miss/callable are addressed by the entry
                // point's own identifier, with no hit-group wrapper.
                record.group_exports.push_back(**general);
                continue;
            }

            if (group.general.module.is_valid()) {
                return invalid_argument("create_ray_tracing_pipeline: hit group cannot contain a general shader.");
            }
            if (group.type == rhi::RayTracingShaderGroupType::TrianglesHitGroup &&
                group.intersection.module.is_valid()) {
                return invalid_argument(
                    "create_ray_tracing_pipeline: triangle hit group cannot contain an intersection shader.");
            }
            if (group.type == rhi::RayTracingShaderGroupType::ProceduralHitGroup &&
                !group.intersection.module.is_valid()) {
                return invalid_argument(
                    "create_ray_tracing_pipeline: procedural hit group requires an intersection shader.");
            }

            auto closest_hit =
                add_stage(group.closest_hit, rhi::ShaderStage::ClosestHit, "sturdy_closesthit_", index);
            if (!closest_hit) {
                return std::unexpected(closest_hit.error());
            }
            auto any_hit = add_stage(group.any_hit, rhi::ShaderStage::AnyHit, "sturdy_anyhit_", index);
            if (!any_hit) {
                return std::unexpected(any_hit.error());
            }
            auto intersection =
                add_stage(group.intersection, rhi::ShaderStage::Intersection, "sturdy_intersection_", index);
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
        if (first_group > record->group_exports.size() ||
            group_count > record->group_exports.size() - first_group) {
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
        geometry_storage.clear();
        switch (desc.type) {
            case rhi::AccelerationStructureType::BottomLevel:
                inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                break;
            case rhi::AccelerationStructureType::TopLevel:
                inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                break;
            default:
                return invalid_argument("build_acceleration_structures: acceleration structure type is invalid.");
        }
        inputs.Flags = to_d3d12(desc.flags);
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        const auto input_buffer = [&](rhi::BufferHandle handle,
                                      const char *role) -> rhi::RhiExpected<const BufferRecord *> {
            const BufferRecord *buffer = device.find_buffer_for_build(handle);
            if (buffer == nullptr) {
                return invalid_argument(std::string("build_acceleration_structures: the ") + role +
                                        " handle is unknown.");
            }
            if (!rhi::has_any(buffer->usage, rhi::BufferUsage::AccelerationStructureInput)) {
                return invalid_argument(std::string("build_acceleration_structures: the ") + role +
                                        " was not created for acceleration-structure input.");
            }
            return buffer;
        };

        if (desc.type == rhi::AccelerationStructureType::TopLevel) {
            if (desc.geometries.size() != 1 ||
                desc.geometries.front().type != rhi::AccelerationStructureGeometryType::Instances) {
                return invalid_argument(
                    "build_acceleration_structures: a top-level build requires exactly one instance geometry.");
            }
            if (desc.ranges.size() != 1) {
                return invalid_argument(
                    "build_acceleration_structures: a top-level build requires exactly one build range.");
            }

            // A TLAS takes instance records by GPU address rather than a geometry array. The RHI's
            // AccelerationStructureInstance is a 64-byte record laid out to match
            // D3D12_RAYTRACING_INSTANCE_DESC bit-for-bit (row-major 3x4 transform, packed 24-bit index
            // + 8-bit mask, packed 24-bit SBT offset + 8-bit flags, then the BLAS address), which is
            // why the buffer is handed straight to D3D12 with no repacking.
            static_assert(sizeof(rhi::AccelerationStructureInstance) == sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            const rhi::AccelerationStructureInstancesDesc &instances = desc.geometries.front().instances;
            const rhi::AccelerationStructureBuildRangeInfo &range = desc.ranges.front();
            if (instances.array_of_pointers) {
                return unsupported("build_acceleration_structures: D3D12 top-level builds do not accept an "
                                   "array-of-pointers instance layout.");
            }
            if (range.first_vertex != 0 || range.transform_offset != 0) {
                return invalid_argument(
                    "build_acceleration_structures: an instance build range cannot use vertex or transform offsets.");
            }

            auto found = input_buffer(instances.buffer, "instance buffer");
            if (!found) {
                return std::unexpected(found.error());
            }
            const BufferRecord *buffer = *found;
            u64 instance_offset = 0;
            if (!checked_add(instances.offset, range.primitive_offset, instance_offset) ||
                !strided_data_fits(buffer->size, instance_offset, range.primitive_count, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), sizeof(D3D12_RAYTRACING_INSTANCE_DESC))) {
                return invalid_argument(
                    "build_acceleration_structures: the instance build range exceeds its buffer.");
            }
            const D3D12_GPU_VIRTUAL_ADDRESS instance_address = buffer->gpu_address + instance_offset;
            if (instance_address % D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT != 0) {
                return invalid_argument(
                    "build_acceleration_structures: the instance data address is not correctly aligned.");
            }

            inputs.NumDescs = range.primitive_count;
            inputs.InstanceDescs = instance_address;
            return {};
        }

        if (desc.geometries.empty()) {
            return invalid_argument("build_acceleration_structures: a bottom-level build requires geometry.");
        }
        if (desc.geometries.size() != desc.ranges.size()) {
            return invalid_argument(
                "build_acceleration_structures: bottom-level geometry and range counts must match.");
        }
        if (desc.geometries.size() > std::numeric_limits<UINT>::max()) {
            return invalid_argument("build_acceleration_structures: the geometry count exceeds D3D12 limits.");
        }

        geometry_storage.reserve(desc.geometries.size());
        for (usize index = 0; index < desc.geometries.size(); ++index) {
            const rhi::AccelerationStructureGeometryDesc &geometry = desc.geometries[index];
            const rhi::AccelerationStructureBuildRangeInfo &range = desc.ranges[index];

            D3D12_RAYTRACING_GEOMETRY_DESC out{};
            out.Flags = to_d3d12(geometry.flags);

            switch (geometry.type) {
                case rhi::AccelerationStructureGeometryType::Aabbs:
                    {
                        if (range.first_vertex != 0 || range.transform_offset != 0) {
                            return invalid_argument("build_acceleration_structures: an AABB build range cannot use vertex "
                                                    "or transform offsets.");
                        }
                        if (geometry.aabbs.stride < sizeof(D3D12_RAYTRACING_AABB) ||
                            geometry.aabbs.stride % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT != 0) {
                            return invalid_argument(
                                "build_acceleration_structures: an AABB stride is too small or incorrectly aligned.");
                        }

                        auto found = input_buffer(geometry.aabbs.buffer, "AABB buffer");
                        if (!found) {
                            return std::unexpected(found.error());
                        }
                        const BufferRecord *buffer = *found;
                        u64 aabb_offset = 0;
                        if (!checked_add(geometry.aabbs.offset, range.primitive_offset, aabb_offset) ||
                            !strided_data_fits(buffer->size, aabb_offset, range.primitive_count, geometry.aabbs.stride, sizeof(D3D12_RAYTRACING_AABB))) {
                            return invalid_argument(
                                "build_acceleration_structures: the AABB build range exceeds its buffer.");
                        }
                        const D3D12_GPU_VIRTUAL_ADDRESS aabb_address = buffer->gpu_address + aabb_offset;
                        if (aabb_address % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT != 0) {
                            return invalid_argument(
                                "build_acceleration_structures: the AABB data address is not correctly aligned.");
                        }

                        out.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                        out.AABBs.AABBCount = range.primitive_count;
                        out.AABBs.AABBs = {
                            .StartAddress = aabb_address,
                            .StrideInBytes = geometry.aabbs.stride,
                        };
                        break;
                    }
                case rhi::AccelerationStructureGeometryType::Triangles:
                    {
                        const rhi::AccelerationStructureTrianglesDesc &triangles = geometry.triangles;
                        auto found_vertices = input_buffer(triangles.vertex_buffer, "vertex buffer");
                        if (!found_vertices) {
                            return std::unexpected(found_vertices.error());
                        }
                        const BufferRecord *vertex_buffer = *found_vertices;

                        DXGI_FORMAT vertex_format = DXGI_FORMAT_UNKNOWN;
                        u32 vertex_size = 0;
                        u32 vertex_alignment = 0;
                        if (!to_dxr_vertex_format(triangles.vertex_format, vertex_format, vertex_size, vertex_alignment)) {
                            return unsupported("build_acceleration_structures: the geometry's vertex format is not a valid "
                                               "DXR position format.");
                        }
                        if (triangles.vertex_stride < vertex_size ||
                            triangles.vertex_stride % vertex_alignment != 0) {
                            return invalid_argument("build_acceleration_structures: the triangle vertex stride is too small "
                                                    "or incorrectly aligned.");
                        }
                        if (range.primitive_count > std::numeric_limits<u32>::max() / 3) {
                            return invalid_argument(
                                "build_acceleration_structures: the triangle primitive count exceeds D3D12 limits.");
                        }

                        const bool indexed = triangles.index_buffer.is_valid();
                        u64 vertex_range_offset = range.primitive_offset;
                        if (indexed) {
                            if (!checked_multiply(range.first_vertex, triangles.vertex_stride, vertex_range_offset)) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle first-vertex offset overflows.");
                            }
                        } else if (range.first_vertex != 0) {
                            return invalid_argument(
                                "build_acceleration_structures: a non-indexed triangle range cannot use first_vertex.");
                        }

                        u64 vertex_data_offset = 0;
                        if (!checked_add(triangles.vertex_offset, vertex_range_offset, vertex_data_offset)) {
                            return invalid_argument(
                                "build_acceleration_structures: the triangle vertex offset overflows.");
                        }
                        const D3D12_GPU_VIRTUAL_ADDRESS vertex_address =
                            vertex_buffer->gpu_address + vertex_data_offset;
                        if (vertex_address % vertex_alignment != 0) {
                            return invalid_argument(
                                "build_acceleration_structures: the triangle vertex address is not correctly aligned.");
                        }

                        u32 vertex_count = range.primitive_count * 3;
                        if (indexed) {
                            if (triangles.max_vertex == std::numeric_limits<u32>::max()) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle max vertex exceeds D3D12 limits.");
                            }
                            vertex_count = triangles.max_vertex + 1;
                        }
                        if (!strided_data_fits(vertex_buffer->size, vertex_data_offset, vertex_count, triangles.vertex_stride, vertex_size)) {
                            return invalid_argument(
                                "build_acceleration_structures: the triangle vertex range exceeds its buffer.");
                        }

                        out.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                        out.Triangles.VertexFormat = vertex_format;
                        out.Triangles.VertexBuffer = {
                            .StartAddress = vertex_address,
                            .StrideInBytes = triangles.vertex_stride,
                        };
                        out.Triangles.VertexCount = vertex_count;

                        if (indexed) {
                            u64 index_size = 0;
                            switch (triangles.index_format) {
                                case rhi::IndexFormat::Uint16:
                                    index_size = sizeof(u16);
                                    break;
                                case rhi::IndexFormat::Uint32:
                                    index_size = sizeof(u32);
                                    break;
                                default:
                                    return invalid_argument(
                                        "build_acceleration_structures: the triangle index format is invalid.");
                            }

                            auto found_indices = input_buffer(triangles.index_buffer, "index buffer");
                            if (!found_indices) {
                                return std::unexpected(found_indices.error());
                            }
                            const BufferRecord *index_buffer = *found_indices;
                            u64 index_data_offset = 0;
                            if (!checked_add(triangles.index_offset, range.primitive_offset, index_data_offset) ||
                                !strided_data_fits(index_buffer->size, index_data_offset, range.primitive_count * 3, index_size, index_size)) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle index range exceeds its buffer.");
                            }
                            const D3D12_GPU_VIRTUAL_ADDRESS index_address =
                                index_buffer->gpu_address + index_data_offset;
                            if (index_address % index_size != 0) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle index address is not correctly aligned.");
                            }

                            out.Triangles.IndexFormat = to_dxgi(triangles.index_format);
                            out.Triangles.IndexCount = range.primitive_count * 3;
                            out.Triangles.IndexBuffer = index_address;
                        }

                        if (triangles.transform_buffer.is_valid()) {
                            auto found_transform = input_buffer(triangles.transform_buffer, "transform buffer");
                            if (!found_transform) {
                                return std::unexpected(found_transform.error());
                            }
                            const BufferRecord *transform_buffer = *found_transform;
                            u64 transform_offset = 0;
                            constexpr u64 transform_size = sizeof(f32) * 12;
                            if (!checked_add(triangles.transform_offset, range.transform_offset, transform_offset) ||
                                !strided_data_fits(transform_buffer->size, transform_offset, 1, transform_size, transform_size)) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle transform exceeds its buffer.");
                            }
                            const D3D12_GPU_VIRTUAL_ADDRESS transform_address =
                                transform_buffer->gpu_address + transform_offset;
                            if (transform_address % D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT != 0) {
                                return invalid_argument(
                                    "build_acceleration_structures: the triangle transform address is not correctly aligned.");
                            }
                            out.Triangles.Transform3x4 = transform_address;
                        } else if (triangles.transform_offset != 0 || range.transform_offset != 0) {
                            return invalid_argument("build_acceleration_structures: triangle transform offsets require a "
                                                    "transform buffer.");
                        }
                        break;
                    }
                case rhi::AccelerationStructureGeometryType::Instances:
                    return invalid_argument(
                        "build_acceleration_structures: a bottom-level build cannot contain instance geometry.");
                default:
                    return invalid_argument("build_acceleration_structures: geometry type is invalid.");
            }
            geometry_storage.push_back(out);
        }
        inputs.NumDescs = static_cast<UINT>(geometry_storage.size());
        inputs.pGeometryDescs = geometry_storage.data();
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
        if (desc.type != rhi::AccelerationStructureType::BottomLevel &&
            desc.type != rhi::AccelerationStructureType::TopLevel) {
            return invalid_argument("create_acceleration_structure: acceleration structure type is invalid.");
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
