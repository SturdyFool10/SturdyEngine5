





#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    rhi::RhiExpected<rhi::QuerySetHandle> D3D12Device::create_query_set(const rhi::QuerySetDesc &desc) {
        ZoneScopedN("D3D12Device::create_query_set");
        if (desc.count == 0) {
            return invalid_argument("create_query_set: count must be non-zero.");
        }

        QuerySetRecord record{};
        record.type = desc.type;
        record.count = desc.count;
        record.statistics = desc.statistics;

        const D3D12_QUERY_HEAP_DESC heap_desc{
            .Type = to_d3d12_query_heap_type(desc.type),
            .Count = desc.count,
            .NodeMask = 0,
        };
        if (const HRESULT hr = device_->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&record.heap)); FAILED(hr)) {
            return hresult_error(hr, "create_query_set (CreateQueryHeap)");
        }
        set_debug_name(record.heap.Get(), desc.label);

        record.readback_bytes = query_result_bytes(desc.type) * desc.count;
        const CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC readback_desc = CD3DX12_RESOURCE_DESC::Buffer(record.readback_bytes);
        if (const HRESULT hr = device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&record.readback));
            FAILED(hr)) {
            return hresult_error(hr, "create_query_set (readback CreateCommittedResource)");
        }
        set_debug_name(record.readback.Get(), "Sturdy query readback");

        return query_sets_.insert(std::move(record));
    }

    void D3D12Device::destroy_query_set(rhi::QuerySetHandle handle) noexcept { query_sets_.erase(handle); }

    void D3D12Device::reset_query_set(rhi::QuerySetHandle, u32, u32) noexcept {




    }

    rhi::RhiResult D3D12Device::get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count, span<std::byte> dst, u64 stride, rhi::QueryResultFlags flags) {
        ZoneScopedN("D3D12Device::get_query_set_results");
        QuerySetRecord *record = query_sets_.find(query_set);
        if (record == nullptr) {
            return invalid_argument("get_query_set_results: unknown query set handle.");
        }
        if (count == 0) {
            return {};
        }
        if (first + count > record->count) {
            return invalid_argument("get_query_set_results: the requested slot range exceeds the query set's size.");
        }
        if (!rhi::has_any(flags, rhi::QueryResultFlags::Result64Bit)) {




            return unsupported("get_query_set_results: D3D12 resolves query results as 64-bit values; "
                               "QueryResultFlags::Result64Bit is required.");
        }
        if (rhi::has_any(flags, rhi::QueryResultFlags::WithAvailability | rhi::QueryResultFlags::Partial)) {


            return unsupported("get_query_set_results: D3D12 supports neither an availability integer nor "
                               "partial query results.");
        }

        const u64 element_bytes = query_result_bytes(record->type);
        const u64 effective_stride = stride != 0 ? stride : element_bytes;
        if (dst.size() < effective_stride * (count - 1) + element_bytes) {
            return invalid_argument("get_query_set_results: the destination span is too small for the requested "
                                    "slot count and stride.");
        }



        auto command = acquire_command_buffer(rhi::QueueLane{rhi::QueueClass::Graphics, 0});
        if (!command) {
            return std::unexpected(command.error());
        }
        command->list->ResolveQueryData(record->heap.Get(),
                                        to_d3d12_query_type(record->type,
                                                            enabled_features_.has(rhi::Feature::PreciseOcclusionQueries)),
                                        first,
                                        count,
                                        record->readback.Get(),
                                        0);
        if (const HRESULT hr = command->list->Close(); FAILED(hr)) {
            return_command_buffer(std::move(*command));
            return hresult_error(hr, "get_query_set_results (Close)");
        }





        const rhi::RhiResult executed = execute_and_wait(command->list.Get(), rhi::QueueClass::Graphics);
        return_command_buffer(std::move(*command));
        if (!executed) {
            return executed;
        }

        void *mapped = nullptr;
        const CD3DX12_RANGE read_range(0, static_cast<SIZE_T>(element_bytes * count));
        if (const HRESULT hr = record->readback->Map(0, &read_range, &mapped); FAILED(hr)) {
            return hresult_error(hr, "get_query_set_results (Map)");
        }
        const auto *source = static_cast<const std::byte *>(mapped);
        for (u32 index = 0; index < count; ++index) {
            std::memcpy(dst.data() + effective_stride * index, source + element_bytes * index, static_cast<usize>(element_bytes));
        }
        const CD3DX12_RANGE no_write(0, 0);
        record->readback->Unmap(0, &no_write);
        return {};
    }

} // namespace SFT::D3D12
