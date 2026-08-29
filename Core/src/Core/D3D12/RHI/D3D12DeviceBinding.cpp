

#include <Core/D3D12/RHI/D3D12Device.hpp>

#pragma region Imports
#include <Core/D3D12/RHI/D3D12Convert.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {


        enum class RegisterClass : u32 { Cbv, Srv, Uav, Sampler };

        /// Registers class of using the supplied arguments and current state.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RegisterClass register_class_of(rhi::BindingType type) noexcept {
            switch (type) {
                case rhi::BindingType::UniformBuffer: return RegisterClass::Cbv;
                case rhi::BindingType::StorageBuffer:
                case rhi::BindingType::StorageTexture:
                    return RegisterClass::Uav;
                case rhi::BindingType::Sampler: return RegisterClass::Sampler;
                case rhi::BindingType::ReadOnlyStorageBuffer:
                case rhi::BindingType::SampledTexture:
                case rhi::BindingType::AccelerationStructure:


                case rhi::BindingType::InputAttachment:
                    return RegisterClass::Srv;
                case rhi::BindingType::CombinedImageSampler:
                    return RegisterClass::Srv;
            }
            return RegisterClass::Srv;
        }

        /// Converts the value to range type representation.
        ///
        /// @param klass `klass` value used by the operation.
        ///
        /// @return Returns the value converted to range type representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_DESCRIPTOR_RANGE_TYPE to_range_type(RegisterClass klass) noexcept {
            switch (klass) {
                case RegisterClass::Cbv: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                case RegisterClass::Uav: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                case RegisterClass::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                case RegisterClass::Srv: break;
            }
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        }

        /// Binds the supplied resource or state for subsequent operations.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool binding_uses_sampler(rhi::BindingType type) noexcept {
            return type == rhi::BindingType::Sampler || type == rhi::BindingType::CombinedImageSampler;
        }

        /// Binds the supplied resource or state for subsequent operations.
        ///
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool binding_uses_resource_descriptor(rhi::BindingType type) noexcept {
            return type != rhi::BindingType::Sampler;
        }

        struct BufferDescriptorRange {
            UINT first_element = 0;
            UINT element_count = 0;
            UINT structure_stride = 0;

            /// Reports whether raw holds for this `BufferDescriptorRange`.
            ///
            /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] bool is_raw() const noexcept { return structure_stride == 0; }
        };

        /// Performs the buffer descriptor range operation for `D3D12` using the supplied arguments.
        ///
        /// @param buffer Buffer used or affected by the operation.
        /// @param entry `entry` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<BufferDescriptorRange> buffer_descriptor_range(
            const BufferRecord &buffer,
            const rhi::BindGroupEntry &entry) {
            if (entry.offset > buffer.size ||
                (entry.size != 0 && entry.size > buffer.size - entry.offset)) {
                return invalid_argument("create_bind_group: storage binding range exceeds its buffer.");
            }

            const u64 structure_stride = entry.structure_stride;
            if (structure_stride != 0 &&
                (structure_stride % sizeof(u32) != 0 ||
                 structure_stride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES)) {
                return invalid_argument(
                    "create_bind_group: a structured-buffer stride must be a four-byte multiple no larger than " +
                    std::to_string(D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES) + " bytes.");
            }

            const u64 element_stride = structure_stride != 0 ? structure_stride : sizeof(u32);
            const u64 size = entry.size != 0 ? entry.size : buffer.size - entry.offset;
            if (entry.offset % element_stride != 0 || size == 0 || size % element_stride != 0) {
                return invalid_argument(
                    "create_bind_group: storage binding offset and size must align to its element stride.");
            }

            const u64 first_element = entry.offset / element_stride;
            const u64 element_count = size / element_stride;
            if (first_element > std::numeric_limits<UINT>::max() ||
                element_count > std::numeric_limits<UINT>::max()) {
                return invalid_argument("create_bind_group: storage binding range has too many elements for D3D12.");
            }

            return BufferDescriptorRange{
                .first_element = static_cast<UINT>(first_element),
                .element_count = static_cast<UINT>(element_count),
                .structure_stride = static_cast<UINT>(structure_stride),
            };
        }


        /// Converts the value to range flags representation.
        ///
        /// @param flags Flags controlling optional behavior.
        /// @param is_sampler Sampler used or affected by the operation.
        ///
        /// @return Returns the value converted to range flags representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_DESCRIPTOR_RANGE_FLAGS to_range_flags(rhi::BindingFlags flags,
                                                                   bool is_sampler) noexcept {
            D3D12_DESCRIPTOR_RANGE_FLAGS result = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
            if (rhi::has_any(flags, rhi::BindingFlags::UpdateAfterBind)) {
                result |= is_sampler ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
                                     : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
            }
            if (rhi::has_any(flags, rhi::BindingFlags::PartiallyBound)) {
                result |= D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            }
            return result;
        }

    } // namespace


    /// Creates a bind group layout from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BindGroupLayoutHandle> D3D12Device::create_bind_group_layout(
        const rhi::BindGroupLayoutDesc &desc) {
        ZoneScopedN("D3D12Device::create_bind_group_layout");

        BindGroupLayoutRecord record{};
        record.entries.assign(desc.entries.begin(), desc.entries.end());


        std::ranges::sort(record.entries, [](const rhi::BindGroupLayoutEntry &a,
                                             const rhi::BindGroupLayoutEntry &b) { return a.binding < b.binding; });

        for (const rhi::BindGroupLayoutEntry &entry : record.entries) {
            const u32 count = std::max(1u, entry.count);
            const u32 shader_register = entry.shader_register == ~0u
                                            ? entry.binding
                                            : entry.shader_register;

            if (entry.has_dynamic_offset) {
                if (entry.type != rhi::BindingType::UniformBuffer &&
                    entry.type != rhi::BindingType::StorageBuffer &&
                    entry.type != rhi::BindingType::ReadOnlyStorageBuffer) {
                    return invalid_argument(
                        "create_bind_group_layout: has_dynamic_offset is only meaningful for a buffer binding.");
                }
                if (count != 1) {


                    return unsupported(
                        "create_bind_group_layout: a dynamic-offset binding cannot be an array binding on D3D12.");
                }
                record.dynamic_slots.push_back(DynamicSlot{
                    .binding = entry.binding,
                    .shader_register = shader_register,
                    .type = entry.type,
                    .visibility = entry.visibility,
                });
                continue;
            }

            if (binding_uses_resource_descriptor(entry.type)) {
                TableSlot slot{};
                slot.binding = entry.binding;
                slot.shader_register = shader_register;
                slot.table_offset = record.resource_descriptor_count;
                slot.count = count;
                slot.type = entry.type;
                slot.visibility = entry.visibility;
                slot.flags = entry.flags;
                if (rhi::has_any(entry.flags, rhi::BindingFlags::VariableDescriptorCount)) {
                    record.variable_slot_index = static_cast<u32>(record.resource_slots.size());
                }
                record.resource_slots.push_back(slot);
                record.resource_descriptor_count += count;
            }

            if (binding_uses_sampler(entry.type)) {
                TableSlot slot{};
                slot.binding = entry.binding;
                slot.shader_register = shader_register;
                slot.table_offset = record.sampler_descriptor_count;
                slot.count = count;
                slot.type = entry.type;
                slot.visibility = entry.visibility;
                slot.flags = entry.flags;
                record.sampler_slots.push_back(slot);
                record.sampler_descriptor_count += count;
            }
        }

        if (record.variable_slot_index != ~0u &&
            record.variable_slot_index + 1 != record.resource_slots.size()) {
            return invalid_argument(
                "create_bind_group_layout: a VariableDescriptorCount binding must be the last binding in its set.");
        }

        return bind_group_layouts_.insert(std::move(record));
    }

    /// Destroys the bind group layout identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_bind_group_layout(rhi::BindGroupLayoutHandle handle) noexcept {
        bind_group_layouts_.erase(handle);
    }


    /// Creates a pipeline layout from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::PipelineLayoutHandle> D3D12Device::create_pipeline_layout(
        const rhi::PipelineLayoutDesc &desc) {
        ZoneScopedN("D3D12Device::create_pipeline_layout");

        PipelineLayoutRecord record{};
        record.set_layouts.assign(desc.bind_group_layouts.begin(), desc.bind_group_layouts.end());
        record.sets.resize(record.set_layouts.size());

        vector<CD3DX12_ROOT_PARAMETER1> parameters;


        vector<vector<CD3DX12_DESCRIPTOR_RANGE1>> range_storage;
        range_storage.reserve(record.set_layouts.size() * 2);

        for (usize set_index = 0; set_index < record.set_layouts.size(); ++set_index) {
            const BindGroupLayoutRecord *layout = bind_group_layouts_.find(record.set_layouts[set_index]);
            if (layout == nullptr) {
                return invalid_argument("create_pipeline_layout: unknown bind group layout handle at set " +
                                        std::to_string(set_index) + ".");
            }
            const UINT space = static_cast<UINT>(set_index);
            SetRootParameters &mapping = record.sets[set_index];


            for (const DynamicSlot &slot : layout->dynamic_slots) {
                CD3DX12_ROOT_PARAMETER1 parameter{};
                const D3D12_SHADER_VISIBILITY visibility = to_d3d12_visibility(slot.visibility);
                switch (register_class_of(slot.type)) {
                    case RegisterClass::Cbv:
                        parameter.InitAsConstantBufferView(slot.shader_register, space,
                                                           D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
                                                           visibility);
                        break;
                    case RegisterClass::Uav:
                        parameter.InitAsUnorderedAccessView(slot.shader_register, space,
                                                            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE, visibility);
                        break;
                    default:
                        parameter.InitAsShaderResourceView(slot.shader_register, space,
                                                           D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
                                                           visibility);
                        break;
                }
                mapping.dynamic_root_parameters.push_back(static_cast<i32>(parameters.size()));
                parameters.push_back(parameter);
            }

            if (!layout->resource_slots.empty()) {
                vector<CD3DX12_DESCRIPTOR_RANGE1> ranges;
                ranges.reserve(layout->resource_slots.size());
                rhi::ShaderStage visibility_union = rhi::ShaderStage::None;
                for (const TableSlot &slot : layout->resource_slots) {
                    CD3DX12_DESCRIPTOR_RANGE1 range{};
                    range.Init(to_range_type(register_class_of(slot.type)), slot.count, slot.shader_register, space,
                               to_range_flags(slot.flags, false), slot.table_offset);
                    ranges.push_back(range);
                    visibility_union |= slot.visibility;
                }
                range_storage.push_back(std::move(ranges));
                CD3DX12_ROOT_PARAMETER1 parameter{};
                parameter.InitAsDescriptorTable(static_cast<UINT>(range_storage.back().size()),
                                                range_storage.back().data(),
                                                to_d3d12_visibility(visibility_union));
                mapping.resource_table = static_cast<i32>(parameters.size());
                parameters.push_back(parameter);
            }

            if (!layout->sampler_slots.empty()) {
                vector<CD3DX12_DESCRIPTOR_RANGE1> ranges;
                ranges.reserve(layout->sampler_slots.size());
                rhi::ShaderStage visibility_union = rhi::ShaderStage::None;
                for (const TableSlot &slot : layout->sampler_slots) {
                    CD3DX12_DESCRIPTOR_RANGE1 range{};
                    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, slot.count, slot.shader_register, space,
                               to_range_flags(slot.flags, true), slot.table_offset);
                    ranges.push_back(range);
                    visibility_union |= slot.visibility;
                }
                range_storage.push_back(std::move(ranges));
                CD3DX12_ROOT_PARAMETER1 parameter{};
                parameter.InitAsDescriptorTable(static_cast<UINT>(range_storage.back().size()),
                                                range_storage.back().data(),
                                                to_d3d12_visibility(visibility_union));
                mapping.sampler_table = static_cast<i32>(parameters.size());
                parameters.push_back(parameter);
            }
        }


        if (!desc.push_constant_ranges.empty()) {
            u32 end_bytes = 0;
            rhi::ShaderStage stages = rhi::ShaderStage::None;
            for (const rhi::PushConstantRange &range : desc.push_constant_ranges) {
                end_bytes = std::max(end_bytes, range.offset + range.size);
                stages |= range.stages;
            }
            if (end_bytes % 4 != 0) {
                return invalid_argument("create_pipeline_layout: push constant ranges must be 4-byte sized/aligned.");
            }
            if (end_bytes > limits_.max_push_constants_size) {
                return unsupported("create_pipeline_layout: push constants of " + std::to_string(end_bytes) +
                                   " bytes exceed this device's " +
                                   std::to_string(limits_.max_push_constants_size) + "-byte budget.");
            }
            record.push_constant_values = end_bytes / 4;
            CD3DX12_ROOT_PARAMETER1 parameter{};

            // Must land at the exact register Slang assigned the push-constant cbuffer for this
            // shader's DXIL target — D3D12 has no dedicated push-constant mechanism, so Slang lowers
            // it to an ordinary cbuffer wherever its whole-program layout puts it, which is not
            // reliably b0 once other constant buffers share the program. Every range here describes
            // the same buffer (this engine only ever has one push-constant block per shader), so the
            // first entry's register is authoritative.
            parameter.InitAsConstants(record.push_constant_values, desc.push_constant_ranges[0].shader_register,
                                      desc.push_constant_ranges[0].register_space, to_d3d12_visibility(stages));
            record.push_constant_root_parameter = static_cast<i32>(parameters.size());
            parameters.push_back(parameter);
        }


        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC root_signature_desc{};
        root_signature_desc.Init_1_1(static_cast<UINT>(parameters.size()),
                                     parameters.empty() ? nullptr : parameters.data(), 0, nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);


        D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature_support{D3D_ROOT_SIGNATURE_VERSION_1_1};
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root_signature_support,
                                                sizeof(root_signature_support)))) {
            root_signature_support.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        if (const HRESULT hr = D3DX12SerializeVersionedRootSignature(
                &root_signature_desc, root_signature_support.HighestVersion, &blob, &error);
            FAILED(hr)) {
            std::string message = "create_pipeline_layout (SerializeVersionedRootSignature) failed: " +
                                  hresult_name(hr) + ".";
            if (error != nullptr && error->GetBufferSize() > 0) {
                message += " ";
                message.append(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize());
            }
            return operation_failed(std::move(message));
        }
        if (const HRESULT hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                             IID_PPV_ARGS(&record.root_signature));
            FAILED(hr)) {
            return hresult_error(hr, "create_pipeline_layout (CreateRootSignature)");
        }
        set_debug_name(record.root_signature.Get(), desc.label);


        record.root_signature_content_hash =
            fnv1a_bytes(fnv1a_offset_basis, blob->GetBufferPointer(), blob->GetBufferSize());

        return pipeline_layouts_.insert(std::move(record));
    }

    /// Destroys the pipeline layout identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_pipeline_layout(rhi::PipelineLayoutHandle handle) noexcept {
        pipeline_layouts_.erase(handle);
    }


    /// Creates a bind group from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::BindGroupHandle> D3D12Device::create_bind_group(const rhi::BindGroupDesc &desc) {
        ZoneScopedN("D3D12Device::create_bind_group");

        const BindGroupLayoutRecord *layout = bind_group_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return invalid_argument("create_bind_group: unknown bind group layout handle.");
        }

        u32 resource_count = layout->resource_descriptor_count;
        if (layout->variable_slot_index != ~0u && desc.variable_descriptor_count != 0) {
            const TableSlot &variable = layout->resource_slots[layout->variable_slot_index];
            if (desc.variable_descriptor_count > variable.count) {
                return invalid_argument(
                    "create_bind_group: variable_descriptor_count exceeds the layout's declared maximum.");
            }


            resource_count = variable.table_offset + desc.variable_descriptor_count;
        }

        BindGroupRecord record{};
        record.layout = desc.layout;
        record.dynamic_addresses.assign(layout->dynamic_slots.size(), 0);

        struct Rollback {
            D3D12Device &device;
            BindGroupRecord &record;
            bool committed = false;
            ~Rollback() {
                if (!committed) {
                    device.release_bind_group_descriptors(record);
                }
            }
        } rollback{*this, record};

        if (resource_count > 0) {
            auto range = cpu_resource_descriptors_.allocate(resource_count);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.resources = *range;
        }
        if (layout->sampler_descriptor_count > 0) {
            auto range = cpu_sampler_descriptors_.allocate(layout->sampler_descriptor_count);
            if (!range) {
                return std::unexpected(range.error());
            }
            record.samplers = *range;
        }


        vector<D3D12_CPU_DESCRIPTOR_HANDLE> resource_copy_dests;
        vector<D3D12_CPU_DESCRIPTOR_HANDLE> resource_copy_srcs;
        vector<D3D12_CPU_DESCRIPTOR_HANDLE> sampler_copy_dests;
        vector<D3D12_CPU_DESCRIPTOR_HANDLE> sampler_copy_srcs;

        for (const rhi::BindGroupEntry &entry : desc.entries) {
            const auto dynamic_it = std::ranges::find_if(
                layout->dynamic_slots, [&](const DynamicSlot &slot) { return slot.binding == entry.binding; });
            if (dynamic_it != layout->dynamic_slots.end()) {
                const BufferRecord *buffer = buffers_.find(entry.buffer);
                if (buffer == nullptr) {
                    return invalid_argument("create_bind_group: dynamic-offset binding " +
                                            std::to_string(entry.binding) + " names an unknown buffer.");
                }
                const usize slot_index =
                    static_cast<usize>(std::distance(layout->dynamic_slots.begin(), dynamic_it));
                record.dynamic_addresses[slot_index] = buffer->gpu_address + entry.offset;
                continue;
            }

            const auto resource_it = std::ranges::find_if(
                layout->resource_slots, [&](const TableSlot &slot) { return slot.binding == entry.binding; });
            const auto sampler_it = std::ranges::find_if(
                layout->sampler_slots, [&](const TableSlot &slot) { return slot.binding == entry.binding; });
            if (resource_it == layout->resource_slots.end() && sampler_it == layout->sampler_slots.end()) {
                return invalid_argument("create_bind_group: binding " + std::to_string(entry.binding) +
                                        " is not declared by this layout.");
            }

            if (resource_it != layout->resource_slots.end()) {
                if (entry.array_element >= resource_it->count) {
                    return invalid_argument("create_bind_group: array_element is out of range for binding " +
                                            std::to_string(entry.binding) + ".");
                }
                const u32 index = resource_it->table_offset + entry.array_element;
                if (index >= resource_count) {
                    return invalid_argument(
                        "create_bind_group: binding " + std::to_string(entry.binding) +
                        " writes past the descriptor table (variable_descriptor_count is too small).");
                }
                const D3D12_CPU_DESCRIPTOR_HANDLE destination =
                    cpu_resource_descriptors_.cpu_handle(record.resources, index);

                switch (resource_it->type) {
                    case rhi::BindingType::UniformBuffer: {
                        const BufferRecord *buffer = buffers_.find(entry.buffer);
                        if (buffer == nullptr) {
                            return invalid_argument("create_bind_group: uniform binding names an unknown buffer.");
                        }
                        const u64 size = entry.size != 0 ? entry.size : buffer->size - entry.offset;
                        const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
                            .BufferLocation = buffer->gpu_address + entry.offset,


                            .SizeInBytes = static_cast<UINT>(align_up(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)),
                        };
                        device_->CreateConstantBufferView(&cbv, destination);
                        break;
                    }
                    case rhi::BindingType::ReadOnlyStorageBuffer: {
                        const BufferRecord *buffer = buffers_.find(entry.buffer);
                        if (buffer == nullptr) {
                            return invalid_argument("create_bind_group: storage binding names an unknown buffer.");
                        }
                        auto range = buffer_descriptor_range(*buffer, entry);
                        if (!range) {
                            return std::unexpected(range.error());
                        }
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = range->is_raw() ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv.Buffer = {
                            .FirstElement = range->first_element,
                            .NumElements = range->element_count,
                            .StructureByteStride = range->structure_stride,
                            .Flags = range->is_raw() ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE,
                        };
                        device_->CreateShaderResourceView(buffer->resource.Get(), &srv, destination);
                        break;
                    }
                    case rhi::BindingType::StorageBuffer: {
                        const BufferRecord *buffer = buffers_.find(entry.buffer);
                        if (buffer == nullptr) {
                            return invalid_argument("create_bind_group: storage binding names an unknown buffer.");
                        }
                        auto range = buffer_descriptor_range(*buffer, entry);
                        if (!range) {
                            return std::unexpected(range.error());
                        }
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                        uav.Format = range->is_raw() ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
                        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        uav.Buffer = {
                            .FirstElement = range->first_element,
                            .NumElements = range->element_count,
                            .StructureByteStride = range->structure_stride,
                            .CounterOffsetInBytes = 0,
                            .Flags = range->is_raw() ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE,
                        };
                        device_->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uav, destination);
                        break;
                    }
                    case rhi::BindingType::AccelerationStructure: {
                        const AccelerationStructureRecord *as =
                            acceleration_structures_.find(entry.acceleration_structure);
                        if (as == nullptr) {
                            return invalid_argument(
                                "create_bind_group: acceleration-structure binding names an unknown handle.");
                        }


                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = DXGI_FORMAT_UNKNOWN;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv.RaytracingAccelerationStructure.Location = as->gpu_address;
                        device_->CreateShaderResourceView(nullptr, &srv, destination);
                        break;
                    }
                    case rhi::BindingType::SampledTexture:
                    case rhi::BindingType::CombinedImageSampler:
                    case rhi::BindingType::InputAttachment: {
                        const TextureViewRecord *view = texture_views_.find(entry.texture_view);
                        if (view == nullptr || !view->srv.is_valid()) {
                            return invalid_argument(
                                "create_bind_group: sampled binding names a texture view with no shader-resource "
                                "view (was the texture created with TextureUsage::Sampled?). binding=" +
                                std::to_string(entry.binding) + " label=" + (desc.label ? desc.label : "<null>") +
                                " view_found=" + (view != nullptr ? "yes" : "no"));
                        }
                        resource_copy_dests.push_back(destination);
                        resource_copy_srcs.push_back(cpu_resource_descriptors_.cpu_handle(view->srv, 0));
                        break;
                    }
                    case rhi::BindingType::StorageTexture: {
                        const TextureViewRecord *view = texture_views_.find(entry.texture_view);
                        if (view == nullptr || !view->uav.is_valid()) {
                            return invalid_argument(
                                "create_bind_group: storage-texture binding names a texture view with no unordered-"
                                "access view (was the texture created with TextureUsage::Storage?).");
                        }
                        resource_copy_dests.push_back(destination);
                        resource_copy_srcs.push_back(cpu_resource_descriptors_.cpu_handle(view->uav, 0));
                        break;
                    }
                    case rhi::BindingType::Sampler:
                        break;
                }
            }

            if (sampler_it != layout->sampler_slots.end()) {
                if (entry.array_element >= sampler_it->count) {
                    return invalid_argument("create_bind_group: array_element is out of range for sampler binding " +
                                            std::to_string(entry.binding) + ".");
                }
                const SamplerRecord *sampler = samplers_.find(entry.sampler);
                if (sampler == nullptr) {
                    return invalid_argument("create_bind_group: sampler binding " + std::to_string(entry.binding) +
                                            " names an unknown sampler.");
                }
                sampler_copy_dests.push_back(
                    cpu_sampler_descriptors_.cpu_handle(record.samplers, sampler_it->table_offset + entry.array_element));
                sampler_copy_srcs.push_back(cpu_sampler_descriptors_.cpu_handle(sampler->descriptor, 0));
            }
        }

        if (!resource_copy_dests.empty()) {
            device_->CopyDescriptors(static_cast<UINT>(resource_copy_dests.size()), resource_copy_dests.data(), nullptr,
                                     static_cast<UINT>(resource_copy_srcs.size()), resource_copy_srcs.data(), nullptr,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (!sampler_copy_dests.empty()) {
            device_->CopyDescriptors(static_cast<UINT>(sampler_copy_dests.size()), sampler_copy_dests.data(), nullptr,
                                     static_cast<UINT>(sampler_copy_srcs.size()), sampler_copy_srcs.data(), nullptr,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }

        rollback.committed = true;
        return bind_groups_.insert(std::move(record));
    }

    /// Releases bind group descriptors using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::release_bind_group_descriptors(BindGroupRecord &record) noexcept {
        cpu_resource_descriptors_.release(record.resources);
        cpu_sampler_descriptors_.release(record.samplers);
        record.resources = {};
        record.samplers = {};
    }

    /// Destroys the bind group identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_bind_group(rhi::BindGroupHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_bind_group");
        if (auto record = bind_groups_.extract(handle)) {
            release_bind_group_descriptors(*record);
        }
    }

} // namespace SFT::D3D12
