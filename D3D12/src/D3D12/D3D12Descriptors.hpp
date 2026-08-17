#pragma once


#include <D3D12/D3D12Common.hpp>

#pragma region Imports
#include <optional>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>

namespace SFT::D3D12 {


    struct DescriptorRange {
        u32 chunk = 0;
        u32 offset = 0;
        u32 count = 0;

        /// Reports whether valid holds for this `DescriptorRange`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
    };


    class CpuDescriptorAllocator {
      public:
        /// Constructs a `CpuDescriptorAllocator` in its default state.
        ///
        /// @note This function does not throw exceptions.
        CpuDescriptorAllocator() = default;

        /// Disables this construction form for `CpuDescriptorAllocator`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        CpuDescriptorAllocator(const CpuDescriptorAllocator &) = delete;
        /// Assigns a new value to this `CpuDescriptorAllocator`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        CpuDescriptorAllocator &operator=(const CpuDescriptorAllocator &) = delete;

        /// Initializes the `CpuDescriptorAllocator` for use.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param chunk_capacity `chunk_capacity` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 chunk_capacity);

        /// Allocates storage or a resource.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<DescriptorRange> allocate(u32 count);
        /// Releases the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param range Range of values to process.
        ///
        /// @note This function does not throw exceptions.
        void release(const DescriptorRange &range) noexcept;


        /// Returns the CPU handle associated with this `CpuDescriptorAllocator`.
        ///
        /// @param range Range of values to process.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(const DescriptorRange &range, u32 index) const noexcept;

        /// Returns the current or globally available increment value.
        ///
        /// @return Returns the current increment value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 increment() const noexcept;
        /// Returns the current or globally available heap type value.
        ///
        /// @return Returns the current heap type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_DESCRIPTOR_HEAP_TYPE heap_type() const noexcept;

      private:
        struct Chunk {
            ComPtr<ID3D12DescriptorHeap> heap;
            D3D12_CPU_DESCRIPTOR_HANDLE start{};
            u32 cursor = 0;
        };

        struct State {
            std::vector<Chunk> chunks;


            std::unordered_map<u32, std::vector<DescriptorRange>> free_ranges;
        };

        ID3D12Device *device_ = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE type_ = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        u32 chunk_capacity_ = 0;
        u32 increment_ = 0;
        mutable Async::Mutex<State> state_;
    };


    class ShaderVisibleDescriptorHeap {
      public:
        /// Initializes the `ShaderVisibleDescriptorHeap` for use.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);


        /// Allocates storage or a resource.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<u32> allocate(u32 count) noexcept;

        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;

        /// Returns the current or globally available heap value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12DescriptorHeap *heap() const noexcept;
        /// Reports whether valid holds for this `ShaderVisibleDescriptorHeap`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the current or globally available increment value.
        ///
        /// @return Returns the current increment value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 increment() const noexcept;
        /// Returns the current or globally available capacity value.
        ///
        /// @return Returns the current capacity value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 capacity() const noexcept;

        /// Returns the CPU handle associated with this `ShaderVisibleDescriptorHeap`.
        ///
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(u32 index) const noexcept;
        /// Returns the GPU handle associated with this `ShaderVisibleDescriptorHeap`.
        ///
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(u32 index) const noexcept;

      private:
        ComPtr<ID3D12DescriptorHeap> heap_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
        u32 capacity_ = 0;
        u32 cursor_ = 0;
        u32 increment_ = 0;
    };


    inline constexpr u32 default_shader_visible_resource_descriptors = 16384;
    inline constexpr u32 default_shader_visible_sampler_descriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    inline constexpr u32 default_cpu_descriptor_chunk_capacity = 4096;
    inline constexpr u32 default_cpu_rtv_chunk_capacity = 256;
    inline constexpr u32 default_cpu_dsv_chunk_capacity = 64;

} // namespace SFT::D3D12
