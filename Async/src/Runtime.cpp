#include <Async/src/Runtime.hpp>


namespace SFT::Async {

    /// Runs on main thread.
    ///
    /// @param fn Callable invoked by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void ParallelRuntime::run_on_main_thread(std::function<void()> fn) noexcept {
        SFT::Async::run_on_main_thread(std::move(fn));
    }

    /// Pumps main thread using the supplied arguments and current state.
    ///
    /// @return Returns the current pump main thread value.
    /// @note This function does not throw exceptions.
    void ParallelRuntime::pump_main_thread() noexcept {
        SFT::Async::pump_main_thread();
    }

    /// Runs on main thread.
    ///
    /// @param fn Callable invoked by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void SynchronousRuntime::run_on_main_thread(std::function<void()> fn) noexcept { fn(); }

    /// Pumps main thread using the supplied arguments and current state.
    ///
    /// @return Returns the current pump main thread value.
    /// @note This function does not throw exceptions.
    void SynchronousRuntime::pump_main_thread() noexcept {}

} // namespace SFT::Async

