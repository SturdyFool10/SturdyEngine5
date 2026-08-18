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


    /// A single fixed-capacity shader-visible descriptor heap with free-list release, used to back
    /// render-bundle bind-group uploads.
    ///
    /// Render bundles are recorded once and `ExecuteBundle`d across many later frames, so their
    /// descriptor tables cannot live in the per-frame ring allocator (ShaderVisibleDescriptorHeap
    /// above, reset every frame by D3D12Device -- see its own initialize() call site) the way a
    /// transient command encoder's bind groups do; a bundle's baked-in
    /// `SetGraphicsRootDescriptorTable` GPU handle would end up pointing at whatever unrelated bind
    /// group the ring allocator recycled that slot to on a later frame. This type gives bundle bind
    /// groups a table allocation that instead lives exactly as long as the bundle that owns it,
    /// released explicitly when the bundle is destroyed (destroy_render_bundle).
    ///
    /// Deliberately a single fixed heap, not a chunk-growing pool like CpuDescriptorAllocator: a root
    /// descriptor table must be a contiguous range within one heap, and only one CBV_SRV_UAV (and one
    /// SAMPLER) shader-visible heap can be bound via SetDescriptorHeaps at a time, so a bind group
    /// cannot straddle two separately-allocated heap objects the way CpuDescriptorAllocator's
    /// on-demand chunk growth would risk. Exhausting this heap's fixed capacity is a hard failure
    /// (RhiErrorCode::OutOfMemory-shaped), not a silent grow -- see allocate()'s own doc comment.
    class PersistentShaderVisibleDescriptorAllocator {
      public:
        /// Initializes the `PersistentShaderVisibleDescriptorAllocator` for use.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiResult initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);

        /// Allocates a contiguous descriptor range from the single backing heap.
        ///
        /// @param count Number of elements or operations to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Returns `RhiErrorCode::OutOfMemory` once the fixed capacity (and any previously
        /// released, reusable ranges) cannot satisfy the request -- this allocator never grows past
        /// its initial heap, unlike CpuDescriptorAllocator (see the class doc comment for why).
        [[nodiscard]] rhi::RhiExpected<DescriptorRange> allocate(u32 count);
        /// Releases the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param range Range of values to process.
        ///
        /// @note This function does not throw exceptions.
        void release(const DescriptorRange &range) noexcept;

        /// Returns the CPU handle associated with this `PersistentShaderVisibleDescriptorAllocator`.
        ///
        /// @param range Range of values to process.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(const DescriptorRange &range, u32 index) const noexcept;
        /// Returns the GPU handle associated with this `PersistentShaderVisibleDescriptorAllocator`.
        ///
        /// @param range Range of values to process.
        /// @param index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(const DescriptorRange &range, u32 index) const noexcept;

        /// Returns the current or globally available heap value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ID3D12DescriptorHeap *heap() const noexcept;
        /// Reports whether valid holds for this `PersistentShaderVisibleDescriptorAllocator`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

      private:
        struct State {
            u32 cursor = 0;
            std::unordered_map<u32, std::vector<DescriptorRange>> free_ranges;
        };

        ComPtr<ID3D12DescriptorHeap> heap_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
        u32 capacity_ = 0;
        u32 increment_ = 0;
        mutable Async::Mutex<State> state_;
    };


    inline constexpr u32 default_shader_visible_resource_descriptors = 16384;
    inline constexpr u32 default_shader_visible_sampler_descriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

    /// Capacity of the device-wide PersistentShaderVisibleDescriptorAllocator backing render-bundle
    /// bind groups (see that class's own doc comment). Deliberately smaller than the per-frame ring
    /// heap above: bundles are pre-baked, comparatively rare, long-lived objects, not a per-draw
    /// allocation, so a smaller fixed budget is expected to comfortably cover real usage.
    inline constexpr u32 default_persistent_bundle_resource_descriptors = 4096;
    inline constexpr u32 default_persistent_bundle_sampler_descriptors = 512;

    inline constexpr u32 default_cpu_descriptor_chunk_capacity = 4096;
    inline constexpr u32 default_cpu_rtv_chunk_capacity = 256;
    inline constexpr u32 default_cpu_dsv_chunk_capacity = 64;

} // namespace SFT::D3D12
