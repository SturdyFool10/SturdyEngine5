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

    /// Installs a callback run (on the posting thread) after every `run_on_main_thread`, so a main loop that blocks
    /// waiting for events can be woken to run the job. Pass an empty function to remove it.
    void set_main_thread_wake_hook(std::function<void()> hook) noexcept;

} // namespace SFT::Async
