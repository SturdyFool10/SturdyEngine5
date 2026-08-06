#include <Foundation/src/Foundation.hpp>
#include <deque>
#include <functional>
#include <utility>
#include <Async/src/MainThread.hpp>
#include <Async/src/Mutex.hpp>

using std::deque;
using std::function;

namespace SFT::Async {

    namespace {

        Mutex<deque<function<void()>>> &main_thread_queue() noexcept {
            static Mutex<deque<function<void()>>> queue;
            // Mutex<T> is non-copyable/non-movable (it guards its value in place), so it can't be
            // built-then-named via a static initializer lambda the usual way -- this runs
            // set_debug_name() exactly once, piggybacking on the same thread-safe "init on first
            // call" guarantee `queue` itself already gets as a function-local static.
            static const bool named = (queue.set_debug_name("Async Main Thread Queue"), true);
            (void)named;
            return queue;
        }

    } // namespace

    void run_on_main_thread(function<void()> fn) noexcept {
        auto guard = main_thread_queue().lock();
        guard->push_back(std::move(fn));
    }

    void pump_main_thread() noexcept {
        // Swap the pending jobs out under the lock, then run them outside it — so a job that itself
        // calls run_on_main_thread() (queuing for the *next* pump) can't deadlock against this one,
        // and one pump can't be held up by producers still pushing more work.
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
