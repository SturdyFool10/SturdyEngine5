#include <Core/D3D12/RHI/D3D12CommandEncoder.hpp>

#pragma region Imports
#include <Core/D3D12/RHI/D3D12Convert.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {

        /// Computes the subresource index required by the supplied values.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param mip `mip` value used by the operation.
        /// @param layer `layer` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 subresource_index(const TextureRecord &texture, u32 mip, u32 layer) noexcept {
            return mip + layer * texture.mip_levels;
        }


        /// Normalizes sync access using the supplied arguments and current state.
        ///
        /// @param sync `sync` value used by the operation.
        /// @param access `access` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void normalize_sync_access(D3D12_BARRIER_SYNC &sync, D3D12_BARRIER_ACCESS &access) noexcept {
            if (sync == D3D12_BARRIER_SYNC_NONE) {
                access = D3D12_BARRIER_ACCESS_NO_ACCESS;
            } else if (access == D3D12_BARRIER_ACCESS_NO_ACCESS) {
                sync = D3D12_BARRIER_SYNC_NONE;
            }
        }

        /// Converts the value to subresource range representation.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param range Range of values to process.
        ///
        /// @return Returns the value converted to subresource range representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_BARRIER_SUBRESOURCE_RANGE to_subresource_range(
            const TextureRecord &texture,
            const rhi::TextureSubresourceRange &range) noexcept {
            const u32 mip_count = range.mip_level_count == ~0u ? texture.mip_levels - range.base_mip_level
                                                               : range.mip_level_count;
            const u32 layer_count = range.array_layer_count == ~0u ? texture.array_layers - range.base_array_layer
                                                                   : range.array_layer_count;
            return D3D12_BARRIER_SUBRESOURCE_RANGE{
                .IndexOrFirstMipLevel = range.base_mip_level,
                .NumMipLevels = std::max(1u, mip_count),
                .FirstArraySlice = range.base_array_layer,
                .NumArraySlices = std::max(1u, layer_count),
                .FirstPlane = 0,


                .NumPlanes = rhi::format_has_stencil(texture.format) ? 2u : 1u,
            };
        }

    } // namespace

    /// Resets the object to its baseline state.
    ///
    /// @return Returns the current reset value.
    /// @note This function does not throw exceptions.
    void BindingState::reset() noexcept {
        layout = {};
        for (PendingBindGroup &group : groups) {
            group = PendingBindGroup{};
        }
        push_constants.clear();
        push_constants_dirty = false;
        layout_dirty = false;
    }


    /// Performs the d3 d12 command encoder operation for `D3D12` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    /// @param record `record` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12CommandEncoder::D3D12CommandEncoder(D3D12Device &device, CommandBufferRecord &&record)
        : device_(&device), record_(std::move(record)), list_(record_.list.Get()) {
        (void)record_.list.As(&list1_);
        (void)record_.list.As(&list5_);
        (void)record_.list.As(&list4_);
        (void)record_.list.As(&list6_);
        if (device_->enhanced_barriers_) {
            (void)record_.list.As(&list7_);
        }
        bind_descriptor_heaps();
    }

    /// Destroys the `D3D12` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    D3D12CommandEncoder::~D3D12CommandEncoder() {
        if (!finished_ && record_.list != nullptr) {


            (void)record_.list->Close();
            device_->return_command_buffer(std::move(record_));
        }
    }

    /// Performs the fail operation for `D3D12` using the supplied arguments.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12CommandEncoder::fail(std::string message) noexcept {
        if (!deferred_error_.has_value()) {
            deferred_error_ = rhi::RhiError{rhi::RhiErrorCode::InvalidArgument, std::move(message)};
        }
    }

    /// Binds descriptor heaps for subsequent operations.
    ///
    /// @return Returns the current bind descriptor heaps value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::bind_descriptor_heaps() {
        if (record_.list_type == D3D12_COMMAND_LIST_TYPE_COPY || !record_.resource_heap.is_valid()) {
            return;
        }
        ID3D12DescriptorHeap *heaps[] = {record_.resource_heap.heap(), record_.sampler_heap.heap()};
        const UINT heap_count = record_.sampler_heap.is_valid() ? 2u : 1u;
        list_->SetDescriptorHeaps(heap_count, heaps);
    }

    /// Uploads bind group using the supplied arguments and current state.
    ///
    /// @param group `group` value used by the operation.
    /// @param layout `layout` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<D3D12CommandEncoder::BoundTables> D3D12CommandEncoder::upload_bind_group(
        const BindGroupRecord &group,
        const BindGroupLayoutRecord &layout,
        bool) {
        BoundTables tables{};

        if (group.resources.is_valid()) {
            const std::optional<u32> offset = record_.resource_heap.allocate(group.resources.count);
            if (!offset.has_value()) {
                return std::nullopt;
            }
            device_->device_->CopyDescriptorsSimple(
                group.resources.count,
                record_.resource_heap.cpu_handle(*offset),
                device_->cpu_resource_descriptors_.cpu_handle(group.resources, 0),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            tables.resource_table = record_.resource_heap.gpu_handle(*offset);
        }

        if (group.samplers.is_valid()) {
            const std::optional<u32> offset = record_.sampler_heap.allocate(group.samplers.count);
            if (!offset.has_value()) {
                return std::nullopt;
            }
            device_->device_->CopyDescriptorsSimple(
                group.samplers.count,
                record_.sampler_heap.cpu_handle(*offset),
                device_->cpu_sampler_descriptors_.cpu_handle(group.samplers, 0),
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            tables.sampler_table = record_.sampler_heap.gpu_handle(*offset);
        }

        (void)layout;
        return tables;
    }

    /// Flushes bindings.
    ///
    /// @param state `state` value used by the operation.
    /// @param graphics `graphics` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool D3D12CommandEncoder::flush_bindings(BindingState &state, bool graphics) {
        const PipelineLayoutRecord *layout = device_->pipeline_layouts_.find(state.layout);
        if (layout == nullptr) {
            fail("A draw or dispatch was recorded before a pipeline (and therefore a pipeline layout) was bound.");
            return false;
        }

        if (state.layout_dirty) {
            if (graphics) {
                list_->SetGraphicsRootSignature(layout->root_signature.Get());
            } else {
                list_->SetComputeRootSignature(layout->root_signature.Get());
            }


            for (PendingBindGroup &group : state.groups) {
                if (group.handle.is_valid()) {
                    group.dirty = true;
                }
            }
            state.push_constants_dirty = !state.push_constants.empty();
            state.layout_dirty = false;
        }


        for (u32 attempt = 0; attempt < 2; ++attempt) {
            bool exhausted = false;

            for (u32 set_index = 0; set_index < max_tracked_bind_groups && set_index < layout->sets.size();
                 ++set_index) {
                PendingBindGroup &pending = state.groups[set_index];
                if (!pending.handle.is_valid() || !pending.dirty) {
                    continue;
                }
                const BindGroupRecord *group = device_->bind_groups_.find(pending.handle);
                if (group == nullptr) {
                    fail("set_bind_group was given a bind group handle that has since been destroyed.");
                    return false;
                }
                const BindGroupLayoutRecord *group_layout = device_->bind_group_layouts_.find(group->layout);
                if (group_layout == nullptr) {
                    fail("A bound bind group's layout has been destroyed.");
                    return false;
                }
                if (set_index >= layout->set_layouts.size() || group->layout != layout->set_layouts[set_index]) {
                    fail("A bound bind group's layout is incompatible with the current pipeline layout at that set index.");
                    return false;
                }
                if (pending.dynamic_offsets.size() != group_layout->dynamic_slots.size()) {
                    fail("set_bind_group supplied the wrong number of dynamic offsets for its layout.");
                    return false;
                }

                const std::optional<BoundTables> tables = upload_bind_group(*group, *group_layout, attempt == 0);
                if (!tables.has_value()) {
                    exhausted = true;
                    break;
                }

                const SetRootParameters &mapping = layout->sets[set_index];
                if (tables->resource_table.has_value() && mapping.resource_table >= 0) {
                    if (graphics) {
                        list_->SetGraphicsRootDescriptorTable(static_cast<UINT>(mapping.resource_table),
                                                              *tables->resource_table);
                    } else {
                        list_->SetComputeRootDescriptorTable(static_cast<UINT>(mapping.resource_table),
                                                             *tables->resource_table);
                    }
                }
                if (tables->sampler_table.has_value() && mapping.sampler_table >= 0) {
                    if (graphics) {
                        list_->SetGraphicsRootDescriptorTable(static_cast<UINT>(mapping.sampler_table),
                                                              *tables->sampler_table);
                    } else {
                        list_->SetComputeRootDescriptorTable(static_cast<UINT>(mapping.sampler_table),
                                                             *tables->sampler_table);
                    }
                }

                for (usize slot = 0; slot < group_layout->dynamic_slots.size(); ++slot) {
                    if (slot >= mapping.dynamic_root_parameters.size() ||
                        slot >= group->dynamic_addresses.size()) {
                        break;
                    }
                    const D3D12_GPU_VIRTUAL_ADDRESS base = group->dynamic_addresses[slot];
                    if (base == 0) {
                        fail("A dynamic-offset binding was never given a buffer when its bind group was created.");
                        return false;
                    }
                    const u64 dynamic_offset = slot < pending.dynamic_offsets.size() ? pending.dynamic_offsets[slot] : 0;
                    const UINT parameter = static_cast<UINT>(mapping.dynamic_root_parameters[slot]);
                    const D3D12_GPU_VIRTUAL_ADDRESS address = base + dynamic_offset;
                    switch (group_layout->dynamic_slots[slot].type) {
                        case rhi::BindingType::UniformBuffer:
                            if (graphics) {
                                list_->SetGraphicsRootConstantBufferView(parameter, address);
                            } else {
                                list_->SetComputeRootConstantBufferView(parameter, address);
                            }
                            break;
                        case rhi::BindingType::StorageBuffer:
                            if (graphics) {
                                list_->SetGraphicsRootUnorderedAccessView(parameter, address);
                            } else {
                                list_->SetComputeRootUnorderedAccessView(parameter, address);
                            }
                            break;
                        default:
                            if (graphics) {
                                list_->SetGraphicsRootShaderResourceView(parameter, address);
                            } else {
                                list_->SetComputeRootShaderResourceView(parameter, address);
                            }
                            break;
                    }
                }
                pending.dirty = false;
            }

            if (!exhausted) {
                break;
            }
            if (attempt == 1) {
                fail("A single command list bound more descriptors than a fresh shader-visible heap can hold.");
                return false;
            }


            if (auto resource_heap = device_->create_shader_visible_heap(
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    default_shader_visible_resource_descriptors)) {
                record_.retired_heaps.push_back(std::move(record_.resource_heap));
                record_.resource_heap = std::move(*resource_heap);
            } else {
                fail("Allocating a replacement shader-visible descriptor heap failed.");
                return false;
            }
            if (auto sampler_heap = device_->create_shader_visible_heap(
                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                    default_shader_visible_sampler_descriptors)) {
                record_.retired_heaps.push_back(std::move(record_.sampler_heap));
                record_.sampler_heap = std::move(*sampler_heap);
            } else {
                fail("Allocating a replacement shader-visible sampler heap failed.");
                return false;
            }
            bind_descriptor_heaps();


            for (BindingState *tracked : {&graphics_bindings_, &compute_bindings_}) {
                for (PendingBindGroup &group : tracked->groups) {
                    if (group.handle.is_valid()) {
                        group.dirty = true;
                    }
                }
            }
        }

        if (state.push_constants.size() > static_cast<usize>(layout->push_constant_values) * sizeof(u32)) {
            fail("Push-constant data exceeds the current pipeline layout's declared range.");
            return false;
        }
        if (state.push_constants_dirty && layout->push_constant_root_parameter >= 0) {
            const UINT values = static_cast<UINT>(state.push_constants.size() / 4);
            if (values > 0) {
                if (graphics) {
                    list_->SetGraphicsRoot32BitConstants(static_cast<UINT>(layout->push_constant_root_parameter),
                                                         values,
                                                         state.push_constants.data(),
                                                         0);
                } else {
                    list_->SetComputeRoot32BitConstants(static_cast<UINT>(layout->push_constant_root_parameter),
                                                        values,
                                                        state.push_constants.data(),
                                                        0);
                }
            }
            state.push_constants_dirty = false;
        }
        return !deferred_error_.has_value();
    }

    /// Reports whether record outside pass holds for this `D3D12`.
    ///
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool D3D12CommandEncoder::can_record_outside_pass(const char *operation) {
        if (finished_ || list_ == nullptr) {
            fail(std::string(operation) + ": the command encoder is already finished.");
            return false;
        }
        if (pass_open_) {
            fail(std::string(operation) + ": this command must be recorded outside a render or compute pass.");
            return false;
        }
        return true;
    }

    /// Creates a transient upload from the supplied parameters.
    ///
    /// @param data Data consumed or referenced by the operation.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<ComPtr<ID3D12Resource>> D3D12CommandEncoder::create_transient_upload(
        span<const std::byte> data, const char *operation) {
        if (data.empty()) {
            return invalid_argument(std::string(operation) + ": upload data cannot be empty.");
        }
        const CD3DX12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(data.size());
        ComPtr<ID3D12Resource> upload;
        if (const HRESULT hr = device_->device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&upload));
            FAILED(hr)) {
            return hresult_error(hr, std::string(operation) + " (CreateCommittedResource)");
        }
        void *mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        if (const HRESULT hr = upload->Map(0, &no_read, &mapped); FAILED(hr)) {
            return hresult_error(hr, std::string(operation) + " (Map)");
        }
        std::memcpy(mapped, data.data(), data.size());
        const D3D12_RANGE written{0, data.size()};
        upload->Unmap(0, &written);
        return upload;
    }


    /// Performs the legacy transition operation for `D3D12` using the supplied arguments.
    ///
    /// @param texture Texture used or affected by the operation.
    /// @param subresource `subresource` value used by the operation.
    /// @param after `after` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::legacy_transition(TextureRecord &texture, u32 subresource, D3D12_RESOURCE_STATES after) {
        if (subresource >= texture.legacy_states.size()) {
            return;
        }
        const D3D12_RESOURCE_STATES before = texture.legacy_states[subresource];
        if (before == after) {
            if (after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = texture.resource.Get();
                list_->ResourceBarrier(1, &barrier);
            }
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition = {.pResource = texture.resource.Get(),
                              .Subresource = subresource,
                              .StateBefore = before,
                              .StateAfter = after};
        list_->ResourceBarrier(1, &barrier);
        texture.legacy_states[subresource] = after;
    }

    /// Performs the barrier operation for `D3D12` using the supplied arguments.
    ///
    /// @param global_barriers `global_barriers` value used by the operation.
    /// @param buffer_barriers Buffer used or affected by the operation.
    /// @param texture_barriers Texture used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::barrier(span<const rhi::GlobalBarrier> global_barriers,
                                      span<const rhi::BufferBarrier> buffer_barriers,
                                      span<const rhi::TextureBarrier> texture_barriers) {
        ZoneScopedN("D3D12CommandEncoder::barrier");
        if (pass_open_) {
            fail("barrier() was recorded inside an open render or compute pass; barriers belong on the parent "
                 "command encoder, outside any pass.");
            return;
        }

        if (list7_ != nullptr) {
            std::vector<D3D12_GLOBAL_BARRIER> globals;
            std::vector<D3D12_BUFFER_BARRIER> buffers;
            std::vector<D3D12_TEXTURE_BARRIER> textures;
            globals.reserve(global_barriers.size());
            buffers.reserve(buffer_barriers.size());
            textures.reserve(texture_barriers.size());

            for (const rhi::GlobalBarrier &source : global_barriers) {
                D3D12_BARRIER_SYNC sync_before = to_d3d12_sync(source.src_stage);
                D3D12_BARRIER_ACCESS access_before = to_d3d12_access(source.src_access);
                D3D12_BARRIER_SYNC sync_after = to_d3d12_sync(source.dst_stage);
                D3D12_BARRIER_ACCESS access_after = to_d3d12_access(source.dst_access);
                normalize_sync_access(sync_before, access_before);
                normalize_sync_access(sync_after, access_after);
                globals.push_back(D3D12_GLOBAL_BARRIER{sync_before, sync_after, access_before, access_after});
            }

            for (const rhi::BufferBarrier &source : buffer_barriers) {
                const BufferRecord *buffer = device_->buffers_.find(source.buffer);
                if (buffer == nullptr) {
                    fail("A buffer barrier names an unknown buffer handle.");
                    return;
                }
                D3D12_BARRIER_SYNC sync_before = to_d3d12_sync(source.src_stage);
                D3D12_BARRIER_ACCESS access_before = to_d3d12_access(source.src_access);
                D3D12_BARRIER_SYNC sync_after = to_d3d12_sync(source.dst_stage);
                D3D12_BARRIER_ACCESS access_after = to_d3d12_access(source.dst_access);
                normalize_sync_access(sync_before, access_before);
                normalize_sync_access(sync_after, access_after);


                buffers.push_back(D3D12_BUFFER_BARRIER{
                    sync_before,
                    sync_after,
                    access_before,
                    access_after,
                    buffer->resource.Get(),
                    source.offset,


                    source.size == 0 ? UINT64_MAX : source.size});
            }

            for (const rhi::TextureBarrier &source : texture_barriers) {
                TextureRecord *texture = device_->textures_.find(source.texture);
                if (texture == nullptr) {
                    fail("A texture barrier names an unknown texture handle.");
                    return;
                }
                D3D12_BARRIER_SYNC sync_before = to_d3d12_sync(source.src_stage);
                D3D12_BARRIER_ACCESS access_before = to_d3d12_access(source.src_access);
                D3D12_BARRIER_SYNC sync_after = to_d3d12_sync(source.dst_stage);
                D3D12_BARRIER_ACCESS access_after = to_d3d12_access(source.dst_access);
                normalize_sync_access(sync_before, access_before);
                normalize_sync_access(sync_after, access_after);

                const bool discarding = source.old_layout == rhi::TextureLayout::Undefined;
                if (discarding) {


                    sync_before = D3D12_BARRIER_SYNC_NONE;
                    access_before = D3D12_BARRIER_ACCESS_NO_ACCESS;
                }
                textures.push_back(D3D12_TEXTURE_BARRIER{
                    sync_before,
                    sync_after,
                    access_before,
                    access_after,
                    to_d3d12_layout(source.old_layout),
                    to_d3d12_layout(source.new_layout),
                    texture->resource.Get(),
                    to_subresource_range(*texture, source.range),
                    discarding ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE});
            }

            std::vector<D3D12_BARRIER_GROUP> groups;
            if (!globals.empty()) {
                groups.push_back(CD3DX12_BARRIER_GROUP(static_cast<UINT32>(globals.size()), globals.data()));
            }
            if (!buffers.empty()) {
                groups.push_back(CD3DX12_BARRIER_GROUP(static_cast<UINT32>(buffers.size()), buffers.data()));
            }
            if (!textures.empty()) {
                groups.push_back(CD3DX12_BARRIER_GROUP(static_cast<UINT32>(textures.size()), textures.data()));
            }
            if (!groups.empty()) {
                list7_->Barrier(static_cast<UINT32>(groups.size()), groups.data());
            }
            return;
        }


        std::vector<D3D12_RESOURCE_BARRIER> barriers;

        for (const rhi::GlobalBarrier &source : global_barriers) {


            if (rhi::has_any(source.src_access, rhi::AccessFlags::ShaderWrite | rhi::AccessFlags::MemoryWrite)) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = nullptr;
                barriers.push_back(barrier);
            }
        }

        for (const rhi::BufferBarrier &source : buffer_barriers) {
            const BufferRecord *buffer = device_->buffers_.find(source.buffer);
            if (buffer == nullptr) {
                fail("A buffer barrier names an unknown buffer handle.");
                return;
            }


            if (buffer->memory != rhi::MemoryLocation::DeviceLocal) {
                continue;
            }
            const D3D12_RESOURCE_STATES before = to_legacy_buffer_state(source.src_access);
            const D3D12_RESOURCE_STATES after = to_legacy_buffer_state(source.dst_access);
            if (before == after) {
                if (after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    barrier.UAV.pResource = buffer->resource.Get();
                    barriers.push_back(barrier);
                }
                continue;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {.pResource = buffer->resource.Get(),
                                  .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  .StateBefore = before,
                                  .StateAfter = after};
            barriers.push_back(barrier);
        }

        if (!barriers.empty()) {
            list_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        for (const rhi::TextureBarrier &source : texture_barriers) {
            TextureRecord *texture = device_->textures_.find(source.texture);
            if (texture == nullptr) {
                fail("A texture barrier names an unknown texture handle.");
                return;
            }
            const D3D12_RESOURCE_STATES after = to_legacy_texture_state(source.new_layout);
            const u32 first_mip = source.range.base_mip_level;
            const u32 mip_count = source.range.mip_level_count == ~0u ? texture->mip_levels - first_mip
                                                                      : source.range.mip_level_count;
            const u32 first_layer = source.range.base_array_layer;
            const u32 layer_count = source.range.array_layer_count == ~0u ? texture->array_layers - first_layer
                                                                          : source.range.array_layer_count;
            for (u32 layer = 0; layer < layer_count; ++layer) {
                for (u32 mip = 0; mip < mip_count; ++mip) {
                    legacy_transition(*texture, subresource_index(*texture, first_mip + mip, first_layer + layer), after);
                }
            }
        }
    }


    /// Copies buffer to buffer to its destination.
    ///
    /// @param src Source value or resource.
    /// @param dst Destination value or resource.
    /// @param region `region` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst, const rhi::BufferCopy &region) {
        if (!can_record_outside_pass("copy_buffer_to_buffer")) {
            return;
        }
        const BufferRecord *source = device_->buffers_.find(src);
        const BufferRecord *destination = device_->buffers_.find(dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_buffer_to_buffer names an unknown buffer handle.");
            return;
        }
        if (region.src_offset > source->size || region.dst_offset > destination->size ||
            !rhi::has_any(source->usage, rhi::BufferUsage::TransferSrc) ||
            !rhi::has_any(destination->usage, rhi::BufferUsage::TransferDst)) {
            fail("copy_buffer_to_buffer: invalid offset or transfer usage.");
            return;
        }
        const u64 size = region.size != 0 ? region.size : source->size - region.src_offset;
        if (size > source->size - region.src_offset || size > destination->size - region.dst_offset) {
            fail("copy_buffer_to_buffer: copy range exceeds a buffer.");
            return;
        }
        list_->CopyBufferRegion(destination->resource.Get(), region.dst_offset, source->resource.Get(), region.src_offset, size);
    }

    namespace {


        /// Builds texture footprint.
        ///
        /// @param texture Texture used or affected by the operation.
        /// @param region `region` value used by the operation.
        /// @param footprint `footprint` value used by the operation.
        /// @param row_pitch `row_pitch` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool build_texture_footprint(const TextureRecord &texture, const rhi::BufferTextureCopy &region, D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, u64 &row_pitch) {
            const u32 block = format_block_extent(texture.format);
            const u32 element_bytes = format_element_bytes(texture.format);
            const u32 width = region.texture_extent.width != 0 ? region.texture_extent.width : texture.extent.width;
            const u32 height = region.texture_extent.height != 0 ? region.texture_extent.height : texture.extent.height;
            const u32 row_length = region.buffer_row_length != 0 ? region.buffer_row_length : width;
            const u32 image_height = region.buffer_image_height != 0 ? region.buffer_image_height : height;

            row_pitch = static_cast<u64>((row_length + block - 1) / block) * element_bytes;
            if (row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) {
                return false;
            }
            footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{
                .Offset = region.buffer_offset,
                .Footprint = {.Format = texture.resource_format,
                              .Width = width,
                              .Height = image_height,
                              .Depth = region.texture_extent.depth_or_layers != 0
                                           ? region.texture_extent.depth_or_layers
                                           : 1u,
                              .RowPitch = static_cast<UINT>(row_pitch)},
            };
            return true;
        }

    } // namespace

    /// Copies buffer to texture to its destination.
    ///
    /// @param src Source value or resource.
    /// @param dst Destination value or resource.
    /// @param region `region` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst, const rhi::BufferTextureCopy &region) {
        if (!can_record_outside_pass("copy_buffer_to_texture")) {
            return;
        }
        const BufferRecord *source = device_->buffers_.find(src);
        const TextureRecord *destination = device_->textures_.find(dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_buffer_to_texture names an unknown handle.");
            return;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u64 row_pitch = 0;
        if (!build_texture_footprint(*destination, region, footprint, row_pitch)) {
            fail("copy_buffer_to_texture: the source row pitch (" + std::to_string(row_pitch) +
                 " bytes) is not a multiple of D3D12's 256-byte texture row-pitch alignment. Pad each row of the "
                 "staging buffer, or set BufferTextureCopy::buffer_row_length to a padded width.");
            return;
        }

        for (u32 layer = 0; layer < std::max(1u, region.array_layer_count); ++layer) {
            const CD3DX12_TEXTURE_COPY_LOCATION destination_location(
                destination->resource.Get(),
                subresource_index(*destination, region.mip_level, region.base_array_layer + layer));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layer_footprint = footprint;
            layer_footprint.Offset += static_cast<u64>(layer) * row_pitch * footprint.Footprint.Height;
            const CD3DX12_TEXTURE_COPY_LOCATION source_location(source->resource.Get(), layer_footprint);
            list_->CopyTextureRegion(&destination_location, region.texture_offset.x, region.texture_offset.y, region.texture_offset.z, &source_location, nullptr);
        }
    }

    /// Copies texture to buffer to its destination.
    ///
    /// @param src Source value or resource.
    /// @param dst Destination value or resource.
    /// @param region `region` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst, const rhi::BufferTextureCopy &region) {
        if (!can_record_outside_pass("copy_texture_to_buffer")) {
            return;
        }
        const TextureRecord *source = device_->textures_.find(src);
        const BufferRecord *destination = device_->buffers_.find(dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_texture_to_buffer names an unknown handle.");
            return;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u64 row_pitch = 0;
        if (!build_texture_footprint(*source, region, footprint, row_pitch)) {
            fail("copy_texture_to_buffer: the destination row pitch (" + std::to_string(row_pitch) +
                 " bytes) is not a multiple of D3D12's 256-byte texture row-pitch alignment.");
            return;
        }

        for (u32 layer = 0; layer < std::max(1u, region.array_layer_count); ++layer) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layer_footprint = footprint;
            layer_footprint.Offset += static_cast<u64>(layer) * row_pitch * footprint.Footprint.Height;
            const CD3DX12_TEXTURE_COPY_LOCATION destination_location(destination->resource.Get(), layer_footprint);
            const CD3DX12_TEXTURE_COPY_LOCATION source_location(
                source->resource.Get(),
                subresource_index(*source, region.mip_level, region.base_array_layer + layer));
            const CD3DX12_BOX box(region.texture_offset.x, region.texture_offset.y, region.texture_offset.z, region.texture_offset.x + static_cast<LONG>(region.texture_extent.width), region.texture_offset.y + static_cast<LONG>(region.texture_extent.height), region.texture_offset.z + static_cast<LONG>(std::max(1u, region.texture_extent.depth_or_layers)));
            list_->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, &box);
        }
    }

    /// Copies texture to texture to its destination.
    ///
    /// @param src Source value or resource.
    /// @param dst Destination value or resource.
    /// @param region `region` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureCopy &region) {
        if (!can_record_outside_pass("copy_texture_to_texture")) {
            return;
        }
        const TextureRecord *source = device_->textures_.find(src);
        const TextureRecord *destination = device_->textures_.find(dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_texture_to_texture names an unknown texture handle.");
            return;
        }
        for (u32 layer = 0; layer < std::max(1u, region.src_subresource.array_layer_count); ++layer) {
            const CD3DX12_TEXTURE_COPY_LOCATION source_location(
                source->resource.Get(),
                subresource_index(*source, region.src_subresource.mip_level, region.src_subresource.base_array_layer + layer));
            const CD3DX12_TEXTURE_COPY_LOCATION destination_location(
                destination->resource.Get(),
                subresource_index(*destination, region.dst_subresource.mip_level, region.dst_subresource.base_array_layer + layer));
            const CD3DX12_BOX box(region.src_offset.x, region.src_offset.y, region.src_offset.z, region.src_offset.x + static_cast<LONG>(region.extent.width), region.src_offset.y + static_cast<LONG>(region.extent.height), region.src_offset.z + static_cast<LONG>(std::max(1u, region.extent.depth_or_layers)));
            list_->CopyTextureRegion(&destination_location, region.dst_offset.x, region.dst_offset.y, region.dst_offset.z, &source_location, &box);
        }
    }

    /// Performs the blit texture operation for `D3D12` using the supplied arguments.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::blit_texture(rhi::TextureHandle, rhi::TextureHandle, const rhi::TextureBlit &, rhi::Filter) {
        if (!can_record_outside_pass("blit_texture")) {
            return;
        }


        fail("blit_texture: D3D12 has no scaled/filtered image blit. Use a compute or raster downsample pass "
             "instead (this is why the operation is reported here rather than approximated).");
    }

    /// Fills buffer using the supplied arguments and current state.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param size Requested or available size for the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::fill_buffer(rhi::BufferHandle buffer, u64 offset, u64 size, u32 value) {
        if (!can_record_outside_pass("fill_buffer")) {
            return;
        }
        const BufferRecord *destination = device_->buffers_.find(buffer);
        if (destination == nullptr || destination->memory != rhi::MemoryLocation::DeviceLocal ||
            !rhi::has_any(destination->usage, rhi::BufferUsage::TransferDst) || offset > destination->size) {
            fail("fill_buffer: destination must be a valid DeviceLocal TransferDst buffer and offset.");
            return;
        }
        const u64 clear_size = size != 0 ? size : destination->size - offset;
        if (clear_size > destination->size - offset || offset % sizeof(u32) != 0 ||
            clear_size % sizeof(u32) != 0) {
            fail("fill_buffer: offset and size must be in range and four-byte aligned.");
            return;
        }
        if (clear_size == 0) {
            return;
        }

        constexpr usize max_pattern_bytes = 64 * 1024;
        const usize pattern_size = static_cast<usize>(std::min<u64>(clear_size, max_pattern_bytes));
        vector<std::byte> pattern(pattern_size);
        for (usize byte_offset = 0; byte_offset < pattern.size(); byte_offset += sizeof(value)) {
            std::memcpy(pattern.data() + byte_offset, &value, sizeof(value));
        }
        auto upload = create_transient_upload(pattern, "fill_buffer");
        if (!upload) {
            fail(upload.error().message);
            return;
        }
        for (u64 copied = 0; copied < clear_size; copied += pattern_size) {
            const u64 copy_size = std::min<u64>(pattern_size, clear_size - copied);
            list_->CopyBufferRegion(destination->resource.Get(), offset + copied, upload->Get(), 0, copy_size);
        }
        record_.transient_uploads.push_back(std::move(*upload));
    }

    /// Updates buffer from the supplied values.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::update_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) {
        if (!can_record_outside_pass("update_buffer")) {
            return;
        }
        const BufferRecord *destination = device_->buffers_.find(buffer);
        if (destination == nullptr || destination->memory != rhi::MemoryLocation::DeviceLocal ||
            !rhi::has_any(destination->usage, rhi::BufferUsage::TransferDst) ||
            offset > destination->size || data.size() > destination->size - offset) {
            fail("update_buffer: destination must be an in-range DeviceLocal TransferDst buffer.");
            return;
        }
        if (data.empty()) {
            return;
        }
        if (offset % sizeof(u32) != 0 || data.size() % sizeof(u32) != 0 || data.size() > 65'536) {
            fail("update_buffer: offset/data size must be four-byte aligned and data cannot exceed 65536 bytes.");
            return;
        }
        auto upload = create_transient_upload(data, "update_buffer");
        if (!upload) {
            fail(upload.error().message);
            return;
        }
        list_->CopyBufferRegion(destination->resource.Get(), offset, upload->Get(), 0, data.size());
        record_.transient_uploads.push_back(std::move(*upload));
    }

    void D3D12CommandEncoder::clear_color_texture(rhi::TextureHandle texture, const rhi::ClearColor &color, const rhi::TextureSubresourceRange &range) {
        if (!can_record_outside_pass("clear_color_texture")) {
            return;
        }
        const TextureRecord *record = device_->textures_.find(texture);
        if (record == nullptr) {
            fail("clear_color_texture names an unknown texture handle.");
            return;
        }
        if (!rhi::has_any(record->usage, rhi::TextureUsage::ColorAttachment)) {
            fail("clear_color_texture: D3D12 clears a color texture through a render-target view, so the texture "
                 "must have been created with TextureUsage::ColorAttachment.");
            return;
        }

        auto rtv = device_->cpu_rtv_descriptors_.allocate(1);
        if (!rtv) {
            fail("clear_color_texture: allocating a render-target descriptor failed.");
            return;
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = to_dxgi_view_format(record->format);
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray = {.MipSlice = range.base_mip_level,
                               .FirstArraySlice = range.base_array_layer,
                               .ArraySize = range.array_layer_count == ~0u
                                                ? record->array_layers - range.base_array_layer
                                                : range.array_layer_count,
                               .PlaneSlice = 0};
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = device_->cpu_rtv_descriptors_.cpu_handle(*rtv, 0);
        device_->device_->CreateRenderTargetView(record->resource.Get(), &view, handle);
        record_.transient_rtv_descriptors.push_back(*rtv);
        const float components[4] = {color.r, color.g, color.b, color.a};
        list_->ClearRenderTargetView(handle, components, 0, nullptr);
    }

    void D3D12CommandEncoder::clear_depth_stencil_texture(rhi::TextureHandle texture,
                                                          const rhi::ClearDepthStencil &value,
                                                          const rhi::TextureSubresourceRange &range) {
        if (!can_record_outside_pass("clear_depth_stencil_texture")) {
            return;
        }
        const TextureRecord *record = device_->textures_.find(texture);
        if (record == nullptr) {
            fail("clear_depth_stencil_texture names an unknown texture handle.");
            return;
        }
        auto dsv = device_->cpu_dsv_descriptors_.allocate(1);
        if (!dsv) {
            fail("clear_depth_stencil_texture: allocating a depth-stencil descriptor failed.");
            return;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = to_dxgi_view_format(record->format);
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray = {.MipSlice = range.base_mip_level,
                               .FirstArraySlice = range.base_array_layer,
                               .ArraySize = range.array_layer_count == ~0u
                                                ? record->array_layers - range.base_array_layer
                                                : range.array_layer_count};
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = device_->cpu_dsv_descriptors_.cpu_handle(*dsv, 0);
        device_->device_->CreateDepthStencilView(record->resource.Get(), &view, handle);
        record_.transient_dsv_descriptors.push_back(*dsv);
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (rhi::format_has_stencil(record->format)) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        list_->ClearDepthStencilView(handle, flags, value.depth, static_cast<UINT8>(value.stencil), 0, nullptr);
    }

    // ─── Ray tracing commands ────────────────────────────────────────────────────

    void D3D12CommandEncoder::build_acceleration_structures(span<const rhi::AccelerationStructureBuildDesc> builds) {
        ZoneScopedN("D3D12CommandEncoder::build_acceleration_structures");
        if (!can_record_outside_pass("build_acceleration_structures")) {
            return;
        }
        if (list4_ == nullptr) {
            fail("build_acceleration_structures: this device/runtime provides no DXR command list.");
            return;
        }
        for (const rhi::AccelerationStructureBuildDesc &build : builds) {
            const AccelerationStructureRecord *destination = device_->acceleration_structures_.find(build.dst);
            if (destination == nullptr) {
                fail("build_acceleration_structures: the destination acceleration structure handle is unknown.");
                return;
            }
            const BufferRecord *scratch = device_->buffers_.find(build.scratch_buffer);
            if (scratch == nullptr) {
                fail("build_acceleration_structures: the scratch buffer handle is unknown.");
                return;
            }

            vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_storage;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            if (auto built = build_acceleration_structure_inputs(*device_, build, geometry_storage, inputs); !built) {
                fail(built.error().message);
                return;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
            desc.DestAccelerationStructureData = destination->gpu_address;
            desc.ScratchAccelerationStructureData = scratch->gpu_address + build.scratch_offset;
            desc.Inputs = inputs;
            if (build.src.is_valid()) {
                const AccelerationStructureRecord *source = device_->acceleration_structures_.find(build.src);
                if (source == nullptr) {
                    fail("build_acceleration_structures: the source (update) acceleration structure handle is unknown.");
                    return;
                }
                desc.SourceAccelerationStructureData = source->gpu_address;
                desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            }
            list4_->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
    }

    void D3D12CommandEncoder::build_opacity_micromaps(span<const rhi::OpacityMicromapBuildDesc>) {
        fail("build_opacity_micromaps: D3D12 opacity micromap support is not implemented.");
    }

    void D3D12CommandEncoder::copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) {
        if (!can_record_outside_pass("copy_acceleration_structure")) {
            return;
        }
        if (list4_ == nullptr) {
            fail("copy_acceleration_structure: this device/runtime provides no DXR command list.");
            return;
        }
        const AccelerationStructureRecord *source = device_->acceleration_structures_.find(copy.src);
        const AccelerationStructureRecord *destination = device_->acceleration_structures_.find(copy.dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_acceleration_structure names an unknown acceleration structure handle.");
            return;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE;
        switch (copy.mode) {
            case rhi::AccelerationStructureCopyMode::Compact:
                mode = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT;
                break;
            case rhi::AccelerationStructureCopyMode::Serialize:
                mode = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE;
                break;
            case rhi::AccelerationStructureCopyMode::Deserialize:
                mode = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE;
                break;
            case rhi::AccelerationStructureCopyMode::Clone:
                break;
        }
        list4_->CopyRaytracingAccelerationStructure(destination->gpu_address, source->gpu_address, mode);
    }

    void D3D12CommandEncoder::trace_rays(const rhi::TraceRaysDesc &desc) {
        if (!can_record_outside_pass("trace_rays")) {
            return;
        }
        if (list4_ == nullptr) {
            fail("trace_rays: this device/runtime provides no DXR command list.");
            return;
        }
        const auto region_address = [&](const rhi::ShaderBindingTableRegion &region) -> D3D12_GPU_VIRTUAL_ADDRESS {
            const BufferRecord *buffer = device_->buffers_.find(region.buffer);
            return buffer != nullptr ? buffer->gpu_address + region.offset : 0;
        };

        D3D12_DISPATCH_RAYS_DESC dispatch{};
        // The raygen region is a single record (start + size), while miss/hit/callable are strided
        // tables — the same split Vulkan's VkStridedDeviceAddressRegionKHR draws, so the RHI's regions
        // map across directly.
        dispatch.RayGenerationShaderRecord = {region_address(desc.raygen), desc.raygen.size};
        dispatch.MissShaderTable = {region_address(desc.miss), desc.miss.size, desc.miss.stride};
        dispatch.HitGroupTable = {region_address(desc.hit), desc.hit.size, desc.hit.stride};
        dispatch.CallableShaderTable = {region_address(desc.callable), desc.callable.size, desc.callable.stride};
        dispatch.Width = desc.width;
        dispatch.Height = desc.height;
        dispatch.Depth = desc.depth;

        flush_bindings(compute_bindings_, false);
        list4_->DispatchRays(&dispatch);
    }

    // ─── Queries and debug markers ───────────────────────────────────────────────

    void D3D12CommandEncoder::reset_query_set(rhi::QuerySetHandle, u32, u32) {
        // No-op for the same reason as the device-level reset_query_set(): a D3D12 query slot has no
        // unwritten state to return to.
    }

    void D3D12CommandEncoder::write_timestamp(rhi::PipelineStage stage, rhi::QuerySetHandle query_set, u32 index) {
        const QuerySetRecord *record = device_->query_sets_.find(query_set);
        if (record == nullptr) {
            fail("write_timestamp names an unknown query set handle.");
            return;
        }
        // D3D12 timestamps are always written at the end of the pipeline; there is no stage selector,
        // so a "top of pipe" timestamp is not expressible. Widening to end-of-pipe never under-measures
        // an interval, which is the safe direction for a profiling number.
        (void)stage;
        list_->EndQuery(record->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void D3D12CommandEncoder::begin_pipeline_statistics_query(rhi::QuerySetHandle query_set, u32 index) {
        const QuerySetRecord *record = device_->query_sets_.find(query_set);
        if (record == nullptr) {
            fail("begin_pipeline_statistics_query names an unknown query set handle.");
            return;
        }
        list_->BeginQuery(record->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index);
        statistics_query_set_ = query_set;
        statistics_query_index_ = index;
    }

    void D3D12CommandEncoder::end_pipeline_statistics_query() {
        const QuerySetRecord *record = device_->query_sets_.find(statistics_query_set_);
        if (record == nullptr) {
            fail("end_pipeline_statistics_query was called without a matching begin.");
            return;
        }
        list_->EndQuery(record->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, statistics_query_index_);
        statistics_query_set_ = {};
    }

    void D3D12CommandEncoder::resolve_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count, rhi::BufferHandle dst, u64 dst_offset, u64 stride, rhi::QueryResultFlags flags) {
        const QuerySetRecord *record = device_->query_sets_.find(query_set);
        const BufferRecord *destination = device_->buffers_.find(dst);
        if (record == nullptr || destination == nullptr) {
            fail("resolve_query_set names an unknown handle.");
            return;
        }
        // D3D12's resolve stride is fixed by the query type and cannot be overridden; a caller asking

        const u64 natural_stride = query_result_bytes(record->type);
        if (stride != 0 && stride != natural_stride) {
            fail("resolve_query_set: D3D12 writes resolved results tightly packed at " +
                 std::to_string(natural_stride) + " bytes per slot and cannot honor a different stride.");
            return;
        }
        (void)flags;
        list_->ResolveQueryData(record->heap.Get(),
                                to_d3d12_query_type(record->type,
                                                    device_->enabled_features_.has(rhi::Feature::PreciseOcclusionQueries)),
                                first,
                                count,
                                destination->resource.Get(),
                                dst_offset);
    }

    /// Adds the supplied value to the end or work queue.
    ///
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::push_debug_group(const char *label) {
        if (label != nullptr) {


            list_->BeginEvent(0, label, static_cast<UINT>(std::char_traits<char>::length(label) + 1));
        }
    }

    /// Removes and returns or discards the next value from the container or queue.
    ///
    /// @return Returns the current pop debug group value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12CommandEncoder::pop_debug_group() { list_->EndEvent(); }

    /// Returns the current or globally available finish value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::CommandBufferHandle> D3D12CommandEncoder::finish() {
        ZoneScopedN("D3D12CommandEncoder::finish");
        if (finished_) {
            return operation_failed("finish: this encoder has already been finished.");
        }
        finished_ = true;

        if (pass_open_) {
            fail("finish: a render or compute pass is still open.");
        }
        if (deferred_error_.has_value()) {
            (void)record_.list->Close();
            device_->return_command_buffer(std::move(record_));
            return std::unexpected(*deferred_error_);
        }
        if (const HRESULT hr = record_.list->Close(); FAILED(hr)) {
            std::string operation = "finish (Close)";
            ComPtr<ID3D12InfoQueue> info_queue;
            if (SUCCEEDED(device_->device_.As(&info_queue))) {
                const u64 count = info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
                if (count != 0) {
                    SIZE_T bytes = 0;
                    if (SUCCEEDED(info_queue->GetMessage(count - 1, nullptr, &bytes)) && bytes != 0) {
                        vector<std::byte> storage(bytes);
                        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                        if (SUCCEEDED(info_queue->GetMessage(count - 1, message, &bytes)) &&
                            message->pDescription != nullptr) {
                            operation += ": ";
                            operation.append(message->pDescription, message->DescriptionByteLength);
                        }
                    }
                }
            }
            device_->return_command_buffer(std::move(record_));
            return hresult_error(hr, operation);
        }
        return device_->command_buffers_.insert(std::move(record_));
    }

    /// Performs the begin render pass operation for `D3D12` using the supplied arguments.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> D3D12CommandEncoder::begin_render_pass(
        const rhi::RenderPassDesc &desc) {
        if (finished_ || pass_open_) {
            return operation_failed("begin_render_pass: the command encoder is finished or already has an open pass.");
        }
        if (record_.list_type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return unsupported("begin_render_pass: only a D3D12 direct-queue encoder can record rendering.");
        }
        if (desc.view_mask != 0) {
            return unsupported("begin_render_pass: D3D12 multiview rendering is not implemented.");
        }

        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> render_targets;
        std::vector<D3D12RenderPassEncoder::ColorResolve> color_resolves;
        render_targets.reserve(desc.color_attachments.size());
        color_resolves.reserve(desc.color_attachments.size());
        for (const rhi::ColorAttachment &attachment : desc.color_attachments) {
            const TextureViewRecord *view = device_->texture_views_.find(attachment.view);
            if (view == nullptr || !view->rtv.is_valid()) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "begin_render_pass: a color attachment has no valid RTV.");
            }
            if (attachment.resolve_view.is_valid()) {
                const TextureViewRecord *resolve = device_->texture_views_.find(attachment.resolve_view);
                const TextureRecord *source_texture = device_->textures_.find(view->texture);
                const TextureRecord *destination_texture = resolve != nullptr ? device_->textures_.find(resolve->texture) : nullptr;
                if (resolve == nullptr || !resolve->rtv.is_valid() || source_texture == nullptr ||
                    destination_texture == nullptr || view->format != resolve->format) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                          "begin_render_pass: color resolve views must be valid and have matching formats.");
                }
                color_resolves.push_back({source_texture->resource.Get(), destination_texture->resource.Get(), subresource_index(*source_texture, view->base_mip_level, view->base_array_layer), subresource_index(*destination_texture, resolve->base_mip_level, resolve->base_array_layer), view->format});
            }
            render_targets.push_back(device_->cpu_rtv_descriptors_.cpu_handle(view->rtv, 0));
        }

        D3D12_CPU_DESCRIPTOR_HANDLE depth_target{};
        const D3D12_CPU_DESCRIPTOR_HANDLE *depth_target_ptr = nullptr;
        std::optional<D3D12RenderPassEncoder::DepthResolve> depth_resolve;
        if (desc.depth_stencil.view.is_valid()) {
            const TextureViewRecord *view = device_->texture_views_.find(desc.depth_stencil.view);
            if (view == nullptr || !view->dsv.is_valid()) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "begin_render_pass: the depth/stencil attachment has no valid DSV.");
            }
            if (desc.depth_stencil.resolve_view.is_valid()) {
                if (list1_ == nullptr) {
                    return unsupported(
                        "begin_render_pass: depth resolve requires ID3D12GraphicsCommandList1, which this "
                        "command list does not expose.");
                }
                const TextureViewRecord *resolve = device_->texture_views_.find(desc.depth_stencil.resolve_view);
                const TextureRecord *source_texture = device_->textures_.find(view->texture);
                const TextureRecord *destination_texture =
                    resolve != nullptr ? device_->textures_.find(resolve->texture) : nullptr;
                if (resolve == nullptr || source_texture == nullptr || destination_texture == nullptr ||
                    view->format != resolve->format) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                          "begin_render_pass: depth resolve views must be valid and have matching "
                                          "formats.");
                }
                D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{.Format = resolve->format};
                const bool resolve_supported =
                    SUCCEEDED(device_->device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support,
                                                                     sizeof(format_support))) &&
                    (format_support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) != 0;
                if (!resolve_supported) {
                    return unsupported(
                        "begin_render_pass: the depth resolve target's format does not report "
                        "D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE on this device.");
                }
                depth_resolve = D3D12RenderPassEncoder::DepthResolve{
                    .source = source_texture->resource.Get(),
                    .destination = destination_texture->resource.Get(),
                    .source_subresource = subresource_index(*source_texture, view->base_mip_level, view->base_array_layer),
                    .destination_subresource =
                        subresource_index(*destination_texture, resolve->base_mip_level, resolve->base_array_layer),
                    .format = resolve->format,
                    .resolve_mode = to_d3d12(desc.depth_stencil.depth_resolve_mode),
                };
            }
            depth_target = device_->cpu_dsv_descriptors_.cpu_handle(view->dsv, 0);
            depth_target_ptr = &depth_target;
        }

        list_->OMSetRenderTargets(static_cast<UINT>(render_targets.size()),
                                  render_targets.empty() ? nullptr : render_targets.data(),
                                  FALSE,
                                  depth_target_ptr);
        for (usize i = 0; i < desc.color_attachments.size(); ++i) {
            if (desc.color_attachments[i].load_op == rhi::LoadOp::Clear) {
                const rhi::ClearColor &color = desc.color_attachments[i].clear_color;
                const float values[] = {color.r, color.g, color.b, color.a};
                list_->ClearRenderTargetView(render_targets[i], values, 0, nullptr);
            }
        }
        if (depth_target_ptr != nullptr) {
            const TextureViewRecord *view = device_->texture_views_.find(desc.depth_stencil.view);
            const TextureRecord *texture = view != nullptr ? device_->textures_.find(view->texture) : nullptr;
            D3D12_CLEAR_FLAGS flags{};
            if (desc.depth_stencil.depth_load_op == rhi::LoadOp::Clear)
                flags |= D3D12_CLEAR_FLAG_DEPTH;
            if (texture != nullptr && rhi::format_has_stencil(texture->format) &&
                desc.depth_stencil.stencil_load_op == rhi::LoadOp::Clear)
                flags |= D3D12_CLEAR_FLAG_STENCIL;
            if (flags != 0) {
                list_->ClearDepthStencilView(depth_target, flags, desc.depth_stencil.clear_value.depth, static_cast<UINT8>(desc.depth_stencil.clear_value.stencil), 0, nullptr);
            }
        }

        if (desc.shading_rate_attachment.is_valid()) {
            if (list5_ == nullptr) {
                return unsupported("begin_render_pass: a shading-rate attachment was supplied but Variable Rate "
                                   "Shading is unavailable on this command list.");
            }
            const TextureViewRecord *view = device_->texture_views_.find(desc.shading_rate_attachment);
            const TextureRecord *texture = view != nullptr ? device_->textures_.find(view->texture) : nullptr;
            if (texture == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "begin_render_pass: shading_rate_attachment is not a valid texture view.");
            }
            list5_->RSSetShadingRateImage(texture->resource.Get());
        }

        bind_descriptor_heaps();
        pass_open_ = true;
        return unique_ptr<rhi::RenderPassEncoder>(std::make_unique<D3D12RenderPassEncoder>(
            *this,
            desc.allow_bundles,
            std::move(color_resolves),
            depth_resolve));
    }

    /// Performs the begin compute pass operation for `D3D12` using the supplied arguments.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::ComputePassEncoder>> D3D12CommandEncoder::begin_compute_pass(
        const rhi::ComputePassDesc &) {
        if (finished_ || pass_open_) {
            return operation_failed("begin_compute_pass: the command encoder is finished or already has an open pass.");
        }
        if (record_.list_type == D3D12_COMMAND_LIST_TYPE_COPY) {
            return unsupported("begin_compute_pass: a copy-queue command encoder cannot record dispatches.");
        }
        bind_descriptor_heaps();
        pass_open_ = true;
        return unique_ptr<rhi::ComputePassEncoder>(std::make_unique<D3D12ComputePassEncoder>(*this));
    }

    /// Performs the d3 d12 render pass encoder operation for `D3D12` using the supplied arguments.
    ///
    /// @param parent `parent` value used by the operation.
    /// @param bundles_only `bundles_only` value used by the operation.
    /// @param color_resolves `color_resolves` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12RenderPassEncoder::D3D12RenderPassEncoder(D3D12CommandEncoder &parent, bool bundles_only, std::vector<ColorResolve> color_resolves,
                                                   std::optional<DepthResolve> depth_resolve)
        : parent_(&parent), color_resolves_(std::move(color_resolves)), depth_resolve_(depth_resolve), bundles_only_(bundles_only) {}
    /// Destroys the `D3D12` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    D3D12RenderPassEncoder::~D3D12RenderPassEncoder() { end(); }

    /// Sets the pipeline for this `D3D12`.
    ///
    /// @param pipeline Pipeline used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_pipeline(rhi::RenderPipelineHandle pipeline) {
        const RenderPipelineRecord *record = parent_->device_->render_pipelines_.find(pipeline);
        if (ended_ || record == nullptr) {
            parent_->fail("set_pipeline: unknown render pipeline or ended pass.");
            return;
        }
        parent_->list_->SetPipelineState(record->pipeline.Get());
        parent_->list_->IASetPrimitiveTopology(record->topology);
        if (parent_->graphics_bindings_.layout != record->layout) {
            parent_->graphics_bindings_.layout = record->layout;
            parent_->graphics_bindings_.layout_dirty = true;
            parent_->graphics_bindings_.push_constants.clear();
            parent_->graphics_bindings_.push_constants_dirty = false;
        }
        pipeline_bound_ = true;
        mesh_pipeline_bound_ = record->is_mesh_pipeline;
        vertex_strides_ = record->vertex_strides;
        if (!mesh_pipeline_bound_) {
            for (u32 slot = 0; slot < vertex_strides_.size(); ++slot) {
                if (vertex_buffers_[slot].buffer.is_valid()) {
                    bind_vertex_buffer(slot);
                }
            }
        }
    }
    /// Sets the bind group for this `D3D12`.
    ///
    /// @param index Zero-based index of the target element or entry.
    /// @param group `group` value used by the operation.
    /// @param offsets Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_bind_group(u32 index, rhi::BindGroupHandle group, span<const u32> offsets) {
        if (ended_ || index >= max_tracked_bind_groups || !group.is_valid()) {
            parent_->fail("set_bind_group: invalid index, handle, or ended pass.");
            return;
        }
        auto &pending = parent_->graphics_bindings_.groups[index];
        pending.handle = group;
        pending.dynamic_offsets.assign(offsets.begin(), offsets.end());
        pending.dirty = true;
    }
    /// Binds vertex buffer for subsequent operations.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::bind_vertex_buffer(u32 slot) {
        if (!pipeline_bound_ || mesh_pipeline_bound_ || slot >= vertex_strides_.size()) {
            parent_->fail("set_vertex_buffer: the slot is not declared by the bound vertex pipeline.");
            return;
        }
        const PendingVertexBuffer &pending = vertex_buffers_[slot];
        const BufferRecord *record = parent_->device_->buffers_.find(pending.buffer);
        if (record == nullptr || pending.offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Vertex)) {
            parent_->fail("set_vertex_buffer: invalid buffer, usage, or offset.");
            return;
        }
        const u64 remaining = record->size - pending.offset;
        const D3D12_VERTEX_BUFFER_VIEW view{
            record->gpu_address + pending.offset,
            static_cast<UINT>(std::min<u64>(remaining, std::numeric_limits<UINT>::max())),
            vertex_strides_[slot],
        };
        parent_->list_->IASetVertexBuffers(slot, 1, &view);
    }
    /// Sets the vertex buffer for this `D3D12`.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || slot >= vertex_buffers_.size() || record == nullptr || offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Vertex)) {
            parent_->fail("set_vertex_buffer: invalid slot, buffer, usage, offset, or ended pass.");
            return;
        }
        vertex_buffers_[slot] = PendingVertexBuffer{.buffer = buffer, .offset = offset};
        if (pipeline_bound_) {
            bind_vertex_buffer(slot);
        }
    }
    /// Sets the index buffer for this `D3D12`.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || record == nullptr || offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Index)) {
            parent_->fail("set_index_buffer: invalid buffer, usage, offset, or ended pass.");
            return;
        }
        const D3D12_INDEX_BUFFER_VIEW view{
            record->gpu_address + offset,
            static_cast<UINT>(std::min<u64>(record->size - offset, std::numeric_limits<UINT>::max())),
            to_dxgi(format),
        };
        parent_->list_->IASetIndexBuffer(&view);
    }
    /// Sets the push constants for this `D3D12`.
    ///
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_push_constants(rhi::ShaderStage, u32 offset, span<const std::byte> data) {
        if (ended_ || offset % 4 != 0 || data.size() % 4 != 0) {
            parent_->fail("set_push_constants: data and offset must be four-byte aligned.");
            return;
        }
        auto &constants = parent_->graphics_bindings_.push_constants;
        if (constants.size() < offset + data.size())
            constants.resize(offset + data.size());
        std::memcpy(constants.data() + offset, data.data(), data.size());
        parent_->graphics_bindings_.push_constants_dirty = true;
    }
    /// Sets the viewport for this `D3D12` render pass, passed through unmodified.
    ///
    /// @param value RHI viewport, authored against the engine's +Y-up clip-space convention.
    ///
    /// @note D3D12 is the RHI's native/reference clip-space convention (see RHI::Viewport's own doc
    ///       comment), so `value` is used as-is — no Y flip, no origin shift. Vulkan is the one
    ///       backend that reconciles its own opposite native NDC Y, and it does so via a
    ///       negative-height viewport (VulkanRhiBridgeCommands.cpp), not here.
    ///       D3D12_VIEWPORT::Height is documented as non-negative, so D3D12 never gets that treatment.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_viewport(const rhi::Viewport &value) {
        if (ended_) {
            parent_->fail("set_viewport: ended pass.");
            return;
        }
        const D3D12_VIEWPORT v{value.x, value.y, value.width, value.height, value.min_depth, value.max_depth};
        parent_->list_->RSSetViewports(1, &v);
    }
    /// Sets the scissor for this `D3D12`.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_scissor(const rhi::Rect2D &value) {
        if (ended_) {
            parent_->fail("set_scissor: ended pass.");
            return;
        }


        const D3D12_RECT r{value.x, value.y, value.x + static_cast<LONG>(value.width), value.y + static_cast<LONG>(value.height)};
        parent_->list_->RSSetScissorRects(1, &r);
    }
    /// Sets custom MSAA sample positions for this `D3D12`.
    ///
    /// @param samples_per_pixel Number of samples each pixel in `grid_size` provides locations for.
    /// @param grid_size Repeating pixel-pattern size the locations apply across.
    /// @param locations `samples_per_pixel * grid_size.width * grid_size.height` positions.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size,
                                                      span<const rhi::SampleLocation> locations) {
        if (ended_ || parent_->list1_ == nullptr) {
            parent_->fail("set_sample_locations: ended pass or SetSamplePositions unavailable on this command list.");
            return;
        }
        std::vector<D3D12_SAMPLE_POSITION> positions;
        positions.reserve(locations.size());
        for (const rhi::SampleLocation &location : locations) {
            positions.push_back(to_d3d12(location));
        }
        parent_->list1_->SetSamplePositions(samples_per_pixel, grid_size.width * grid_size.height, positions.data());
    }
    /// Sets the blend constant for this `D3D12`.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_blend_constant(const rhi::ClearColor &color) {
        if (ended_) {
            parent_->fail("set_blend_constant: ended pass.");
            return;
        }
        const float v[] = {color.r, color.g, color.b, color.a};
        parent_->list_->OMSetBlendFactor(v);
    }
    /// Sets the stencil reference for this `D3D12`.
    ///
    /// @param reference `reference` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_stencil_reference(u32 reference) {
        if (ended_) {
            parent_->fail("set_stencil_reference: ended pass.");
            return;
        }
        parent_->list_->OMSetStencilRef(reference);
    }
    /// Sets the depth bounds test range for this `D3D12`.
    ///
    /// @param min_depth Lower bound of the depth range that passes the test, in [0, 1].
    /// @param max_depth Upper bound of the depth range that passes the test, in [0, 1].
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_depth_bounds(f32 min_depth, f32 max_depth) {
        if (ended_ || parent_->list1_ == nullptr) {
            parent_->fail("set_depth_bounds: ended pass or OMSetDepthBounds unavailable on this command list.");
            return;
        }
        parent_->list1_->OMSetDepthBounds(min_depth, max_depth);
    }
    /// Sets the pipeline-stage shading rate and its combiners for this `D3D12`.
    ///
    /// @param rate Base shading rate for subsequent draws, before any combiner runs.
    /// @param primitive_combiner Combines `rate` with a draw's per-primitive shading rate output.
    /// @param attachment_combiner Combines the result with the bound shading-rate attachment's rate.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::set_shading_rate(rhi::ShadingRate rate, rhi::ShadingRateCombiner primitive_combiner,
                                                  rhi::ShadingRateCombiner attachment_combiner) {
        if (ended_ || parent_->list5_ == nullptr) {
            parent_->fail("set_shading_rate: ended pass or Variable Rate Shading unavailable on this command list.");
            return;
        }
        const D3D12_SHADING_RATE_COMBINER combiners[2] = {to_d3d12(primitive_combiner), to_d3d12(attachment_combiner)};
        parent_->list5_->RSSetShadingRate(to_d3d12(rate), combiners);
    }

    /// Draws the requested content using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw(const rhi::DrawArgs &args) {
        if (ended_ || bundles_only_) {
            parent_->fail("draw: direct draws are not legal in this pass.");
            return;
        }
        if (!pipeline_bound_ || mesh_pipeline_bound_) {
            parent_->fail("draw: a vertex pipeline must be bound before drawing.");
            return;
        }
        if (!parent_->flush_bindings(parent_->graphics_bindings_, true)) {
            return;
        }
        parent_->list_->DrawInstanced(args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
    }
    /// Draws indexed using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indexed(const rhi::DrawIndexedArgs &args) {
        if (ended_ || bundles_only_) {
            parent_->fail("draw_indexed: direct draws are not legal in this pass.");
            return;
        }
        if (!pipeline_bound_ || mesh_pipeline_bound_) {
            parent_->fail("draw_indexed: a vertex pipeline must be bound before drawing.");
            return;
        }
        if (!parent_->flush_bindings(parent_->graphics_bindings_, true)) {
            return;
        }
        parent_->list_->DrawIndexedInstanced(args.index_count, args.instance_count, args.first_index, args.base_vertex, args.first_instance);
    }
    /// Draws mesh tasks using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) {
        if (ended_ || bundles_only_ || parent_->list6_ == nullptr) {
            parent_->fail("draw_mesh_tasks: mesh dispatch is unavailable or not legal in this pass.");
            return;
        }
        if (!pipeline_bound_ || !mesh_pipeline_bound_) {
            parent_->fail("draw_mesh_tasks: a mesh pipeline must be bound before dispatching mesh tasks.");
            return;
        }
        if (!parent_->flush_bindings(parent_->graphics_bindings_, true)) {
            return;
        }
        parent_->list6_->DispatchMesh(args.group_count_x, args.group_count_y, args.group_count_z);
    }

    /// Records indirect using the supplied arguments and current state.
    ///
    /// @param kind `kind` value used by the operation.
    /// @param indirect `indirect` value used by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param count Number of elements or operations to process.
    /// @param count_offset Offset from the beginning of the relevant range or buffer.
    /// @param max_draws `max_draws` value used by the operation.
    /// @param stride `stride` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::record_indirect(D3D12Device::IndirectKind kind, rhi::BufferHandle indirect, u64 offset, rhi::BufferHandle count, u64 count_offset, u32 max_draws, u32 stride) {
        const BufferRecord *arguments = parent_->device_->buffers_.find(indirect);
        const BufferRecord *counter = count.is_valid() ? parent_->device_->buffers_.find(count) : nullptr;
        if (ended_ || bundles_only_ || arguments == nullptr || (count.is_valid() && counter == nullptr) ||
            !rhi::has_any(arguments->usage, rhi::BufferUsage::Indirect) ||
            (counter != nullptr && !rhi::has_any(counter->usage, rhi::BufferUsage::Indirect))) {
            parent_->fail("indirect draw: invalid buffer usage or pass state.");
            return;
        }
        const u64 argument_size = kind == D3D12Device::IndirectKind::Draw
                                      ? sizeof(D3D12_DRAW_ARGUMENTS)
                                  : kind == D3D12Device::IndirectKind::DrawIndexed
                                      ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                                      : sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        const u64 required_arguments = max_draws == 0
                                           ? 0
                                           : static_cast<u64>(max_draws - 1) * stride + argument_size;
        if (offset > arguments->size || required_arguments > arguments->size - offset ||
            (counter != nullptr && (count_offset % sizeof(u32) != 0 || count_offset > counter->size ||
                                    sizeof(u32) > counter->size - count_offset))) {
            parent_->fail("indirect draw: argument or count range exceeds its buffer.");
            return;
        }
        auto signature = parent_->device_->indirect_signature(kind, stride);
        if (!signature) {
            parent_->fail(signature.error().message);
            return;
        }
        const bool mesh_command = kind == D3D12Device::IndirectKind::DispatchMesh;
        if (!pipeline_bound_ || mesh_pipeline_bound_ != mesh_command || (mesh_command && parent_->list6_ == nullptr)) {
            parent_->fail("indirect draw command is incompatible with the bound render pipeline.");
            return;
        }
        if (!parent_->flush_bindings(parent_->graphics_bindings_, true)) {
            return;
        }
        parent_->list_->ExecuteIndirect(*signature, max_draws, arguments->resource.Get(), offset, counter ? counter->resource.Get() : nullptr, count_offset);
    }
    /// Draws indirect using the current rendering state.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indirect(rhi::BufferHandle b, u64 o) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, {}, 0, 1, sizeof(D3D12_DRAW_ARGUMENTS)); }
    /// Draws indexed indirect using the current rendering state.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle b, u64 o) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, {}, 0, 1, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)); }
    /// Draws indirect using the current rendering state.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    /// @param n `n` value used by the operation.
    /// @param s `s` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indirect(rhi::BufferHandle b, u64 o, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, {}, 0, n, s); }
    /// Draws indexed indirect using the current rendering state.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    /// @param n `n` value used by the operation.
    /// @param s `s` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle b, u64 o, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, {}, 0, n, s); }
    /// Returns the draw indirect count for this `D3D12`.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    /// @param c `c` value used by the operation.
    /// @param co `co` value used by the operation.
    /// @param n `n` value used by the operation.
    /// @param s `s` value used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indirect_count(rhi::BufferHandle b, u64 o, rhi::BufferHandle c, u64 co, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, c, co, n, s); }
    /// Returns the draw indexed indirect count for this `D3D12`.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    /// @param c `c` value used by the operation.
    /// @param co `co` value used by the operation.
    /// @param n `n` value used by the operation.
    /// @param s `s` value used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_indexed_indirect_count(rhi::BufferHandle b, u64 o, rhi::BufferHandle c, u64 co, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, c, co, n, s); }
    /// Draws mesh tasks indirect using the current rendering state.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_mesh_tasks_indirect(rhi::BufferHandle b, u64 o) {
        record_indirect(D3D12Device::IndirectKind::DispatchMesh, b, o, {}, 0, 1,
                        sizeof(D3D12_DISPATCH_MESH_ARGUMENTS));
    }
    /// Returns the draw mesh tasks indirect count for this `D3D12`.
    ///
    /// @param b `b` value used by the operation.
    /// @param o `o` value used by the operation.
    /// @param c `c` value used by the operation.
    /// @param co `co` value used by the operation.
    /// @param n `n` value used by the operation.
    /// @param s `s` value used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::draw_mesh_tasks_indirect_count(
        rhi::BufferHandle b, u64 o, rhi::BufferHandle c, u64 co, u32 n, u32 s) {
        record_indirect(D3D12Device::IndirectKind::DispatchMesh, b, o, c, co, n, s);
    }
    /// Executes bundles.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::execute_bundles(span<const rhi::RenderBundleHandle> bundles) {
        if (ended_) {
            parent_->fail("execute_bundles: ended pass.");
            return;
        }
        if (bundles.empty()) {
            return;
        }

        // A bundle's own SetDescriptorHeaps (set once in create_render_bundle_encoder) must match
        // whatever heaps are bound on this list while ExecuteBundle runs -- see
        // D3D12RenderBundleEncoder's own class doc comment. Switch onto the same persistent bundle
        // heaps for the duration of this call, then switch back to this pass's per-frame ring heaps
        // afterward so any direct draws later in the same pass see the right heaps again.
        ID3D12DescriptorHeap *bundle_heaps[] = {parent_->device_->bundle_resource_descriptors_.heap(),
                                                parent_->device_->bundle_sampler_descriptors_.heap()};
        parent_->list_->SetDescriptorHeaps(2, bundle_heaps);

        for (rhi::RenderBundleHandle handle : bundles) {
            const RenderBundleRecord *record = parent_->device_->render_bundles_.find(handle);
            if (record == nullptr || record->list == nullptr) {
                parent_->fail("execute_bundles: unknown render bundle handle.");
                continue;
            }
            if (record->viewport.has_value()) {
                parent_->list_->RSSetViewports(1, &*record->viewport);
            }
            if (record->scissor.has_value()) {
                parent_->list_->RSSetScissorRects(1, &*record->scissor);
            }
            if (record->sample_locations.has_value() && parent_->list1_ != nullptr) {
                const RenderBundleRecord::SampleLocations &locations = *record->sample_locations;
                parent_->list1_->SetSamplePositions(locations.samples_per_pixel,
                                                     locations.grid_width * locations.grid_height,
                                                     const_cast<D3D12_SAMPLE_POSITION *>(locations.positions.data()));
            }
            parent_->list_->ExecuteBundle(record->list.Get());
        }

        // Per the D3D12 spec, "after a bundle executes, all state on the command list is undefined,
        // except the pipeline state object and primitive topology" -- so this pass's own
        // pipeline/root-signature/bind-group/vertex-buffer tracking is no longer trustworthy even
        // though nothing here actually changed those members. Force set_pipeline() to be called again
        // (which re-establishes vertex buffers too, see its own body) before any further direct draw,
        // and force a full root-signature + bind-group re-flush once it is, rather than silently
        // drawing against GPU state a bundle may have clobbered.
        pipeline_bound_ = false;
        parent_->graphics_bindings_.layout_dirty = true;

        parent_->bind_descriptor_heaps();
    }
    /// Performs the begin occlusion query operation for `D3D12` using the supplied arguments.
    ///
    /// @param set `set` value used by the operation.
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::begin_occlusion_query(rhi::QuerySetHandle set, u32 index) {
        const QuerySetRecord *q = parent_->device_->query_sets_.find(set);
        if (ended_ || q == nullptr || q->type != rhi::QueryType::Occlusion) {
            parent_->fail("begin_occlusion_query: invalid query set or pass state.");
            return;
        }
        parent_->list_->BeginQuery(q->heap.Get(), to_d3d12_query_type(q->type, parent_->device_->enabled_features_.has(rhi::Feature::PreciseOcclusionQueries)), index);
        occlusion_query_set_ = set;
        occlusion_query_index_ = index;
    }
    /// Returns the current or globally available end occlusion query value.
    ///
    /// @return Returns the current end occlusion query value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::end_occlusion_query() {
        const QuerySetRecord *q = parent_->device_->query_sets_.find(occlusion_query_set_);
        if (ended_ || q == nullptr) {
            parent_->fail("end_occlusion_query: no matching query is open.");
            return;
        }
        parent_->list_->EndQuery(q->heap.Get(), to_d3d12_query_type(q->type, parent_->device_->enabled_features_.has(rhi::Feature::PreciseOcclusionQueries)), occlusion_query_index_);
        occlusion_query_set_ = {};
    }
    /// Returns the one-past-the-end iterator for the range.
    ///
    /// @return Returns the one-past-the-end iterator.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderPassEncoder::end() {
        if (ended_)
            return;
        if (occlusion_query_set_.is_valid())
            end_occlusion_query();
        for (const ColorResolve &resolve : color_resolves_) {
            parent_->list_->ResolveSubresource(resolve.destination, resolve.destination_subresource, resolve.source, resolve.source_subresource, resolve.format);
        }
        if (depth_resolve_.has_value() && parent_->list1_ != nullptr) {
            const DepthResolve &resolve = *depth_resolve_;
            parent_->list1_->ResolveSubresourceRegion(resolve.destination, resolve.destination_subresource, 0, 0,
                                                       resolve.source, resolve.source_subresource, nullptr,
                                                       resolve.format, resolve.resolve_mode);
        }
        ended_ = true;
        parent_->pass_open_ = false;
    }

    /// Performs the d3 d12 compute pass encoder operation for `D3D12` using the supplied arguments.
    ///
    /// @param parent `parent` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12ComputePassEncoder::D3D12ComputePassEncoder(D3D12CommandEncoder &parent) : parent_(&parent) {}
    /// Destroys the `D3D12` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    D3D12ComputePassEncoder::~D3D12ComputePassEncoder() { end(); }
    /// Sets the pipeline for this `D3D12`.
    ///
    /// @param pipeline Pipeline used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::set_pipeline(rhi::ComputePipelineHandle pipeline) {
        const ComputePipelineRecord *record = parent_->device_->compute_pipelines_.find(pipeline);
        if (ended_ || record == nullptr) {
            parent_->fail("set_pipeline: unknown compute pipeline or ended pass.");
            return;
        }
        parent_->list_->SetPipelineState(record->pipeline.Get());
        if (parent_->compute_bindings_.layout != record->layout) {
            parent_->compute_bindings_.layout = record->layout;
            parent_->compute_bindings_.layout_dirty = true;
            parent_->compute_bindings_.push_constants.clear();
            parent_->compute_bindings_.push_constants_dirty = false;
        }
    }
    /// Sets the bind group for this `D3D12`.
    ///
    /// @param index Zero-based index of the target element or entry.
    /// @param group `group` value used by the operation.
    /// @param offsets Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::set_bind_group(u32 index, rhi::BindGroupHandle group, span<const u32> offsets) {
        if (ended_ || index >= max_tracked_bind_groups || !group.is_valid()) {
            parent_->fail("set_bind_group: invalid index, handle, or ended pass.");
            return;
        }
        auto &pending = parent_->compute_bindings_.groups[index];
        pending.handle = group;
        pending.dynamic_offsets.assign(offsets.begin(), offsets.end());
        pending.dirty = true;
    }
    /// Sets the push constants for this `D3D12`.
    ///
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::set_push_constants(rhi::ShaderStage, u32 offset, span<const std::byte> data) {
        if (ended_ || offset % 4 != 0 || data.size() % 4 != 0) {
            parent_->fail("set_push_constants: data and offset must be four-byte aligned.");
            return;
        }
        auto &constants = parent_->compute_bindings_.push_constants;
        if (constants.size() < offset + data.size())
            constants.resize(offset + data.size());
        std::memcpy(constants.data() + offset, data.data(), data.size());
        parent_->compute_bindings_.push_constants_dirty = true;
    }
    /// Dispatches the requested work.
    ///
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param z `z` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::dispatch(u32 x, u32 y, u32 z) {
        if (ended_) {
            parent_->fail("dispatch: ended pass.");
            return;
        }
        if (!parent_->flush_bindings(parent_->compute_bindings_, false)) {
            return;
        }
        parent_->list_->Dispatch(x, y, z);
    }
    /// Dispatches indirect.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::dispatch_indirect(rhi::BufferHandle buffer, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || record == nullptr || !rhi::has_any(record->usage, rhi::BufferUsage::Indirect) ||
            offset > record->size || sizeof(D3D12_DISPATCH_ARGUMENTS) > record->size - offset) {
            parent_->fail("dispatch_indirect: invalid buffer usage, range, or ended pass.");
            return;
        }
        auto signature = parent_->device_->indirect_signature(D3D12Device::IndirectKind::Dispatch, sizeof(D3D12_DISPATCH_ARGUMENTS));
        if (!signature) {
            parent_->fail(signature.error().message);
            return;
        }
        if (!parent_->flush_bindings(parent_->compute_bindings_, false)) {
            return;
        }
        parent_->list_->ExecuteIndirect(*signature, 1, record->resource.Get(), offset, nullptr, 0);
    }
    /// Returns the one-past-the-end iterator for the range.
    ///
    /// @return Returns the one-past-the-end iterator.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12ComputePassEncoder::end() {
        if (!ended_) {
            ended_ = true;
            parent_->pass_open_ = false;
        }
    }

    /// Constructs a `D3D12RenderBundleEncoder` from the supplied initialization values.
    ///
    /// @param device Device used or affected by the operation.
    /// @param record `record` value used by the operation; ownership of its allocator/list moves in.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12RenderBundleEncoder::D3D12RenderBundleEncoder(D3D12Device &device, RenderBundleRecord record)
        : device_(&device), record_(std::move(record)) {
        (void)record_.list.As(&list1_);
        (void)record_.list.As(&list6_);
    }
    /// Destroys the `D3D12RenderBundleEncoder` and releases resources owned by it.
    ///
    /// @note This function does not throw exceptions.
    D3D12RenderBundleEncoder::~D3D12RenderBundleEncoder() = default;

    /// Records the fail state for subsequent operations, keeping only the first failure recorded.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void D3D12RenderBundleEncoder::fail(std::string message) noexcept {
        if (!deferred_error_.has_value()) {
            deferred_error_ = rhi::RhiError{rhi::RhiErrorCode::InvalidArgument, std::move(message)};
        }
    }

    /// Rejects an indirect draw call, which D3D12 bundles cannot legally record.
    ///
    /// @param operation `operation` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::reject_indirect(const char *operation) noexcept {
        fail(std::string(operation) + ": ExecuteIndirect is not legal inside a D3D12 render bundle.");
    }

    /// Sets the pipeline for this `D3D12RenderBundleEncoder`.
    ///
    /// @param pipeline Pipeline used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_pipeline(rhi::RenderPipelineHandle pipeline) {
        const RenderPipelineRecord *record = device_->render_pipelines_.find(pipeline);
        if (finished_ || record == nullptr) {
            fail("set_pipeline: unknown render pipeline or finished bundle.");
            return;
        }
        record_.list->SetPipelineState(record->pipeline.Get());
        record_.list->IASetPrimitiveTopology(record->topology);
        if (bindings_.layout != record->layout) {
            bindings_.layout = record->layout;
            bindings_.layout_dirty = true;
            bindings_.push_constants.clear();
            bindings_.push_constants_dirty = false;
        }
        pipeline_bound_ = true;
        mesh_pipeline_bound_ = record->is_mesh_pipeline;
        vertex_strides_ = record->vertex_strides;
        if (!mesh_pipeline_bound_) {
            for (u32 slot = 0; slot < vertex_strides_.size(); ++slot) {
                if (vertex_buffers_[slot].buffer.is_valid()) {
                    bind_vertex_buffer(slot);
                }
            }
        }
    }
    /// Sets the bind group for this `D3D12RenderBundleEncoder`.
    ///
    /// @param index Zero-based index of the target element or entry.
    /// @param group `group` value used by the operation.
    /// @param offsets Offset from the beginning of the relevant range or buffer.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_bind_group(u32 index, rhi::BindGroupHandle group, span<const u32> offsets) {
        if (finished_ || index >= max_tracked_bind_groups || !group.is_valid()) {
            fail("set_bind_group: invalid index, handle, or finished bundle.");
            return;
        }
        auto &pending = bindings_.groups[index];
        pending.handle = group;
        pending.dynamic_offsets.assign(offsets.begin(), offsets.end());
        pending.dirty = true;
        record_.referenced_bind_groups.push_back(group);
    }
    /// Binds vertex buffer for subsequent operations.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::bind_vertex_buffer(u32 slot) {
        if (!pipeline_bound_ || mesh_pipeline_bound_ || slot >= vertex_strides_.size()) {
            fail("set_vertex_buffer: the slot is not declared by the bound vertex pipeline.");
            return;
        }
        const PendingVertexBuffer &pending = vertex_buffers_[slot];
        const BufferRecord *record = device_->buffers_.find(pending.buffer);
        if (record == nullptr || pending.offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Vertex)) {
            fail("set_vertex_buffer: invalid buffer, usage, or offset.");
            return;
        }
        const u64 remaining = record->size - pending.offset;
        const D3D12_VERTEX_BUFFER_VIEW view{
            record->gpu_address + pending.offset,
            static_cast<UINT>(std::min<u64>(remaining, std::numeric_limits<UINT>::max())),
            vertex_strides_[slot],
        };
        record_.list->IASetVertexBuffers(slot, 1, &view);
    }
    /// Sets the vertex buffer for this `D3D12RenderBundleEncoder`.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param buffer Buffer used or affected by the operation.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) {
        const BufferRecord *record = device_->buffers_.find(buffer);
        if (finished_ || slot >= vertex_buffers_.size() || record == nullptr || offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Vertex)) {
            fail("set_vertex_buffer: invalid slot, buffer, usage, offset, or finished bundle.");
            return;
        }
        vertex_buffers_[slot] = PendingVertexBuffer{.buffer = buffer, .offset = offset};
        if (pipeline_bound_) {
            bind_vertex_buffer(slot);
        }
    }
    /// Sets the index buffer for this `D3D12RenderBundleEncoder`.
    ///
    /// @param buffer Buffer used or affected by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param offset Offset from the beginning of the relevant range or buffer.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset) {
        const BufferRecord *record = device_->buffers_.find(buffer);
        if (finished_ || record == nullptr || offset > record->size ||
            !rhi::has_any(record->usage, rhi::BufferUsage::Index)) {
            fail("set_index_buffer: invalid buffer, usage, offset, or finished bundle.");
            return;
        }
        const D3D12_INDEX_BUFFER_VIEW view{
            record->gpu_address + offset,
            static_cast<UINT>(std::min<u64>(record->size - offset, std::numeric_limits<UINT>::max())),
            to_dxgi(format),
        };
        record_.list->IASetIndexBuffer(&view);
    }
    /// Sets the push constants for this `D3D12RenderBundleEncoder`.
    ///
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_push_constants(rhi::ShaderStage, u32 offset, span<const std::byte> data) {
        if (finished_ || offset % 4 != 0 || data.size() % 4 != 0) {
            fail("set_push_constants: data and offset must be four-byte aligned.");
            return;
        }
        auto &constants = bindings_.push_constants;
        if (constants.size() < offset + data.size())
            constants.resize(offset + data.size());
        std::memcpy(constants.data() + offset, data.data(), data.size());
        bindings_.push_constants_dirty = true;
    }
    /// Captures the viewport for this `D3D12RenderBundleEncoder` instead of recording it.
    ///
    /// @param value RHI viewport, authored against the engine's +Y-up clip-space convention.
    ///
    /// @note RSSetViewports is not a legal call inside a D3D12_COMMAND_LIST_TYPE_BUNDLE command list
    ///       (D3D12 always inherits viewport/scissor from the list that executes the bundle); the
    ///       last value given here is instead applied by D3D12RenderPassEncoder::execute_bundles() on
    ///       its own DIRECT list immediately before ExecuteBundle. See RenderBundleRecord::viewport's
    ///       own doc comment.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_viewport(const rhi::Viewport &value) {
        if (finished_) {
            fail("set_viewport: finished bundle.");
            return;
        }
        record_.viewport = D3D12_VIEWPORT{value.x, value.y, value.width, value.height, value.min_depth, value.max_depth};
    }
    /// Captures the scissor for this `D3D12RenderBundleEncoder` instead of recording it.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @note See set_viewport()'s own doc comment -- RSSetScissorRects is equally illegal inside a bundle.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_scissor(const rhi::Rect2D &value) {
        if (finished_) {
            fail("set_scissor: finished bundle.");
            return;
        }
        record_.scissor = D3D12_RECT{value.x, value.y, value.x + static_cast<LONG>(value.width),
                                     value.y + static_cast<LONG>(value.height)};
    }
    /// Captures custom MSAA sample positions for this `D3D12RenderBundleEncoder` instead of recording
    /// them.
    ///
    /// @param samples_per_pixel Number of samples each pixel in `grid_size` provides locations for.
    /// @param grid_size Repeating pixel-pattern size the locations apply across.
    /// @param locations `samples_per_pixel * grid_size.width * grid_size.height` positions.
    ///
    /// @note See set_viewport()'s own doc comment -- SetSamplePositions gets the same
    /// capture-and-hoist treatment for the same "uncertain-to-illegal inside a bundle" reason.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_sample_locations(u32 samples_per_pixel, rhi::Extent2D grid_size,
                                                        span<const rhi::SampleLocation> locations) {
        if (finished_) {
            fail("set_sample_locations: finished bundle.");
            return;
        }
        RenderBundleRecord::SampleLocations captured{
            .samples_per_pixel = samples_per_pixel,
            .grid_width = grid_size.width,
            .grid_height = grid_size.height,
        };
        captured.positions.reserve(locations.size());
        for (const rhi::SampleLocation &location : locations) {
            captured.positions.push_back(to_d3d12(location));
        }
        record_.sample_locations = std::move(captured);
    }
    /// Sets the blend constant for this `D3D12RenderBundleEncoder`.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_blend_constant(const rhi::ClearColor &color) {
        if (finished_) {
            fail("set_blend_constant: finished bundle.");
            return;
        }
        const float v[] = {color.r, color.g, color.b, color.a};
        record_.list->OMSetBlendFactor(v);
    }
    /// Sets the stencil reference for this `D3D12RenderBundleEncoder`.
    ///
    /// @param reference `reference` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_stencil_reference(u32 reference) {
        if (finished_) {
            fail("set_stencil_reference: finished bundle.");
            return;
        }
        record_.list->OMSetStencilRef(reference);
    }
    /// Sets the depth bounds test range for this `D3D12RenderBundleEncoder`.
    ///
    /// @param min_depth Lower bound of the depth range that passes the test, in [0, 1].
    /// @param max_depth Upper bound of the depth range that passes the test, in [0, 1].
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::set_depth_bounds(f32 min_depth, f32 max_depth) {
        if (finished_ || list1_ == nullptr) {
            fail("set_depth_bounds: finished bundle or OMSetDepthBounds unavailable on this command list.");
            return;
        }
        list1_->OMSetDepthBounds(min_depth, max_depth);
    }

    /// Uploads pending bind-group descriptor tables into the device's persistent bundle allocator and
    /// issues the matching SetGraphicsRootDescriptorTable/root-view calls.
    ///
    /// @return Returns `true` on success; `false` after recording a failure via fail().
    /// @note Unlike D3D12CommandEncoder::flush_bindings, there is no "allocate a fresh heap and retry"
    /// path on exhaustion: PersistentShaderVisibleDescriptorAllocator deliberately never grows past
    /// its one fixed heap (see that class's own doc comment), so running out here is a hard failure.
    bool D3D12RenderBundleEncoder::flush_bindings() {
        const PipelineLayoutRecord *layout = device_->pipeline_layouts_.find(bindings_.layout);
        if (layout == nullptr) {
            fail("A draw was recorded before a pipeline (and therefore a pipeline layout) was bound.");
            return false;
        }

        if (bindings_.layout_dirty) {
            record_.list->SetGraphicsRootSignature(layout->root_signature.Get());
            for (PendingBindGroup &group : bindings_.groups) {
                if (group.handle.is_valid()) {
                    group.dirty = true;
                }
            }
            bindings_.push_constants_dirty = !bindings_.push_constants.empty();
            bindings_.layout_dirty = false;
        }

        for (u32 set_index = 0; set_index < max_tracked_bind_groups && set_index < layout->sets.size();
             ++set_index) {
            PendingBindGroup &pending = bindings_.groups[set_index];
            if (!pending.handle.is_valid() || !pending.dirty) {
                continue;
            }
            const BindGroupRecord *group = device_->bind_groups_.find(pending.handle);
            if (group == nullptr) {
                fail("set_bind_group was given a bind group handle that has since been destroyed.");
                return false;
            }
            const BindGroupLayoutRecord *group_layout = device_->bind_group_layouts_.find(group->layout);
            if (group_layout == nullptr) {
                fail("A bound bind group's layout has been destroyed.");
                return false;
            }
            if (set_index >= layout->set_layouts.size() || group->layout != layout->set_layouts[set_index]) {
                fail("A bound bind group's layout is incompatible with the current pipeline layout at that set index.");
                return false;
            }
            if (pending.dynamic_offsets.size() != group_layout->dynamic_slots.size()) {
                fail("set_bind_group supplied the wrong number of dynamic offsets for its layout.");
                return false;
            }

            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> resource_table;
            std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> sampler_table;
            if (group->resources.is_valid()) {
                auto range = device_->bundle_resource_descriptors_.allocate(group->resources.count);
                if (!range) {
                    fail(range.error().message);
                    return false;
                }
                device_->device_->CopyDescriptorsSimple(
                    group->resources.count,
                    device_->bundle_resource_descriptors_.cpu_handle(*range, 0),
                    device_->cpu_resource_descriptors_.cpu_handle(group->resources, 0),
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                resource_table = device_->bundle_resource_descriptors_.gpu_handle(*range, 0);
                record_.resource_table_ranges.push_back(*range);
            }
            if (group->samplers.is_valid()) {
                auto range = device_->bundle_sampler_descriptors_.allocate(group->samplers.count);
                if (!range) {
                    fail(range.error().message);
                    return false;
                }
                device_->device_->CopyDescriptorsSimple(
                    group->samplers.count,
                    device_->bundle_sampler_descriptors_.cpu_handle(*range, 0),
                    device_->cpu_sampler_descriptors_.cpu_handle(group->samplers, 0),
                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                sampler_table = device_->bundle_sampler_descriptors_.gpu_handle(*range, 0);
                record_.sampler_table_ranges.push_back(*range);
            }

            const SetRootParameters &mapping = layout->sets[set_index];
            if (resource_table.has_value() && mapping.resource_table >= 0) {
                record_.list->SetGraphicsRootDescriptorTable(static_cast<UINT>(mapping.resource_table), *resource_table);
            }
            if (sampler_table.has_value() && mapping.sampler_table >= 0) {
                record_.list->SetGraphicsRootDescriptorTable(static_cast<UINT>(mapping.sampler_table), *sampler_table);
            }

            for (usize slot = 0; slot < group_layout->dynamic_slots.size(); ++slot) {
                if (slot >= mapping.dynamic_root_parameters.size() || slot >= group->dynamic_addresses.size()) {
                    break;
                }
                const D3D12_GPU_VIRTUAL_ADDRESS base = group->dynamic_addresses[slot];
                if (base == 0) {
                    fail("A dynamic-offset binding was never given a buffer when its bind group was created.");
                    return false;
                }
                const u64 dynamic_offset = slot < pending.dynamic_offsets.size() ? pending.dynamic_offsets[slot] : 0;
                const UINT parameter = static_cast<UINT>(mapping.dynamic_root_parameters[slot]);
                const D3D12_GPU_VIRTUAL_ADDRESS address = base + dynamic_offset;
                switch (group_layout->dynamic_slots[slot].type) {
                    case rhi::BindingType::UniformBuffer:
                        record_.list->SetGraphicsRootConstantBufferView(parameter, address);
                        break;
                    case rhi::BindingType::StorageBuffer:
                        record_.list->SetGraphicsRootUnorderedAccessView(parameter, address);
                        break;
                    default:
                        record_.list->SetGraphicsRootShaderResourceView(parameter, address);
                        break;
                }
            }
            pending.dirty = false;
        }

        if (bindings_.push_constants.size() > static_cast<usize>(layout->push_constant_values) * sizeof(u32)) {
            fail("Push-constant data exceeds the current pipeline layout's declared range.");
            return false;
        }
        if (bindings_.push_constants_dirty && layout->push_constant_root_parameter >= 0) {
            const UINT values = static_cast<UINT>(bindings_.push_constants.size() / 4);
            if (values > 0) {
                record_.list->SetGraphicsRoot32BitConstants(static_cast<UINT>(layout->push_constant_root_parameter),
                                                             values, bindings_.push_constants.data(), 0);
            }
            bindings_.push_constants_dirty = false;
        }
        return !deferred_error_.has_value();
    }

    /// Draws the requested content using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::draw(const rhi::DrawArgs &args) {
        if (finished_ || !pipeline_bound_ || mesh_pipeline_bound_) {
            fail("draw: a vertex pipeline must be bound before drawing, or the bundle is finished.");
            return;
        }
        if (!flush_bindings()) {
            return;
        }
        record_.list->DrawInstanced(args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
    }
    /// Draws indexed using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::draw_indexed(const rhi::DrawIndexedArgs &args) {
        if (finished_ || !pipeline_bound_ || mesh_pipeline_bound_) {
            fail("draw_indexed: a vertex pipeline must be bound before drawing, or the bundle is finished.");
            return;
        }
        if (!flush_bindings()) {
            return;
        }
        record_.list->DrawIndexedInstanced(args.index_count, args.instance_count, args.first_index, args.base_vertex,
                                           args.first_instance);
    }
    /// Draws mesh tasks using the current rendering state.
    ///
    /// @param args `args` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void D3D12RenderBundleEncoder::draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) {
        if (finished_ || list6_ == nullptr || !pipeline_bound_ || !mesh_pipeline_bound_) {
            fail("draw_mesh_tasks: mesh dispatch is unavailable, no mesh pipeline is bound, or the bundle is finished.");
            return;
        }
        if (!flush_bindings()) {
            return;
        }
        list6_->DispatchMesh(args.group_count_x, args.group_count_y, args.group_count_z);
    }
    /// Draws indirect using the current rendering state.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indirect(rhi::BufferHandle, u64) { reject_indirect("draw_indirect"); }
    /// Draws indexed indirect using the current rendering state.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indexed_indirect(rhi::BufferHandle, u64) { reject_indirect("draw_indexed_indirect"); }
    /// Draws indirect using the current rendering state.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indirect(rhi::BufferHandle, u64, u32, u32) { reject_indirect("draw_indirect"); }
    /// Draws indexed indirect using the current rendering state.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indexed_indirect(rhi::BufferHandle, u64, u32, u32) { reject_indirect("draw_indexed_indirect"); }
    /// Returns the draw indirect count for this `D3D12RenderBundleEncoder`.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indirect_count(rhi::BufferHandle, u64, rhi::BufferHandle, u64, u32, u32) {
        reject_indirect("draw_indirect_count");
    }
    /// Returns the draw indexed indirect count for this `D3D12RenderBundleEncoder`.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_indexed_indirect_count(rhi::BufferHandle, u64, rhi::BufferHandle, u64, u32, u32) {
        reject_indirect("draw_indexed_indirect_count");
    }
    /// Draws mesh tasks indirect using the current rendering state.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_mesh_tasks_indirect(rhi::BufferHandle, u64) { reject_indirect("draw_mesh_tasks_indirect"); }
    /// Returns the draw mesh tasks indirect count for this `D3D12RenderBundleEncoder`.
    ///
    /// @note ExecuteIndirect is not a legal call inside a D3D12 bundle -- see reject_indirect()'s own doc comment.
    void D3D12RenderBundleEncoder::draw_mesh_tasks_indirect_count(rhi::BufferHandle, u64, rhi::BufferHandle, u64, u32, u32) {
        reject_indirect("draw_mesh_tasks_indirect_count");
    }

    /// Returns the current or globally available finish value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::RenderBundleHandle> D3D12RenderBundleEncoder::finish() {
        if (finished_) {
            return operation_failed("finish: this render bundle encoder has already finished.");
        }
        finished_ = true;
        if (deferred_error_.has_value()) {
            return std::unexpected(*deferred_error_);
        }
        if (const HRESULT hr = record_.list->Close(); FAILED(hr)) {
            return hresult_error(hr, "finish (Close)");
        }
        return device_->render_bundles_.insert(std::move(record_));
    }

    /// Creates a command encoder from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> D3D12Device::create_command_encoder(const rhi::CommandEncoderDesc &desc) {
        if (device_ == nullptr)
            return operation_failed("create_command_encoder: device is not initialized.");
        if (desc.queue.queue != rhi::QueueClass::Graphics && desc.queue.queue != rhi::QueueClass::Compute && desc.queue.queue != rhi::QueueClass::Transfer)
            return unsupported("create_command_encoder: this D3D12 backend supports graphics, compute, and transfer queues only.");
        if (auto valid = validate_queue_lane(desc.queue, "create_command_encoder"); !valid)
            return std::unexpected(valid.error());
        auto record = acquire_command_buffer(desc.queue);
        if (!record)
            return std::unexpected(record.error());
        return unique_ptr<rhi::CommandEncoder>(std::make_unique<D3D12CommandEncoder>(*this, std::move(*record)));
    }
    /// Creates a render bundle encoder from the supplied parameters.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> D3D12Device::create_render_bundle_encoder(
        const rhi::RenderBundleDesc &desc) {
        if (device_ == nullptr) {
            return device_not_ready<unique_ptr<rhi::RenderBundleEncoder>>("create_render_bundle_encoder");
        }

        RenderBundleRecord record{};
        if (const HRESULT hr =
                device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&record.allocator));
            FAILED(hr)) {
            return hresult_error(hr, "create_render_bundle_encoder (CreateCommandAllocator)");
        }
        if (const HRESULT hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, record.allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&record.list));
            FAILED(hr)) {
            return hresult_error(hr, "create_render_bundle_encoder (CreateCommandList)");
        }
        set_debug_name(record.list.Get(), desc.label);

        // D3D12 requires a bundle to call SetDescriptorHeaps itself with the same heap pointers the
        // executing (DIRECT) command list has bound -- see D3D12RenderBundleEncoder's own class doc
        // comment and D3D12RenderPassEncoder::execute_bundles(), which switches onto these same
        // persistent bundle heaps before ExecuteBundle for exactly this reason. Bound once here,
        // unconditionally, since every bundle uses the persistent allocator for its bind groups.
        ID3D12DescriptorHeap *bundle_heaps[] = {bundle_resource_descriptors_.heap(), bundle_sampler_descriptors_.heap()};
        record.list->SetDescriptorHeaps(2, bundle_heaps);

        // D3D12 bundles carry no render-target/format declaration the way Vulkan secondary command
        // buffers need one for VkCommandBufferInheritanceRenderingInfo -- a bundle is just state-set
        // and draw calls, replayed into whatever render targets happen to be bound on the executing
        // list. Nothing here needs these fields.
        (void)desc.color_formats;
        (void)desc.depth_stencil_format;
        (void)desc.samples;
        (void)desc.view_mask;

        return unique_ptr<rhi::RenderBundleEncoder>(std::make_unique<D3D12RenderBundleEncoder>(*this, std::move(record)));
    }
    /// Performs the indirect signature operation for `D3D12` using the supplied arguments.
    ///
    /// @param kind `kind` value used by the operation.
    /// @param stride `stride` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<ID3D12CommandSignature *> D3D12Device::indirect_signature(IndirectKind kind, u32 stride) {
        D3D12_INDIRECT_ARGUMENT_TYPE type{};
        u32 minimum_stride = 0;
        switch (kind) {
            case IndirectKind::Draw:
                type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
                minimum_stride = sizeof(D3D12_DRAW_ARGUMENTS);
                break;
            case IndirectKind::DrawIndexed:
                type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
                minimum_stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
                break;
            case IndirectKind::Dispatch:
                type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                minimum_stride = sizeof(D3D12_DISPATCH_ARGUMENTS);
                break;
            case IndirectKind::DispatchMesh:
                type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
                minimum_stride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
                break;
            default:
                return unsupported("indirect_signature: this indirect command kind is not implemented.");
        }
        if (stride < minimum_stride)
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "indirect_signature: argument stride is smaller than the D3D12 argument structure.");
        const u64 key = (static_cast<u64>(kind) << 32) | stride;
        auto signatures = indirect_signatures_.lock();
        if (auto it = signatures->find(key); it != signatures->end())
            return it->second.Get();
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = type;
        const D3D12_COMMAND_SIGNATURE_DESC desc{.ByteStride = stride, .NumArgumentDescs = 1, .pArgumentDescs = &argument, .NodeMask = 0};
        ComPtr<ID3D12CommandSignature> signature;
        if (const HRESULT hr = device_->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)); FAILED(hr))
            return hresult_error(hr, "indirect_signature (CreateCommandSignature)");
        ID3D12CommandSignature *result = signature.Get();
        signatures->emplace(key, std::move(signature));
        return result;
    }

} // namespace SFT::D3D12
