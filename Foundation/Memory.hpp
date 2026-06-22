#pragma once

#include "Types.hpp"

#include <string>

// Thin Foundation wrapper over mimalloc.
//
// Foundation initializes mimalloc from the Sturdy.Foundation module initializer, so callers do not
// need to call Memory::initialize() before using the engine. The explicit initialize() hook remains
// available and idempotent for tests or non-module consumers that want to force initialization.

namespace SFT::Foundation::Memory {

    enum class ByteUnit {
        Bytes,
        Kilobytes,
        Megabytes,
        Gigabytes,
    };

    struct ByteFormatOptions {
        // Decimal units: KB = 1,000 bytes, MB = 1,000,000 bytes, GB = 1,000,000,000 bytes.
        ByteUnit unit = ByteUnit::Megabytes;
        u32 decimal_places = 2;
        bool include_unit = true;
        bool include_bytes = false;
        bool space_before_unit = true;
    };

    struct HeapUsage {
        usize current_bytes;          // current committed mimalloc heap bytes
        usize peak_bytes;             // peak committed mimalloc heap bytes
        usize current_resident_bytes; // current process RSS, estimated from committed heap bytes on some platforms
        usize peak_resident_bytes;    // peak process RSS, estimated from committed heap bytes on some platforms
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

    // Converts using decimal byte units, not binary IEC units (MB, not MiB).
    [[nodiscard]] f64 bytes_as(usize bytes, ByteUnit unit) noexcept;
    [[nodiscard]] std::string format_bytes(usize bytes, ByteFormatOptions options = {});
    [[nodiscard]] std::string format_heap_bytes(ByteFormatOptions options = {});

    void collect(bool force = false) noexcept;
    void reset_stats() noexcept;
    void merge_thread_stats() noexcept;
    void log_stats(const char *tag = "mimalloc") noexcept;

} // namespace SFT::Foundation::Memory
