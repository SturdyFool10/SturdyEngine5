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
