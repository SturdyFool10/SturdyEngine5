#pragma once

#include <Foundation/Types.hpp>

#include <string>


using SFT::Foundation::f64;
using SFT::Foundation::u32;
using SFT::Foundation::usize;
using std::string;

namespace SFT::Foundation::Memory {


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


    /// Initializes the associated runtime state for use.
    ///
    /// @note This function does not throw exceptions.
    void initialize() noexcept;


    /// Reports whether initialized holds.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool is_initialized() noexcept;


    /// Returns the current or globally available mimalloc version value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u32 mimalloc_version() noexcept;


    /// Allocates storage or a resource.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *allocate(usize size) noexcept;


    /// Allocates zeroed.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *allocate_zeroed(usize size) noexcept;


    /// Allocates aligned.
    ///
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *allocate_aligned(usize size, usize alignment) noexcept;


    /// Allocates zeroed aligned.
    ///
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *allocate_zeroed_aligned(usize size, usize alignment) noexcept;


    /// Performs the reallocate operation using the supplied arguments.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *reallocate(void *pointer, usize size) noexcept;


    /// Performs the reallocate aligned operation using the supplied arguments.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    /// @param size Requested or available size for the operation.
    /// @param alignment `alignment` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] void *reallocate_aligned(void *pointer, usize size, usize alignment) noexcept;


    /// Releases previously allocated storage or resources.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void deallocate(void *pointer) noexcept;


    /// Returns the requested usable size.
    ///
    /// @param pointer Pointer to the object or storage used by the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize usable_size(const void *pointer) noexcept;


    /// Returns the requested good size.
    ///
    /// @param size Requested or available size for the operation.
    ///
    /// @return Returns the requested count or size.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize good_size(usize size) noexcept;


    /// Returns the current or globally available heap usage value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] HeapUsage heap_usage() noexcept;


    /// Computes the heap bytes required by the supplied values.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize heap_bytes() noexcept;


    /// Computes the peak heap bytes required by the supplied values.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize peak_heap_bytes() noexcept;


    /// Performs the bytes as operation using the supplied arguments.
    ///
    /// @param bytes Size of the relevant data in bytes.
    /// @param unit `unit` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] f64 bytes_as(usize bytes, ByteUnit unit) noexcept;


    /// Computes the format bytes required by the supplied values.
    ///
    /// @param bytes Size of the relevant data in bytes.
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string format_bytes(usize bytes, ByteFormatOptions options = {});


    /// Computes the format heap bytes required by the supplied values.
    ///
    /// @param options Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string format_heap_bytes(ByteFormatOptions options = {});


    /// Collects the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param force `force` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void collect(bool force = false) noexcept;


    /// Resets stats to its baseline state.
    ///
    /// @note This function does not throw exceptions.
    void reset_stats() noexcept;


    /// Performs the merge thread stats operation using the supplied arguments.
    ///
    /// @note This function does not throw exceptions.
    void merge_thread_stats() noexcept;


    /// Logs stats using the supplied arguments and current state.
    ///
    /// @param tag `tag` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_stats(const char *tag = "mimalloc") noexcept;

} // namespace SFT::Foundation::Memory
