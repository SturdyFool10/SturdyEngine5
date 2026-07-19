#include <atomic>
#include <format>
#include <mimalloc.h>
#include <string_view>
#include <Foundation/src/Foundation.hpp>

#if defined(__linux__)
    #include <cstdio>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>
    // psapi.h must follow windows.h.
    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
#endif

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

    // The true, physically-resident footprint as the OS accounts for it. mimalloc's
    // mi_process_info reports its own *committed* address space (reserved from the OS but not
    // necessarily touched), which runs far larger than what a system monitor shows and is not a
    // meaningful "how much RAM are we using" number. Returns 0 if the OS value is unavailable, in
    // which case callers fall back to mimalloc's figure.
    usize os_resident_bytes() noexcept {
#if defined(__linux__)
        // /proc/self/statm reports (in pages): size resident shared text lib data dt.
        // resident - shared is the private working set, which matches the "Memory" column that
        // KDE/GNOME system monitors show (shared pages are attributed separately).
        FILE *statm = std::fopen("/proc/self/statm", "r");
        if (statm == nullptr) {
            return 0;
        }
        unsigned long resident_pages = 0;
        unsigned long shared_pages = 0;
        const int matched = std::fscanf(statm, "%*u %lu %lu", &resident_pages, &shared_pages);
        std::fclose(statm);
        if (matched != 2 || resident_pages < shared_pages) {
            return 0;
        }
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            return 0;
        }
        return static_cast<usize>(resident_pages - shared_pages) * static_cast<usize>(page_size);
#elif defined(_WIN32)
        // WorkingSetSize is the total resident set (private + shared). It is not the private-only
        // figure Task Manager's "Memory" column shows (that needs QueryWorkingSetEx enumeration),
        // but it is a true resident measurement rather than committed address space.
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
            return 0;
        }
        return static_cast<usize>(counters.WorkingSetSize);
#elif defined(__APPLE__)
        // phys_footprint matches Activity Monitor's "Memory" column.
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
            return 0;
        }
        return static_cast<usize>(info.phys_footprint);
#else
        return 0;
#endif
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

        // Prefer the OS's real resident figure over mimalloc's, which actually reports committed
        // address space and vastly overstates physical footprint. Fall back to mimalloc only if the
        // OS query fails.
        if (const usize os_resident = os_resident_bytes(); os_resident != 0) {
            usage.current_resident_bytes = os_resident;
        }
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
