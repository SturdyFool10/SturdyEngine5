#include <D3D12/D3D12CommandEncoder.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {

        [[nodiscard]] u32 subresource_index(const TextureRecord &texture, u32 mip, u32 layer) noexcept {
            return mip + layer * texture.mip_levels;
        }

        // D3D12 requires that a barrier naming no sync scope also name no access, and vice versa —
        // the two are validated against each other rather than being independent. An access mask
        // paired with SYNC_NONE is the common way to get this wrong, so it is normalized here once
        // instead of at each of the three barrier kinds.
        void normalize_sync_access(D3D12_BARRIER_SYNC &sync, D3D12_BARRIER_ACCESS &access) noexcept {
            if (sync == D3D12_BARRIER_SYNC_NONE) {
                access = D3D12_BARRIER_ACCESS_NO_ACCESS;
            } else if (access == D3D12_BARRIER_ACCESS_NO_ACCESS) {
                sync = D3D12_BARRIER_SYNC_NONE;
            }
        }

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
                // Depth-stencil resources have two planes; naming only plane 0 would leave the stencil
                // aspect untransitioned, which the RHI's single-layout model gives no way to express
                // separately.
                .NumPlanes = rhi::format_has_stencil(texture.format) ? 2u : 1u,
            };
        }

    } // namespace

    void BindingState::reset() noexcept {
        layout = {};
        for (PendingBindGroup &group : groups) {
            group = PendingBindGroup{};
        }
        push_constants.clear();
        push_constants_dirty = false;
        layout_dirty = false;
    }

    // ─── D3D12CommandEncoder ─────────────────────────────────────────────────────

    D3D12CommandEncoder::D3D12CommandEncoder(D3D12Device &device, CommandBufferRecord &&record)
        : device_(&device), record_(std::move(record)), list_(record_.list.Get()) {
        (void)record_.list.As(&list4_);
        (void)record_.list.As(&list6_);
        if (device_->enhanced_barriers_) {
            (void)record_.list.As(&list7_);
        }
        bind_descriptor_heaps();
    }

    D3D12CommandEncoder::~D3D12CommandEncoder() {
        if (!finished_ && record_.list != nullptr) {
            // An abandoned encoder still owns a live, open command list and allocator. Closing it makes
            // the pair recyclable again; leaking them would slowly starve the free list of a
            // long-running process that hits error paths.
            (void)record_.list->Close();
            device_->return_command_buffer(std::move(record_));
        }
    }

    void D3D12CommandEncoder::fail(std::string message) noexcept {
        if (!deferred_error_.has_value()) {
            deferred_error_ = rhi::RhiError{rhi::RhiErrorCode::InvalidArgument, std::move(message)};
        }
    }

    void D3D12CommandEncoder::bind_descriptor_heaps() {
        if (record_.list_type == D3D12_COMMAND_LIST_TYPE_COPY || !record_.resource_heap.is_valid()) {
            return;
        }
        ID3D12DescriptorHeap *heaps[] = {record_.resource_heap.heap(), record_.sampler_heap.heap()};
        const UINT heap_count = record_.sampler_heap.is_valid() ? 2u : 1u;
        list_->SetDescriptorHeaps(heap_count, heaps);
    }

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

    void D3D12CommandEncoder::flush_bindings(BindingState &state, bool graphics) {
        const PipelineLayoutRecord *layout = device_->pipeline_layouts_.find(state.layout);
        if (layout == nullptr) {
            fail("A draw or dispatch was recorded before a pipeline (and therefore a pipeline layout) was bound.");
            return;
        }

        if (state.layout_dirty) {
            if (graphics) {
                list_->SetGraphicsRootSignature(layout->root_signature.Get());
            } else {
                list_->SetComputeRootSignature(layout->root_signature.Get());
            }
            // A root signature change discards every root argument, so everything must be re-applied —
            // this is why binds are retained rather than consumed (see D3D12CommandEncoder.hpp).
            for (PendingBindGroup &group : state.groups) {
                if (group.handle.is_valid()) {
                    group.dirty = true;
                }
            }
            state.push_constants_dirty = !state.push_constants.empty();
            state.layout_dirty = false;
        }

        // Two attempts at most: the second follows a shader-visible heap swap, which cannot itself fail
        // for capacity reasons because a fresh heap is empty.
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
                    return;
                }
                const BindGroupLayoutRecord *group_layout = device_->bind_group_layouts_.find(group->layout);
                if (group_layout == nullptr) {
                    fail("A bound bind group's layout has been destroyed.");
                    return;
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
                        return;
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
                return;
            }

            // Retire the exhausted heaps rather than freeing them: commands already recorded into this
            // list still reference descriptors inside them, so they must stay alive until the whole
            // record is recycled.
            if (auto resource_heap = device_->create_shader_visible_heap(
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    default_shader_visible_resource_descriptors)) {
                record_.retired_heaps.push_back(std::move(record_.resource_heap));
                record_.resource_heap = std::move(*resource_heap);
            } else {
                fail("Allocating a replacement shader-visible descriptor heap failed.");
                return;
            }
            if (auto sampler_heap = device_->create_shader_visible_heap(
                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                    default_shader_visible_sampler_descriptors)) {
                record_.retired_heaps.push_back(std::move(record_.sampler_heap));
                record_.sampler_heap = std::move(*sampler_heap);
            } else {
                fail("Allocating a replacement shader-visible sampler heap failed.");
                return;
            }
            bind_descriptor_heaps();
            for (PendingBindGroup &group : state.groups) {
                if (group.handle.is_valid()) {
                    group.dirty = true;
                }
            }
        }

        if (state.push_constants_dirty && layout->push_constant_root_parameter >= 0) {
            const UINT values = std::min<UINT>(layout->push_constant_values,
                                               static_cast<UINT>(state.push_constants.size() / 4));
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
    }

    // ─── Barriers ────────────────────────────────────────────────────────────────

    void D3D12CommandEncoder::legacy_transition(TextureRecord &texture, u32 subresource, D3D12_RESOURCE_STATES after) {
        if (subresource >= texture.legacy_states.size()) {
            return;
        }
        const D3D12_RESOURCE_STATES before = texture.legacy_states[subresource];
        if (before == after) {
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
                // `ownership` is dropped: D3D12 has no queue-family ownership to transfer (a resource is
                // visible to every queue on the device), which is exactly the case
                // QueueOwnershipTransfer documents as "backends without ownership transfers ignore it".
                buffers.push_back(D3D12_BUFFER_BARRIER{
                    sync_before,
                    sync_after,
                    access_before,
                    access_after,
                    buffer->resource.Get(),
                    source.offset,
                    // A whole-buffer barrier must be spelled as size UINT64_MAX rather than the real
                    // size; D3D12 rejects a range that is merely equal to the resource size.
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
                    // Coming from UNDEFINED means the prior contents are being thrown away, and D3D12
                    // requires the source access to say so explicitly.
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

        // ── Legacy fallback ──
        // A single ResourceBarrier batch, with the (stage, access, layout) triple collapsed onto one
        // D3D12_RESOURCE_STATES value per resource. The information loss is real and unavoidable; it is
        // also why Feature::Synchronization2 is not reported on devices that land here.
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

        for (const rhi::GlobalBarrier &source : global_barriers) {
            // The only legacy equivalent of a global memory dependency is a UAV barrier with a null
            // resource ("all UAV writes before this must complete"), which covers the shader-write case
            // global barriers are overwhelmingly used for.
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
            // Host-visible buffers are permanently pinned in GENERIC_READ/COPY_DEST and cannot be
            // transitioned at all, so a barrier on one is correctly a no-op rather than an error.
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

    // ─── Copies and clears ───────────────────────────────────────────────────────

    void D3D12CommandEncoder::copy_buffer_to_buffer(rhi::BufferHandle src, rhi::BufferHandle dst, const rhi::BufferCopy &region) {
        const BufferRecord *source = device_->buffers_.find(src);
        const BufferRecord *destination = device_->buffers_.find(dst);
        if (source == nullptr || destination == nullptr) {
            fail("copy_buffer_to_buffer names an unknown buffer handle.");
            return;
        }
        const u64 size = region.size != 0 ? region.size : source->size - region.src_offset;
        list_->CopyBufferRegion(destination->resource.Get(), region.dst_offset, source->resource.Get(), region.src_offset, size);
    }

    namespace {

        // A buffer<->texture copy footprint. D3D12 describes the buffer side as a placed footprint whose
        // RowPitch must be a multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) — a hard hardware
        // requirement with no equivalent in Vulkan's bufferRowLength, and the single most common source
        // of silently-wrong texture uploads when ported naively. It is validated rather than rounded:
        // rounding would change which bytes the copy reads, producing a skewed image instead of an error.
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

    void D3D12CommandEncoder::copy_buffer_to_texture(rhi::BufferHandle src, rhi::TextureHandle dst, const rhi::BufferTextureCopy &region) {
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

    void D3D12CommandEncoder::copy_texture_to_buffer(rhi::TextureHandle src, rhi::BufferHandle dst, const rhi::BufferTextureCopy &region) {
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

    void D3D12CommandEncoder::copy_texture_to_texture(rhi::TextureHandle src, rhi::TextureHandle dst, const rhi::TextureCopy &region) {
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

    void D3D12CommandEncoder::blit_texture(rhi::TextureHandle, rhi::TextureHandle, const rhi::TextureBlit &, rhi::Filter) {
        // D3D12 has no scaled, filtered texture blit. vkCmdBlitImage's closest relatives are
        // CopyTextureRegion (same size, no filtering) and ResolveSubresource (MSAA only, no scaling) —
        // neither of which can do what this asks. A real implementation is a full-screen draw through
        // a sampler, which means a pipeline, a root signature, and a shader the RHI has no way to
        // supply from inside a backend. Reported rather than silently doing an unscaled copy, which
        // would produce a wrong image with no diagnostic at all: the mip-generation and bloom paths
        // this exists for must use a compute or raster downsample pass on D3D12.
        fail("blit_texture: D3D12 has no scaled/filtered image blit. Use a compute or raster downsample pass "
             "instead (this is why the operation is reported here rather than approximated).");
    }

    void D3D12CommandEncoder::fill_buffer(rhi::BufferHandle buffer, u64 offset, u64 size, u32 value) {
        // ClearUnorderedAccessViewUint needs both a shader-visible and a non-shader-visible descriptor
        // for the same UAV, and the RHI's fill_buffer carries no view. Building one here per call would
        // mean allocating two descriptors mid-recording; instead the buffer's own Storage usage is
        // required and the clear goes through a transient UAV in the list's shader-visible heap.
        const BufferRecord *record = device_->buffers_.find(buffer);
        if (record == nullptr) {
            fail("fill_buffer names an unknown buffer handle.");
            return;
        }
        if (!rhi::has_any(record->usage, rhi::BufferUsage::Storage)) {
            fail("fill_buffer: the buffer must have been created with BufferUsage::Storage — D3D12 clears a buffer "
                 "through an unordered-access view, which a non-Storage buffer cannot have.");
            return;
        }

        const u64 clear_size = size != 0 ? size : record->size - offset;
        auto staging = device_->cpu_resource_descriptors_.allocate(1);
        if (!staging) {
            fail("fill_buffer: allocating a staging descriptor failed.");
            return;
        }
        const std::optional<u32> heap_offset = record_.resource_heap.allocate(1);
        if (!heap_offset.has_value()) {
            device_->cpu_resource_descriptors_.release(*staging);
            fail("fill_buffer: the command list's shader-visible descriptor heap is full.");
            return;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer = {.FirstElement = offset / 4,
                      .NumElements = static_cast<UINT>(clear_size / 4),
                      .StructureByteStride = 0,
                      .CounterOffsetInBytes = 0,
                      .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
        const D3D12_CPU_DESCRIPTOR_HANDLE staging_handle = device_->cpu_resource_descriptors_.cpu_handle(*staging, 0);
        device_->device_->CreateUnorderedAccessView(record->resource.Get(), nullptr, &uav, staging_handle);
        device_->device_->CopyDescriptorsSimple(1, record_.resource_heap.cpu_handle(*heap_offset), staging_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        const UINT values[4] = {value, value, value, value};
        list_->ClearUnorderedAccessViewUint(record_.resource_heap.gpu_handle(*heap_offset), staging_handle, record->resource.Get(), values, 0, nullptr);
        // The staging descriptor must stay valid until the GPU executes the clear, so it is not
        // released here — it is retained for the life of the command record, the same lifetime rule the
        // retired heaps follow.
    }

    void D3D12CommandEncoder::update_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) {
        // D3D12 has no vkCmdUpdateBuffer — no inline data in the command stream at all. The honest
        // equivalent is a staged copy, which is what the device's own blocking upload path does; doing
        // it here would mean submitting work in the middle of recording, changing the ordering the
        // caller asked for. Reported instead, so a caller uses write_buffer() (outside recording) or a
        // per-frame upload buffer plus copy_buffer_to_buffer().
        (void)buffer;
        (void)offset;
        (void)data;
        fail("update_buffer: D3D12 has no inline command-stream buffer update. Use RhiDevice::write_buffer() "
             "outside recording, or copy from a HostUpload buffer with copy_buffer_to_buffer().");
    }

    void D3D12CommandEncoder::clear_color_texture(rhi::TextureHandle texture, const rhi::ClearColor &color, const rhi::TextureSubresourceRange &range) {
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
        const float components[4] = {color.r, color.g, color.b, color.a};
        list_->ClearRenderTargetView(handle, components, 0, nullptr);
    }

    void D3D12CommandEncoder::clear_depth_stencil_texture(rhi::TextureHandle texture,
                                                          const rhi::ClearDepthStencil &value,
                                                          const rhi::TextureSubresourceRange &range) {
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
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (rhi::format_has_stencil(record->format)) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        list_->ClearDepthStencilView(handle, flags, value.depth, static_cast<UINT8>(value.stencil), 0, nullptr);
    }

    // ─── Ray tracing commands ────────────────────────────────────────────────────

    void D3D12CommandEncoder::build_acceleration_structures(span<const rhi::AccelerationStructureBuildDesc> builds) {
        ZoneScopedN("D3D12CommandEncoder::build_acceleration_structures");
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

    void D3D12CommandEncoder::copy_acceleration_structure(const rhi::AccelerationStructureCopyDesc &copy) {
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
        // for a different one would silently get tightly-packed results instead.
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

    void D3D12CommandEncoder::push_debug_group(const char *label) {
        if (label != nullptr) {
            // PIX consumes this directly; RenderDoc and the debug layer both surface it too. The 0 is
            // PIX's "the data is a plain string" metadata code.
            list_->BeginEvent(0, label, static_cast<UINT>(std::char_traits<char>::length(label) + 1));
        }
    }

    void D3D12CommandEncoder::pop_debug_group() { list_->EndEvent(); }

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

    rhi::RhiExpected<unique_ptr<rhi::RenderPassEncoder>> D3D12CommandEncoder::begin_render_pass(
        const rhi::RenderPassDesc &desc) {
        if (finished_ || pass_open_) {
            return operation_failed("begin_render_pass: the command encoder is finished or already has an open pass.");
        }
        if (record_.list_type == D3D12_COMMAND_LIST_TYPE_COPY) {
            return unsupported("begin_render_pass: a copy-queue command encoder cannot record rendering.");
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
        if (desc.depth_stencil.view.is_valid()) {
            const TextureViewRecord *view = device_->texture_views_.find(desc.depth_stencil.view);
            if (view == nullptr || !view->dsv.is_valid()) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "begin_render_pass: the depth/stencil attachment has no valid DSV.");
            }
            if (desc.depth_stencil.resolve_view.is_valid()) {
                return unsupported("begin_render_pass: depth/stencil resolve is not implemented for D3D12.");
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

        bind_descriptor_heaps();
        pass_open_ = true;
        return unique_ptr<rhi::RenderPassEncoder>(std::make_unique<D3D12RenderPassEncoder>(
            *this,
            desc.allow_bundles,
            std::move(color_resolves)));
    }

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

    D3D12RenderPassEncoder::D3D12RenderPassEncoder(D3D12CommandEncoder &parent, bool bundles_only, std::vector<ColorResolve> color_resolves)
        : parent_(&parent), color_resolves_(std::move(color_resolves)), bundles_only_(bundles_only) {}
    D3D12RenderPassEncoder::~D3D12RenderPassEncoder() { end(); }

    void D3D12RenderPassEncoder::set_pipeline(rhi::RenderPipelineHandle pipeline) {
        const RenderPipelineRecord *record = parent_->device_->render_pipelines_.find(pipeline);
        if (ended_ || record == nullptr) {
            parent_->fail("set_pipeline: unknown render pipeline or ended pass.");
            return;
        }
        parent_->list_->SetPipelineState(record->pipeline.Get());
        parent_->list_->IASetPrimitiveTopology(record->topology);
        parent_->graphics_bindings_.layout = record->layout;
        parent_->graphics_bindings_.layout_dirty = true;
    }
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
    void D3D12RenderPassEncoder::set_vertex_buffer(u32 slot, rhi::BufferHandle buffer, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || record == nullptr || offset > record->size) {
            parent_->fail("set_vertex_buffer: invalid buffer, offset, or ended pass.");
            return;
        }
        const D3D12_VERTEX_BUFFER_VIEW view{record->gpu_address + offset, static_cast<UINT>(record->size - offset), 0};
        parent_->list_->IASetVertexBuffers(slot, 1, &view);
    }
    void D3D12RenderPassEncoder::set_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || record == nullptr || offset > record->size) {
            parent_->fail("set_index_buffer: invalid buffer, offset, or ended pass.");
            return;
        }
        const D3D12_INDEX_BUFFER_VIEW view{record->gpu_address + offset, static_cast<UINT>(record->size - offset), to_dxgi(format)};
        parent_->list_->IASetIndexBuffer(&view);
    }
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
    void D3D12RenderPassEncoder::set_viewport(const rhi::Viewport &value) {
        if (ended_) {
            parent_->fail("set_viewport: ended pass.");
            return;
        }
        // Passed through as-is. The RHI's Vulkan-style clip space (see RHI::Viewport) is reconciled
        // with D3D12's opposite NDC Y in the shader, via the clip-space ABI in sturdy_common.slang —
        // not here. A negative-height viewport would also work on the hardware, but D3D12 documents
        // D3D12_VIEWPORT::Height as non-negative, so the RHI does not rely on that.
        const D3D12_VIEWPORT v{value.x, value.y, value.width, value.height, value.min_depth, value.max_depth};
        parent_->list_->RSSetViewports(1, &v);
    }
    void D3D12RenderPassEncoder::set_scissor(const rhi::Rect2D &value) {
        if (ended_) {
            parent_->fail("set_scissor: ended pass.");
            return;
        }
        // Scissor rects are framebuffer pixels with a top-left origin in both APIs, so they need no
        // translation — only clip space differs between Vulkan and D3D12, not pixel space.
        const D3D12_RECT r{value.x, value.y, value.x + static_cast<LONG>(value.width), value.y + static_cast<LONG>(value.height)};
        parent_->list_->RSSetScissorRects(1, &r);
    }
    void D3D12RenderPassEncoder::set_blend_constant(const rhi::ClearColor &color) {
        if (ended_) {
            parent_->fail("set_blend_constant: ended pass.");
            return;
        }
        const float v[] = {color.r, color.g, color.b, color.a};
        parent_->list_->OMSetBlendFactor(v);
    }
    void D3D12RenderPassEncoder::set_stencil_reference(u32 reference) {
        if (ended_) {
            parent_->fail("set_stencil_reference: ended pass.");
            return;
        }
        parent_->list_->OMSetStencilRef(reference);
    }

    void D3D12RenderPassEncoder::draw(const rhi::DrawArgs &args) {
        if (ended_ || bundles_only_) {
            parent_->fail("draw: direct draws are not legal in this pass.");
            return;
        }
        parent_->flush_bindings(parent_->graphics_bindings_, true);
        parent_->list_->DrawInstanced(args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
    }
    void D3D12RenderPassEncoder::draw_indexed(const rhi::DrawIndexedArgs &args) {
        if (ended_ || bundles_only_) {
            parent_->fail("draw_indexed: direct draws are not legal in this pass.");
            return;
        }
        parent_->flush_bindings(parent_->graphics_bindings_, true);
        parent_->list_->DrawIndexedInstanced(args.index_count, args.instance_count, args.first_index, args.base_vertex, args.first_instance);
    }
    void D3D12RenderPassEncoder::draw_mesh_tasks(const rhi::DrawMeshTasksArgs &args) {
        if (ended_ || bundles_only_ || parent_->list6_ == nullptr) {
            parent_->fail("draw_mesh_tasks: mesh dispatch is unavailable or not legal in this pass.");
            return;
        }
        parent_->flush_bindings(parent_->graphics_bindings_, true);
        parent_->list6_->DispatchMesh(args.group_count_x, args.group_count_y, args.group_count_z);
    }

    void D3D12RenderPassEncoder::record_indirect(D3D12Device::IndirectKind kind, rhi::BufferHandle indirect, u64 offset, rhi::BufferHandle count, u64 count_offset, u32 max_draws, u32 stride) {
        const BufferRecord *arguments = parent_->device_->buffers_.find(indirect);
        const BufferRecord *counter = count.is_valid() ? parent_->device_->buffers_.find(count) : nullptr;
        if (ended_ || bundles_only_ || arguments == nullptr || (count.is_valid() && counter == nullptr)) {
            parent_->fail("indirect draw: invalid buffer or pass state.");
            return;
        }
        auto signature = parent_->device_->indirect_signature(kind, stride);
        if (!signature) {
            parent_->fail(signature.error().message);
            return;
        }
        parent_->flush_bindings(parent_->graphics_bindings_, true);
        parent_->list_->ExecuteIndirect(*signature, max_draws, arguments->resource.Get(), offset, counter ? counter->resource.Get() : nullptr, count_offset);
    }
    void D3D12RenderPassEncoder::draw_indirect(rhi::BufferHandle b, u64 o) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, {}, 0, 1, sizeof(D3D12_DRAW_ARGUMENTS)); }
    void D3D12RenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle b, u64 o) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, {}, 0, 1, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)); }
    void D3D12RenderPassEncoder::draw_indirect(rhi::BufferHandle b, u64 o, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, {}, 0, n, s); }
    void D3D12RenderPassEncoder::draw_indexed_indirect(rhi::BufferHandle b, u64 o, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, {}, 0, n, s); }
    void D3D12RenderPassEncoder::draw_indirect_count(rhi::BufferHandle b, u64 o, rhi::BufferHandle c, u64 co, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::Draw, b, o, c, co, n, s); }
    void D3D12RenderPassEncoder::draw_indexed_indirect_count(rhi::BufferHandle b, u64 o, rhi::BufferHandle c, u64 co, u32 n, u32 s) { record_indirect(D3D12Device::IndirectKind::DrawIndexed, b, o, c, co, n, s); }
    void D3D12RenderPassEncoder::draw_mesh_tasks_indirect(rhi::BufferHandle, u64) { parent_->fail("draw_mesh_tasks_indirect: D3D12 mesh indirect commands are not implemented."); }
    void D3D12RenderPassEncoder::draw_mesh_tasks_indirect_count(rhi::BufferHandle, u64, rhi::BufferHandle, u64, u32, u32) { parent_->fail("draw_mesh_tasks_indirect_count: D3D12 mesh indirect commands are not implemented."); }
    void D3D12RenderPassEncoder::execute_bundles(span<const rhi::RenderBundleHandle>) { parent_->fail("execute_bundles: D3D12 render bundles are not implemented."); }
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
    void D3D12RenderPassEncoder::end_occlusion_query() {
        const QuerySetRecord *q = parent_->device_->query_sets_.find(occlusion_query_set_);
        if (ended_ || q == nullptr) {
            parent_->fail("end_occlusion_query: no matching query is open.");
            return;
        }
        parent_->list_->EndQuery(q->heap.Get(), to_d3d12_query_type(q->type, parent_->device_->enabled_features_.has(rhi::Feature::PreciseOcclusionQueries)), occlusion_query_index_);
        occlusion_query_set_ = {};
    }
    void D3D12RenderPassEncoder::end() {
        if (ended_)
            return;
        if (occlusion_query_set_.is_valid())
            end_occlusion_query();
        for (const ColorResolve &resolve : color_resolves_) {
            parent_->list_->ResolveSubresource(resolve.destination, resolve.destination_subresource, resolve.source, resolve.source_subresource, resolve.format);
        }
        ended_ = true;
        parent_->pass_open_ = false;
    }

    D3D12ComputePassEncoder::D3D12ComputePassEncoder(D3D12CommandEncoder &parent) : parent_(&parent) {}
    D3D12ComputePassEncoder::~D3D12ComputePassEncoder() { end(); }
    void D3D12ComputePassEncoder::set_pipeline(rhi::ComputePipelineHandle pipeline) {
        const ComputePipelineRecord *record = parent_->device_->compute_pipelines_.find(pipeline);
        if (ended_ || record == nullptr) {
            parent_->fail("set_pipeline: unknown compute pipeline or ended pass.");
            return;
        }
        parent_->list_->SetPipelineState(record->pipeline.Get());
        parent_->compute_bindings_.layout = record->layout;
        parent_->compute_bindings_.layout_dirty = true;
    }
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
    void D3D12ComputePassEncoder::dispatch(u32 x, u32 y, u32 z) {
        if (ended_) {
            parent_->fail("dispatch: ended pass.");
            return;
        }
        parent_->flush_bindings(parent_->compute_bindings_, false);
        parent_->list_->Dispatch(x, y, z);
    }
    void D3D12ComputePassEncoder::dispatch_indirect(rhi::BufferHandle buffer, u64 offset) {
        const BufferRecord *record = parent_->device_->buffers_.find(buffer);
        if (ended_ || record == nullptr) {
            parent_->fail("dispatch_indirect: invalid buffer or ended pass.");
            return;
        }
        auto signature = parent_->device_->indirect_signature(D3D12Device::IndirectKind::Dispatch, sizeof(D3D12_DISPATCH_ARGUMENTS));
        if (!signature) {
            parent_->fail(signature.error().message);
            return;
        }
        parent_->flush_bindings(parent_->compute_bindings_, false);
        parent_->list_->ExecuteIndirect(*signature, 1, record->resource.Get(), offset, nullptr, 0);
    }
    void D3D12ComputePassEncoder::end() {
        if (!ended_) {
            ended_ = true;
            parent_->pass_open_ = false;
        }
    }

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
    rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> D3D12Device::create_render_bundle_encoder(const rhi::RenderBundleDesc &) {
        return unsupported("create_render_bundle_encoder: D3D12 render bundles are not implemented.");
    }
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
