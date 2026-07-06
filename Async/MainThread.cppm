module;

#pragma region Imports
#include <functional>
#pragma endregion

export module Sturdy.Async:MainThread;

export namespace SFT::Async {

    // Queues `fn` to run on the main thread. Callable from any thread — producers never need to know
    // or care which thread is "main," only that this call is fire-and-forget and `fn` will run later,
    // in submission order, on whichever thread calls `pump_main_thread()`.
    void run_on_main_thread(std::function<void()> fn) noexcept;

    // Runs every job queued since the last call, in submission order, on the calling thread. Only
    // ever call this from the main thread — e.g. once per frame from the application's run loop.
    void pump_main_thread() noexcept;

} // namespace SFT::Async
