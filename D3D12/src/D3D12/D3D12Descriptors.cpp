#include <D3D12/src/D3D12/D3D12Descriptors.hpp>
#include <D3D12/D3D12Descriptors.hpp>

#pragma region Imports
#include <utility>
#pragma endregion

namespace SFT::D3D12 {


    /// Initializes the `D3D12` for use.
    ///
    /// @param device Device used or affected by the operation.
    /// @param type Type value to inspect, select, or convert.
    /// @param chunk_capacity `chunk_capacity` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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

    /// Allocates storage or a resource.
    ///
    /// @param count Number of elements or operations to process.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<DescriptorRange> CpuDescriptorAllocator::allocate(u32 count) {
        if (device_ == nullptr) {
            return operation_failed("CpuDescriptorAllocator::allocate: allocator was never initialized.");
        }
        if (count == 0) {


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

    /// Releases the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void CpuDescriptorAllocator::release(const DescriptorRange &range) noexcept {
        if (!range.is_valid()) {
            return;
        }
        auto state = state_.lock();
        state->free_ranges[range.count].push_back(range);
    }

    /// Returns the CPU handle associated with this `D3D12`.
    ///
    /// @param range Range of values to process.
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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


    /// Initializes the `D3D12` for use.
    ///
    /// @param device Device used or affected by the operation.
    /// @param type Type value to inspect, select, or convert.
    /// @param capacity `capacity` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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

    /// Allocates storage or a resource.
    ///
    /// @param count Number of elements or operations to process.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    std::optional<u32> ShaderVisibleDescriptorHeap::allocate(u32 count) noexcept {
        if (heap_ == nullptr || count == 0 || capacity_ - cursor_ < count) {
            return std::nullopt;
        }
        const u32 offset = cursor_;
        cursor_ += count;
        return offset;
    }

    /// Returns the CPU handle associated with this `D3D12`.
    ///
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleDescriptorHeap::cpu_handle(u32 index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_start_;
        handle.ptr += static_cast<SIZE_T>(index) * increment_;
        return handle;
    }

    /// Returns the GPU handle associated with this `D3D12`.
    ///
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleDescriptorHeap::gpu_handle(u32 index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = gpu_start_;
        handle.ptr += static_cast<UINT64>(index) * increment_;
        return handle;
    }

    /// Initializes the `PersistentShaderVisibleDescriptorAllocator` for use.
    ///
    /// @param device Device used or affected by the operation.
    /// @param type Type value to inspect, select, or convert.
    /// @param capacity `capacity` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult PersistentShaderVisibleDescriptorAllocator::initialize(ID3D12Device *device,
                                                                          D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                                          u32 capacity) {
        if (device == nullptr || capacity == 0) {
            return invalid_argument("PersistentShaderVisibleDescriptorAllocator::initialize: null device or zero capacity.");
        }
        const D3D12_DESCRIPTOR_HEAP_DESC desc{
            .Type = type,
            .NumDescriptors = capacity,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            .NodeMask = 0,
        };
        if (const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)); FAILED(hr)) {
            return hresult_error(hr, "PersistentShaderVisibleDescriptorAllocator::initialize (CreateDescriptorHeap)");
        }
        cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
        gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
        capacity_ = capacity;
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        return {};
    }

    /// Allocates a contiguous descriptor range from the single backing heap.
    ///
    /// @param count Number of elements or operations to process.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<DescriptorRange> PersistentShaderVisibleDescriptorAllocator::allocate(u32 count) {
        if (heap_ == nullptr) {
            return operation_failed("PersistentShaderVisibleDescriptorAllocator::allocate: allocator was never initialized.");
        }
        if (count == 0) {
            return DescriptorRange{};
        }

        auto state = state_.lock();

        if (auto it = state->free_ranges.find(count); it != state->free_ranges.end() && !it->second.empty()) {
            DescriptorRange range = it->second.back();
            it->second.pop_back();
            return range;
        }

        if (capacity_ - state->cursor < count) {
            return rhi::rhi_error(rhi::RhiErrorCode::OutOfMemory,
                                  "PersistentShaderVisibleDescriptorAllocator::allocate: the fixed render-bundle "
                                  "descriptor heap is exhausted; destroy unused render bundles or raise "
                                  "default_persistent_bundle_resource_descriptors/"
                                  "default_persistent_bundle_sampler_descriptors.");
        }

        const DescriptorRange range{0, state->cursor, count};
        state->cursor += count;
        return range;
    }

    /// Releases the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void PersistentShaderVisibleDescriptorAllocator::release(const DescriptorRange &range) noexcept {
        if (!range.is_valid()) {
            return;
        }
        auto state = state_.lock();
        state->free_ranges[range.count].push_back(range);
    }

    /// Returns the CPU handle associated with this `PersistentShaderVisibleDescriptorAllocator`.
    ///
    /// @param range Range of values to process.
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    D3D12_CPU_DESCRIPTOR_HANDLE PersistentShaderVisibleDescriptorAllocator::cpu_handle(const DescriptorRange &range,
                                                                                       u32 index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_start_;
        handle.ptr += static_cast<SIZE_T>(range.offset + index) * increment_;
        return handle;
    }

    /// Returns the GPU handle associated with this `PersistentShaderVisibleDescriptorAllocator`.
    ///
    /// @param range Range of values to process.
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    D3D12_GPU_DESCRIPTOR_HANDLE PersistentShaderVisibleDescriptorAllocator::gpu_handle(const DescriptorRange &range,
                                                                                       u32 index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = gpu_start_;
        handle.ptr += static_cast<UINT64>(range.offset + index) * increment_;
        return handle;
    }

    /// Returns the current or globally available heap value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    ID3D12DescriptorHeap *PersistentShaderVisibleDescriptorAllocator::heap() const noexcept { return heap_.Get(); }

    /// Reports whether valid holds for this `PersistentShaderVisibleDescriptorAllocator`.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool PersistentShaderVisibleDescriptorAllocator::is_valid() const noexcept { return heap_ != nullptr; }

} // namespace SFT::D3D12

