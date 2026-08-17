#pragma once

#include <functional>

namespace SFT::Async {

    /// Runs on main thread.
    ///
    /// @param fn Callable invoked by the operation.
    ///
    /// @note This function does not throw exceptions.
    void run_on_main_thread(std::function<void()> fn) noexcept;
    /// Pumps main thread using the supplied arguments and current state.
    ///
    /// @note This function does not throw exceptions.
    void pump_main_thread() noexcept;

} // namespace SFT::Async
