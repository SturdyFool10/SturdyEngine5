#include <Foundation/Foundation.hpp>
#include <deque>
#include <functional>
#include <utility>
#include <Async/MainThread.hpp>
#include <Async/Mutex.hpp>

using std::deque;
using std::function;

namespace SFT::Async {

    namespace {

        /// Returns the current or globally available main thread queue value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        Mutex<deque<function<void()>>> &main_thread_queue() noexcept {
            static Mutex<deque<function<void()>>> queue;


            static const bool named = (queue.set_debug_name("Async Main Thread Queue"), true);
            (void)named;
            return queue;
        }

    } // namespace

    /// Runs on main thread.
    ///
    /// @param fn Callable invoked by the operation.
    ///
    /// @note This function does not throw exceptions.
    void run_on_main_thread(function<void()> fn) noexcept {
        auto guard = main_thread_queue().lock();
        guard->push_back(std::move(fn));
    }

    /// Pumps main thread using the supplied arguments and current state.
    ///
    /// @note This function does not throw exceptions.
    void pump_main_thread() noexcept {


        deque<function<void()>> jobs;
        {
            auto guard = main_thread_queue().lock();
            jobs.swap(*guard);
        }

        for (function<void()> &job : jobs) {
            job();
        }
    }

} // namespace SFT::Async
