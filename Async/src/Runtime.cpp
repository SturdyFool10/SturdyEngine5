#include <Async/src/Runtime.hpp>


namespace SFT::Async {

    void ParallelRuntime::run_on_main_thread(std::function<void()> fn) noexcept {
        SFT::Async::run_on_main_thread(std::move(fn));
    }

    void ParallelRuntime::pump_main_thread() noexcept {
        SFT::Async::pump_main_thread();
    }

    void SynchronousRuntime::run_on_main_thread(std::function<void()> fn) noexcept { fn(); }

    void SynchronousRuntime::pump_main_thread() noexcept {}

} // namespace SFT::Async

