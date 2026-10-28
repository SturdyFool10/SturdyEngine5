#include <Async/ThreadPool.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace SFT::Async {

    namespace Detail {

        /// One shared queue guarded by a mutex: simple and correct, and cheap at the task granularity the splitting engine
        /// produces (a handful of leaves per thread). Waiting threads help by running queued tasks, so nested fork-join
        /// cannot starve the pool.
        struct PoolImpl final : PoolBase {
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<std::unique_ptr<TaskBase>> queue;
            std::vector<std::thread> workers;
            bool stopping = false;
            std::size_t size = 0;

            void enqueue(std::unique_ptr<TaskBase> task) noexcept override {
                {
                    std::lock_guard lock(mutex);
                    queue.push_back(std::move(task));
                }
                wake.notify_one();
            }

            /// Used by threads that are waiting for a task. They take the *newest* queued task: in fork-join code that is
            /// almost always the one they are about to wait for, so the stack depth stays at the recursion depth instead of
            /// growing with the amount of unrelated queued work. Idle workers take the oldest (FIFO) for fairness.
            bool try_run_one() noexcept override {
                std::unique_ptr<TaskBase> task;
                {
                    std::lock_guard lock(mutex);
                    if (queue.empty()) return false;
                    task = std::move(queue.back());
                    queue.pop_back();
                }
                task->execute();
                return true;
            }

            [[nodiscard]] std::size_t thread_count() const noexcept override { return size; }

            void run_worker(const std::string &name, std::size_t index) {
                t_pool = this;
#if defined(__linux__)
                const std::string full = (name + "-" + std::to_string(index)).substr(0, 15); // pthread names are limited to 15 characters
                pthread_setname_np(pthread_self(), full.c_str());
#else
                (void)name;
                (void)index;
#endif
                for (;;) {
                    std::unique_ptr<TaskBase> task;
                    {
                        std::unique_lock lock(mutex);
                        wake.wait(lock, [&] { return stopping || !queue.empty(); });
                        if (queue.empty()) break; // stopping and drained
                        task = std::move(queue.front());
                        queue.pop_front();
                    }
                    task->execute();
                }
                t_pool = nullptr;
            }
        };

    } // namespace Detail

    ThreadPool::ThreadPool(ThreadPoolOptions options) : impl_(std::make_unique<Detail::PoolImpl>()) {
        const std::size_t count = options.threads != 0 ? options.threads : (std::max)(std::size_t{1}, static_cast<std::size_t>(std::thread::hardware_concurrency()));
        impl_->size = count;
        impl_->workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            impl_->workers.emplace_back([impl = impl_.get(), name = options.name, i] { impl->run_worker(name, i); });
        }
    }

    ThreadPool::~ThreadPool() {
        if (!impl_) return;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->wake.notify_all();
        for (auto &worker : impl_->workers)
            if (worker.joinable()) worker.join();
    }

    ThreadPool::ThreadPool(ThreadPool &&) noexcept = default;
    ThreadPool &ThreadPool::operator=(ThreadPool &&other) noexcept {
        if (this != &other) {
            this->~ThreadPool();
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    std::size_t ThreadPool::num_threads() const noexcept { return impl_ ? impl_->size : 0; }
    bool ThreadPool::on_pool_thread() const noexcept { return impl_ && Detail::t_pool == impl_.get(); }
    void ThreadPool::submit(std::unique_ptr<Detail::TaskBase> task) { impl_->enqueue(std::move(task)); }

} // namespace SFT::Async
