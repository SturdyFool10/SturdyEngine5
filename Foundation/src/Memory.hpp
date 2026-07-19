#pragma once

#include <string>



using SFT::Foundation::f64;
using SFT::Foundation::u32;
using SFT::Foundation::usize;
using std::string;

namespace SFT::Foundation::Memory {

    // The engine's explicit allocator, backed by **mimalloc**. The process allocator and global
    // `new`/`delete` are intentionally left to the platform CRT so system shared libraries keep their
    // expected allocator ownership. Use these functions for engine-owned raw, sized, or over-aligned
    // allocations and for the heap statistics/formatting helpers below.

    // Unit for byte formatting / conversion.
    enum class ByteUnit {
        Bytes,
        Kilobytes,
        Megabytes,
        Gigabytes,
    };

    // Formatting knobs for `format_bytes()` / `format_heap_bytes()`.
    //
    // - `unit`             — which `ByteUnit` to render in.
    // - `decimal_places`   — digits after the point.
    // - `include_unit`     — append the unit suffix (e.g. `" MB"`).
    // - `include_bytes`    — also append the exact byte count in parentheses.
    // - `space_before_unit`— put a space between the number and the suffix.
    struct ByteFormatOptions {
        ByteUnit unit = ByteUnit::Megabytes;
        u32 decimal_places = 2;
        bool include_unit = true;
        bool include_bytes = false;
        bool space_before_unit = true;
    };

    // A snapshot of allocator/process memory use, all in bytes (plus a fault count).
    //
    // - `current_bytes` / `peak_bytes`                   — live and high-water heap allocated by mimalloc.
    // - `current_resident_bytes` / `peak_resident_bytes` — physical (resident) memory, OS-reported.
    // - `page_faults`                                    — page faults since process start.
    struct HeapUsage {
        usize current_bytes;
        usize peak_bytes;
        usize current_resident_bytes;
        usize peak_resident_bytes;
        usize page_faults;
    };

    // Install mimalloc as the process allocator. Idempotent; called automatically during
    // `Sturdy.Foundation` load, so you normally never call it yourself.
    void initialize() noexcept;

    // Whether `initialize()` has run.
    [[nodiscard]] bool is_initialized() noexcept;

    // The linked mimalloc version, encoded as `major*100 + minor*10 + patch` (e.g. `210` for 2.1.0).
    [[nodiscard]] u32 mimalloc_version() noexcept;

    // Allocate `size` bytes (uninitialized). Returns `nullptr` on failure. Free with `deallocate()`.
    [[nodiscard]] void *allocate(usize size) noexcept;

    // Allocate `size` zero-initialized bytes. Returns `nullptr` on failure.
    [[nodiscard]] void *allocate_zeroed(usize size) noexcept;

    // Allocate `size` bytes aligned to `alignment` (a power of two) — for SIMD/cache-line/GPU-upload
    // buffers. Returns `nullptr` on failure. Free with `deallocate()`.
    [[nodiscard]] void *allocate_aligned(usize size, usize alignment) noexcept;

    // Aligned + zero-initialized allocation. Returns `nullptr` on failure.
    [[nodiscard]] void *allocate_zeroed_aligned(usize size, usize alignment) noexcept;

    // Resize the block at `pointer` to `size` bytes, preserving contents (moving if needed). Passing
    // `nullptr` behaves like `allocate`. Returns `nullptr` on failure (the original block stays valid).
    [[nodiscard]] void *reallocate(void *pointer, usize size) noexcept;

    // `reallocate` preserving a power-of-two `alignment`.
    [[nodiscard]] void *reallocate_aligned(void *pointer, usize size, usize alignment) noexcept;

    // Free a block from any `allocate*` / `reallocate*` above. `nullptr` is a no-op.
    void deallocate(void *pointer) noexcept;

    // Actual usable size of the block at `pointer` — may exceed the requested size (mimalloc rounds up
    // to a size class). `0` for `nullptr`.
    [[nodiscard]] usize usable_size(const void *pointer) noexcept;

    // The size mimalloc would actually reserve for a request of `size` bytes — query it up front to
    // size a buffer to its real capacity and skip a later reallocation.
    [[nodiscard]] usize good_size(usize size) noexcept;

    // Full `HeapUsage` snapshot (see above).
    [[nodiscard]] HeapUsage heap_usage() noexcept;

    // Shorthand for `heap_usage().current_bytes` — bytes currently allocated.
    [[nodiscard]] usize heap_bytes() noexcept;

    // Shorthand for `heap_usage().peak_bytes` — high-water bytes allocated.
    [[nodiscard]] usize peak_heap_bytes() noexcept;

    // Convert a raw byte count into `unit`, as a `f64` (e.g. `bytes_as(1<<20, ByteUnit::Megabytes) == 1.0`).
    [[nodiscard]] f64 bytes_as(usize bytes, ByteUnit unit) noexcept;

    // Render `bytes` as a human-readable string per `options`, e.g. `"12.50 MB"`.
    [[nodiscard]] string format_bytes(usize bytes, ByteFormatOptions options = {});

    // `format_bytes(heap_bytes(), options)` — the current heap size, formatted.
    [[nodiscard]] string format_heap_bytes(ByteFormatOptions options = {});

    // Ask mimalloc to return free memory to the OS. `force` collects more aggressively (slower). Handy
    // after a large transient spike (e.g. asset loading).
    void collect(bool force = false) noexcept;

    // Reset the allocator's peak/statistics counters.
    void reset_stats() noexcept;

    // Fold the calling thread's per-thread stats into the process totals — call before reading stats on
    // a thread that did significant allocation.
    void merge_thread_stats() noexcept;

    // Dump mimalloc's own statistics to the log, prefixed with `tag`.
    void log_stats(const char *tag = "mimalloc") noexcept;

} // namespace SFT::Foundation::Memory
