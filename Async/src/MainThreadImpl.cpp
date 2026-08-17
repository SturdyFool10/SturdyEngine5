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
