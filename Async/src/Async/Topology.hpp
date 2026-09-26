#pragma once

#include <cstdint>
#include <thread>
#include <vector>

namespace SFT::Async {

    using u32 = std::uint32_t;


    /// Returns the current or globally available ranked logical cores value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::vector<u32> ranked_logical_cores() noexcept;


    /// Returns the current or globally available ranked physical cores value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::vector<u32> ranked_physical_cores() noexcept;


    /// Performs the pin thread to core operation for `Async` using the supplied arguments.
    ///
    /// @param thread Thread used or affected by the operation.
    /// @param core_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool pin_thread_to_core(std::thread &thread, u32 core_index) noexcept;

    /// Pins a thread the caller does not own as a `std::thread` (a Rust/C thread, a pool worker) given
    /// its native handle (`pthread_t` on POSIX, `HANDLE` on Windows).
    ///
    /// @return Returns `true` when the affinity was applied; `false` on unsupported platforms.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool pin_native_thread_to_core(std::thread::native_handle_type handle, u32 core_index) noexcept;

    /// Pins the calling thread to `core_index`.
    ///
    /// @return Returns `true` when the affinity was applied; `false` on unsupported platforms.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool pin_current_thread_to_core(u32 core_index) noexcept;

} // namespace SFT::Async
