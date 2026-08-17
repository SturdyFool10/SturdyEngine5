#pragma once






















#include <D3D12/D3D12Common.hpp>

#pragma region Imports
#include <optional>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>

namespace SFT::D3D12 {

    /// A contiguous run of `count` descriptors owned by a CpuDescriptorAllocator. Contiguity is not a
    /// convenience here: a D3D12 descriptor table is addressed as "base handle + offset", so every
    /// descriptor a bind group exposes through one table must be adjacent in the heap.
    struct DescriptorRange {
        u32 chunk = 0;
        u32 offset = 0;
        u32 count = 0;

        [[nodiscard]] bool is_valid() const noexcept;
    };

    /// A chained allocator of non-shader-visible descriptors of one heap type. Grows by adding fixed-
    /// size chunks; freed ranges go onto a per-size free list and are handed back out before any new
    /// chunk space is consumed. Never shrinks — a descriptor heap is cheap to hold and re-creating one
    /// whenever live usage briefly dipped would reintroduce exactly the per-allocation heap-creation
    /// cost this exists to avoid (the same reasoning the Vulkan bridge's descriptor-pool chunks use).
    ///
    /// Internally synchronized: bind-group and texture-view creation are documented as callable
    /// concurrently, and both allocate from here.
    class CpuDescriptorAllocator {
      public:
        CpuDescriptorAllocator() = default;

        CpuDescriptorAllocator(const CpuDescriptorAllocator &) = delete;
        CpuDescriptorAllocator &operator=(const CpuDescriptorAllocator &) = delete;

        [[nodiscard]] rhi::RhiResult initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 chunk_capacity);

        [[nodiscard]] rhi::RhiExpected<DescriptorRange> allocate(u32 count);
        void release(const DescriptorRange &range) noexcept;

        /// The CPU handle of descriptor `index` within `range`. Asserts nothing — an out-of-range index
        /// is a backend bug, and every caller derives the index from the same layout that sized the
        /// range.
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(const DescriptorRange &range, u32 index) const noexcept;

        [[nodiscard]] u32 increment() const noexcept;
        [[nodiscard]] D3D12_DESCRIPTOR_HEAP_TYPE heap_type() const noexcept;

      private:
        struct Chunk {
            ComPtr<ID3D12DescriptorHeap> heap;
            D3D12_CPU_DESCRIPTOR_HANDLE start{};
            u32 cursor = 0;
        };

        struct State {
            std::vector<Chunk> chunks;
            /// Keyed by descriptor count, because a table's size is fixed by its layout: a freed
            /// 6-descriptor range is only ever reusable by another 6-descriptor request. Splitting or
            /// coalescing ranges would need a real suballocator; descriptor demand in practice is a
            /// small number of distinct sizes reused constantly, which an exact-fit free list serves
            /// without any of that machinery.
            std::unordered_map<u32, std::vector<DescriptorRange>> free_ranges;
        };

        ID3D12Device *device_ = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE type_ = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        u32 chunk_capacity_ = 0;
        u32 increment_ = 0;
        mutable Async::Mutex<State> state_;
    };

    /// One shader-visible heap, used as a linear bump allocator for the lifetime of a single command
    /// list's recording. Not internally synchronized: a CommandEncoder is documented single-threaded,
    /// and this belongs to exactly one encoder at a time.
    ///
    /// `reset()` rewinds the cursor for reuse by the next encoder that checks this heap out of the
    /// device's free list — safe only once that encoder's prior submission has completed, which is the
    /// same condition the RHI already places on destroy_command_buffer().
    class ShaderVisibleDescriptorHeap {
      public:
        [[nodiscard]] rhi::RhiResult initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);

        /// Reserves `count` adjacent descriptors, or nullopt when this heap is full — the caller's
        /// signal to swap in a fresh heap and re-upload its currently-bound tables (see
        /// D3D12CommandEncoder's own handling; a table's GPU handle is only valid while the heap it
        /// was allocated from is the bound one).
        [[nodiscard]] std::optional<u32> allocate(u32 count) noexcept;

        void reset() noexcept;

        [[nodiscard]] ID3D12DescriptorHeap *heap() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] u32 increment() const noexcept;
        [[nodiscard]] u32 capacity() const noexcept;

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(u32 index) const noexcept;
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(u32 index) const noexcept;

      private:
        ComPtr<ID3D12DescriptorHeap> heap_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
        u32 capacity_ = 0;
        u32 cursor_ = 0;
        u32 increment_ = 0;
    };

    /// Default capacities. The CBV/SRV/UAV figure is a per-encoder scratch budget, not a global limit —
    /// it caps how many descriptors one command list may *bind* (not own), and an encoder that exhausts
    /// it transparently switches to a fresh heap. The sampler figure is the hardware maximum:
    /// D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE is 2048 on every resource-binding tier, so a sampler
    /// heap simply cannot be made larger.
    inline constexpr u32 default_shader_visible_resource_descriptors = 16384;
    inline constexpr u32 default_shader_visible_sampler_descriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    inline constexpr u32 default_cpu_descriptor_chunk_capacity = 4096;
    inline constexpr u32 default_cpu_rtv_chunk_capacity = 256;
    inline constexpr u32 default_cpu_dsv_chunk_capacity = 64;

} // namespace SFT::D3D12
