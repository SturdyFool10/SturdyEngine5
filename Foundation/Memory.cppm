module;

#include <string>

export module Sturdy.Foundation:Memory;

import :Types;
import :Log;

using SFT::Foundation::f64;
using SFT::Foundation::u32;
using SFT::Foundation::usize;
using std::string;

export namespace SFT::Foundation::Memory {

    enum class ByteUnit {
        Bytes,
        Kilobytes,
        Megabytes,
        Gigabytes,
    };

    struct ByteFormatOptions {
        ByteUnit unit = ByteUnit::Megabytes;
        u32 decimal_places = 2;
        bool include_unit = true;
        bool include_bytes = false;
        bool space_before_unit = true;
    };

    struct HeapUsage {
        usize current_bytes;
        usize peak_bytes;
        usize current_resident_bytes;
        usize peak_resident_bytes;
        usize page_faults;
    };

    void initialize() noexcept;
    [[nodiscard]] bool is_initialized() noexcept;

    [[nodiscard]] u32 mimalloc_version() noexcept;

    [[nodiscard]] void *allocate(usize size) noexcept;
    [[nodiscard]] void *allocate_zeroed(usize size) noexcept;
    [[nodiscard]] void *allocate_aligned(usize size, usize alignment) noexcept;
    [[nodiscard]] void *allocate_zeroed_aligned(usize size, usize alignment) noexcept;
    [[nodiscard]] void *reallocate(void *pointer, usize size) noexcept;
    [[nodiscard]] void *reallocate_aligned(void *pointer, usize size, usize alignment) noexcept;

    void deallocate(void *pointer) noexcept;

    [[nodiscard]] usize usable_size(const void *pointer) noexcept;
    [[nodiscard]] usize good_size(usize size) noexcept;

    [[nodiscard]] HeapUsage heap_usage() noexcept;
    [[nodiscard]] usize heap_bytes() noexcept;
    [[nodiscard]] usize peak_heap_bytes() noexcept;

    [[nodiscard]] f64 bytes_as(usize bytes, ByteUnit unit) noexcept;
    [[nodiscard]] string format_bytes(usize bytes, ByteFormatOptions options = {});
    [[nodiscard]] string format_heap_bytes(ByteFormatOptions options = {});

    void collect(bool force = false) noexcept;
    void reset_stats() noexcept;
    void merge_thread_stats() noexcept;
    void log_stats(const char *tag = "mimalloc") noexcept;

} // namespace SFT::Foundation::Memory
