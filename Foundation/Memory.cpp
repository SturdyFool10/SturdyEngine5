#include "Foundation/Memory.hpp"

#include "Log.hpp"
#include "Types.hpp"

#include <atomic>
#include <format>
#include <mimalloc-new-delete.h>
#include <mimalloc.h>
#include <string_view>

using SFT::Foundation::f64, SFT::Foundation::u32, SFT::Foundation::usize;
using std::atomic;
using std::format;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::string;
using std::string_view;

namespace {

    constexpr f64 bytes_per_kb = 1'000.0;
    constexpr f64 bytes_per_mb = 1'000'000.0;
    constexpr f64 bytes_per_gb = 1'000'000'000.0;

    struct ByteUnitInfo {
        f64 divisor;
        string_view suffix;
        bool integral;
    };

    constinit atomic<bool> g_initialized{false};

    constexpr ByteUnitInfo byte_unit_info(SFT::Foundation::Memory::ByteUnit unit) noexcept {
        using enum SFT::Foundation::Memory::ByteUnit;

        switch (unit) {
            case Bytes:
                return {1.0, "bytes", true};
            case Kilobytes:
                return {bytes_per_kb, "KB", false};
            case Megabytes:
                return {bytes_per_mb, "MB", false};
            case Gigabytes:
                return {bytes_per_gb, "GB", false};
        }

        return {bytes_per_mb, "MB", false};
    }

    void log_mimalloc_line(const char *message, void *arg) noexcept {
        if (message == nullptr || message[0] == '\0') {
            return;
        }

        auto line = string_view{message};
        while (!line.empty() and (line.back() == '\n' or line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            return;
        }

        const auto *tag = static_cast<const char *>(arg);
        SFT::Foundation::log_info("mimalloc [{}]: {}", tag != nullptr ? tag : "stats", line);
    }

} // namespace

namespace SFT::Foundation::Memory {

    void initialize() noexcept {
        if (g_initialized.exchange(true, memory_order_acq_rel)) {
            return;
        }

        mi_thread_init();
    }

    bool is_initialized() noexcept {
        return g_initialized.load(memory_order_acquire);
    }

    u32 mimalloc_version() noexcept {
        return static_cast<u32>(mi_version());
    }

    void *allocate(usize size) noexcept {
        initialize();
        return mi_malloc(size);
    }

    void *allocate_zeroed(usize size) noexcept {
        initialize();
        return mi_zalloc(size);
    }

    void *allocate_aligned(usize size, usize alignment) noexcept {
        initialize();
        return mi_malloc_aligned(size, alignment);
    }

    void *allocate_zeroed_aligned(usize size, usize alignment) noexcept {
        initialize();
        return mi_zalloc_aligned(size, alignment);
    }

    void *reallocate(void *pointer, usize size) noexcept {
        initialize();
        return mi_realloc(pointer, size);
    }

    void *reallocate_aligned(void *pointer, usize size, usize alignment) noexcept {
        initialize();
        return mi_realloc_aligned(pointer, size, alignment);
    }

    void deallocate(void *pointer) noexcept {
        mi_free(pointer);
    }

    usize usable_size(const void *pointer) noexcept {
        return mi_usable_size(pointer);
    }

    usize good_size(usize size) noexcept {
        return mi_good_size(size);
    }

    HeapUsage heap_usage() noexcept {
        initialize();
        mi_stats_merge();

        HeapUsage usage{};
        mi_process_info(nullptr, nullptr, nullptr, &usage.current_resident_bytes, &usage.peak_resident_bytes, &usage.current_bytes, &usage.peak_bytes, &usage.page_faults);
        return usage;
    }

    usize heap_bytes() noexcept {
        return heap_usage().current_bytes;
    }

    usize peak_heap_bytes() noexcept {
        return heap_usage().peak_bytes;
    }

    f64 bytes_as(usize bytes, ByteUnit unit) noexcept {
        return static_cast<f64>(bytes) / byte_unit_info(unit).divisor;
    }

    string format_bytes(usize bytes, ByteFormatOptions options) {
        const ByteUnitInfo unit = byte_unit_info(options.unit);
        string formatted = unit.integral
                               ? format("{}", bytes)
                               : format("{:.{}f}", bytes_as(bytes, options.unit), options.decimal_places);

        if (options.include_unit) {
            formatted += format("{}{}", options.space_before_unit ? " " : "", unit.suffix);
        }

        if (options.include_bytes) {
            formatted += format(" ({} bytes)", bytes);
        }

        return formatted;
    }

    string format_heap_bytes(ByteFormatOptions options) {
        return format_bytes(heap_bytes(), options);
    }

    void collect(bool force) noexcept {
        initialize();
        mi_collect(force);
    }

    void reset_stats() noexcept {
        initialize();
        mi_stats_reset();
    }

    void merge_thread_stats() noexcept {
        initialize();
        mi_stats_merge();
    }

    void log_stats(const char *tag) noexcept {
        initialize();
        mi_stats_merge();
        mi_stats_print_out(&log_mimalloc_line, const_cast<char *>(tag != nullptr ? tag : "stats"));
    }

} // namespace SFT::Foundation::Memory