namespace SFT::D3D12 {

    /// Reports whether valid holds for this `D3D12`.
    ///
    /// @return Returns the current is valid value.
    /// @note This function does not throw exceptions.
    bool DescriptorRange::is_valid() const noexcept { return count != 0; }

    /// Returns the current or globally available increment value.
    ///
    /// @return Returns the current increment value.
    /// @note This function does not throw exceptions.
    u32 CpuDescriptorAllocator::increment() const noexcept { return increment_; }

    /// Returns the current or globally available heap type value.
    ///
    /// @return Returns the current heap type value.
    /// @note This function does not throw exceptions.
    D3D12_DESCRIPTOR_HEAP_TYPE CpuDescriptorAllocator::heap_type() const noexcept { return type_; }

    /// Resets the object to its baseline state.
    ///
    /// @return Returns the current reset value.
    /// @note This function does not throw exceptions.
    void ShaderVisibleDescriptorHeap::reset() noexcept { cursor_ = 0; }

    /// Returns the current or globally available heap value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    ID3D12DescriptorHeap *ShaderVisibleDescriptorHeap::heap() const noexcept { return heap_.Get(); }

    /// Reports whether valid holds for this `D3D12`.
    ///
    /// @return Returns the current is valid value.
    /// @note This function does not throw exceptions.
    bool ShaderVisibleDescriptorHeap::is_valid() const noexcept { return heap_ != nullptr; }

    /// Returns the current or globally available increment value.
    ///
    /// @return Returns the current increment value.
    /// @note This function does not throw exceptions.
    u32 ShaderVisibleDescriptorHeap::increment() const noexcept { return increment_; }

    /// Returns the current or globally available capacity value.
    ///
    /// @return Returns the current capacity value.
    /// @note This function does not throw exceptions.
    u32 ShaderVisibleDescriptorHeap::capacity() const noexcept { return capacity_; }

} // namespace SFT::D3D12

