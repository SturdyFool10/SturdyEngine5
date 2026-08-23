#include <atomic>
#include <format>
#include <mimalloc.h>
#include <string_view>
#include <Foundation/Foundation.hpp>

#if defined(__linux__)
    #include <cstdio>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>

    #include <psapi.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
#endif

using SFT::Foundation::f64, SFT::Foundation::u32, SFT::Foundation::usize;
using std::atomic;
using std::format;
using std::make_format_args;
using std::vformat;
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

    /// Performs the byte unit info operation using the supplied arguments.
    ///
    /// @param unit `unit` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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


    /// Computes the os resident bytes required by the supplied values.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize os_resident_bytes() noexcept {
#if defined(__linux__)


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


        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
            return 0;
        }
        return static_cast<usize>(counters.WorkingSetSize);
#elif defined(__APPLE__)

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

    /// Logs mimalloc line using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    /// @param arg `arg` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
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

    /// Initializes the `Memory` for use.
    ///
    /// @note This function does not throw exceptions.
    void initialize() noexcept {
        if (g_initialized.exchange(true, memory_order_acq_rel)) {
            return;
        }

        mi_thread_init();
    }

    /// Reports whether initialized holds for this `Memory`.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    bool is_initialized() noexcept {
        return g_initialized.load(memory_order_acquire);
    }

    /// Returns the current or globally available mimalloc version value.
    ///
    /// @return Returns the current mimalloc version value.
    /// @note This function does not throw exceptions.
    u32 mimalloc_version() noexcept {
        return static_cast<u32>(mi_version());
    }

    /// Allocates storage or a resource.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *allocate(usize size) noexcept {
        initialize();
        return mi_malloc(size);
    }

    /// Allocates zeroed.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *allocate_zeroed(usize size) noexcept {
        initialize();
        return mi_zalloc(size);
    }

    /// Allocates aligned.
    ///
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *allocate_aligned(usize size, usize alignment) noexcept {
        initialize();
        return mi_malloc_aligned(size, alignment);
    }

    /// Allocates zeroed aligned.
    ///
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *allocate_zeroed_aligned(usize size, usize alignment) noexcept {
        initialize();
        return mi_zalloc_aligned(size, alignment);
    }

    /// Performs the reallocate operation for `Memory` using the supplied arguments.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *reallocate(void *pointer, usize size) noexcept {
        initialize();
        return mi_realloc(pointer, size);
    }

    /// Performs the reallocate aligned operation for `Memory` using the supplied arguments.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    void *reallocate_aligned(void *pointer, usize size, usize alignment) noexcept {
        initialize();
        return mi_realloc_aligned(pointer, size, alignment);
    }

    /// Releases previously allocated storage or resources.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void deallocate(void *pointer) noexcept {
        mi_free(pointer);
    }

    /// Returns the usable size for this `Memory`.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    usize usable_size(const void *pointer) noexcept {
        return mi_usable_size(pointer);
    }

    /// Returns the good size for this `Memory`.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    usize good_size(usize size) noexcept {
        return mi_good_size(size);
    }

    /// Returns the current or globally available heap usage value.
    ///
    /// @return Returns the current heap usage value.
    /// @note This function does not throw exceptions.
    HeapUsage heap_usage() noexcept {
        initialize();
        mi_stats_merge();

        HeapUsage usage{};
        mi_process_info(nullptr, nullptr, nullptr, &usage.current_resident_bytes, &usage.peak_resident_bytes, &usage.current_bytes, &usage.peak_bytes, &usage.page_faults);


        if (const usize os_resident = os_resident_bytes(); os_resident != 0) {
            usage.current_resident_bytes = os_resident;
        }
        return usage;
    }

    /// Computes the heap bytes required by the supplied values.
    ///
    /// @return Returns the current heap bytes value.
    /// @note This function does not throw exceptions.
    usize heap_bytes() noexcept {
        return heap_usage().current_bytes;
    }

    /// Computes the peak heap bytes required by the supplied values.
    ///
    /// @return Returns the current peak heap bytes value.
    /// @note This function does not throw exceptions.
    usize peak_heap_bytes() noexcept {
        return heap_usage().peak_bytes;
    }

    /// Performs the bytes as operation for `Memory` using the supplied arguments.
    ///
    /// @param bytes Size of the relevant data in bytes.
    /// @param unit `unit` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f64 bytes_as(usize bytes, ByteUnit unit) noexcept {
        return static_cast<f64>(bytes) / byte_unit_info(unit).divisor;
    }

    /// Computes the format bytes required by the supplied values.
    ///
    /// @param bytes Size of the relevant data in bytes.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string format_bytes(usize bytes, ByteFormatOptions options) {
        const ByteUnitInfo unit = byte_unit_info(options.unit);
        // Dynamic precision goes through vformat: libstdc++ 16's consteval check for `{:.{}f}`
        // is not usable at the point of use under clang, so the compile-time path is avoided.
        const f64 scaled = bytes_as(bytes, options.unit);
        const u32 places = options.decimal_places;
        string formatted = unit.integral ? format("{}", bytes) : vformat("{:.{}f}", make_format_args(scaled, places));

        if (options.include_unit) {
            formatted += format("{}{}", options.space_before_unit ? " " : "", unit.suffix);
        }

        if (options.include_bytes) {
            formatted += format(" ({} bytes)", bytes);
        }

        return formatted;
    }

    /// Computes the format heap bytes required by the supplied values.
    ///
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string format_heap_bytes(ByteFormatOptions options) {
        return format_bytes(heap_bytes(), options);
    }

    /// Collects the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param force `force` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void collect(bool force) noexcept {
        initialize();
        mi_collect(force);
    }

    /// Resets stats to its baseline state.
    ///
    /// @note This function does not throw exceptions.
    void reset_stats() noexcept {
        initialize();
        mi_stats_reset();
    }

    /// Performs the merge thread stats operation for `Memory` using the supplied arguments.
    ///
    /// @note This function does not throw exceptions.
    void merge_thread_stats() noexcept {
        initialize();
        mi_stats_merge();
    }

    /// Logs stats using the supplied arguments and current state.
    ///
    /// @param tag `tag` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_stats(const char *tag) noexcept {
        initialize();
        mi_stats_merge();
        mi_stats_print_out(&log_mimalloc_line, const_cast<char *>(tag != nullptr ? tag : "stats"));
    }

} // namespace SFT::Foundation::Memory
