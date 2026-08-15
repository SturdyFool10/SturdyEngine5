#include <D3D12/D3D12Descriptors.hpp>

#pragma region Imports
#include <utility>
#pragma endregion

namespace SFT::D3D12 {

    // ─── CpuDescriptorAllocator ──────────────────────────────────────────────────

    rhi::RhiResult CpuDescriptorAllocator::initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                      u32 chunk_capacity) {
        if (device == nullptr || chunk_capacity == 0) {
            return invalid_argument("CpuDescriptorAllocator::initialize: null device or zero chunk capacity.");
        }
        device_ = device;
        type_ = type;
        chunk_capacity_ = chunk_capacity;
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        return {};
    }

    rhi::RhiExpected<DescriptorRange> CpuDescriptorAllocator::allocate(u32 count) {
        if (device_ == nullptr) {
            return operation_failed("CpuDescriptorAllocator::allocate: allocator was never initialized.");
        }
        if (count == 0) {
            // A bind group with (say) no samplers legitimately asks for a zero-sized sampler range.
            // An invalid range is the correct answer: it is never bound, never freed, and never
            // consumes heap space.
            return DescriptorRange{};
        }
        if (count > chunk_capacity_) {
            return operation_failed("CpuDescriptorAllocator::allocate: request of " + std::to_string(count) +
                                    " descriptors exceeds the " + std::to_string(chunk_capacity_) +
                                    "-descriptor chunk capacity.");
        }

        auto state = state_.lock();

        if (auto it = state->free_ranges.find(count); it != state->free_ranges.end() && !it->second.empty()) {
            DescriptorRange range = it->second.back();
            it->second.pop_back();
            return range;
        }

        if (!state->chunks.empty()) {
            Chunk &last = state->chunks.back();
            if (chunk_capacity_ - last.cursor >= count) {
                const DescriptorRange range{static_cast<u32>(state->chunks.size() - 1), last.cursor, count};
                last.cursor += count;
                return range;
            }
        }

        const D3D12_DESCRIPTOR_HEAP_DESC desc{
            .Type = type_,
            .NumDescriptors = chunk_capacity_,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            .NodeMask = 0,
        };
        Chunk chunk{};
        if (const HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&chunk.heap)); FAILED(hr)) {
            return hresult_error(hr, "CpuDescriptorAllocator::allocate (CreateDescriptorHeap)");
        }
        chunk.start = chunk.heap->GetCPUDescriptorHandleForHeapStart();
        chunk.cursor = count;

        const DescriptorRange range{static_cast<u32>(state->chunks.size()), 0, count};
        state->chunks.push_back(std::move(chunk));
        return range;
    }

    void CpuDescriptorAllocator::release(const DescriptorRange &range) noexcept {
        if (!range.is_valid()) {
            return;
        }
        auto state = state_.lock();
        state->free_ranges[range.count].push_back(range);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorAllocator::cpu_handle(const DescriptorRange &range,
                                                                  u32 index) const noexcept {
        auto state = state_.lock();
        if (range.chunk >= state->chunks.size()) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle = state->chunks[range.chunk].start;
        handle.ptr += static_cast<SIZE_T>(range.offset + index) * increment_;
        return handle;
    }

    // ─── ShaderVisibleDescriptorHeap ─────────────────────────────────────────────

    rhi::RhiResult ShaderVisibleDescriptorHeap::initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                           u32 capacity) {
        if (device == nullptr || capacity == 0) {
            return invalid_argument("ShaderVisibleDescriptorHeap::initialize: null device or zero capacity.");
        }
        const D3D12_DESCRIPTOR_HEAP_DESC desc{
            .Type = type,
            .NumDescriptors = capacity,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            .NodeMask = 0,
        };
        if (const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)); FAILED(hr)) {
            return hresult_error(hr, "ShaderVisibleDescriptorHeap::initialize (CreateDescriptorHeap)");
        }
        cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
        gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
        capacity_ = capacity;
        cursor_ = 0;
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        return {};
    }

    std::optional<u32> ShaderVisibleDescriptorHeap::allocate(u32 count) noexcept {
        if (heap_ == nullptr || count == 0 || capacity_ - cursor_ < count) {
            return std::nullopt;
        }
        const u32 offset = cursor_;
        cursor_ += count;
        return offset;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleDescriptorHeap::cpu_handle(u32 index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_start_;
        handle.ptr += static_cast<SIZE_T>(index) * increment_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleDescriptorHeap::gpu_handle(u32 index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = gpu_start_;
        handle.ptr += static_cast<UINT64>(index) * increment_;
        return handle;
    }

} // namespace SFT::D3D12
