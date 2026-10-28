#include <Async/IoHandler.hpp>

#if defined(STURDY_PLATFORM_WEB)
namespace SFT::Async {
    bool IoHandler::configure(const IoHandlerConfig &) { return false; }
    void IoHandler::enqueue(std::function<void()> operation) { operation(); }
    IoBackend IoHandler::backend() noexcept { return IoBackend::Synchronous; }
    bool IoHandler::is_worker_thread() noexcept { return true; }
    unsigned IoHandler::worker_count() { return 0; }
    unsigned IoHandler::pending_count() { return 0; }
    unsigned IoHandler::pending_capacity() { return 0; }
    bool IoHandler::shutdown() { return true; }
} // namespace SFT::Async
#else

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace SFT::Async {
    namespace {
        thread_local bool t_is_io_worker = false;

        [[nodiscard]] bool valid_config(const IoHandlerConfig &config) noexcept {
            return (config.worker_count == 0 || config.worker_count <= 64) &&
                config.max_pending_operations > 0 && config.max_pending_operations <= 1'000'000;
        }

        class BlockingIoPool {
          public:
            explicit BlockingIoPool(const IoHandlerConfig &config) : max_pending_(config.max_pending_operations) {
                const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
                const unsigned count = config.worker_count == 0 ? std::clamp(hardware, 2u, 8u) : config.worker_count;
                workers_.reserve(count);
                try {
                    for (unsigned i = 0; i < count; ++i) {
                        workers_.emplace_back([this] { worker_loop(); });
                    }
                } catch (...) {
                    {
                        std::lock_guard lock(mutex_);
                        stopping_ = true;
                    }
                    ready_.notify_all();
                    for (auto &worker : workers_) {
                        if (worker.joinable()) {
                            worker.join();
                        }
                    }
                    throw;
                }
            }

            ~BlockingIoPool() { shutdown(); }

            void enqueue(std::function<void()> operation) {
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_) {
                        throw std::runtime_error("Async IO handler is shutting down");
                    }
                    if (queue_.size() >= max_pending_) {
                        throw IoQueueFull{};
                    }
                    queue_.push_back(std::move(operation));
                }
                ready_.notify_one();
            }

            void shutdown() {
                std::lock_guard join_lock(join_mutex_);
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                ready_.notify_all();
                for (auto &worker : workers_) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
            }

            [[nodiscard]] unsigned worker_count() const noexcept {
                return static_cast<unsigned>(workers_.size());
            }

            [[nodiscard]] unsigned pending_count() const {
                std::lock_guard lock(mutex_);
                return static_cast<unsigned>(queue_.size());
            }

            [[nodiscard]] unsigned pending_capacity() const noexcept {
                return max_pending_;
            }

          private:
            void worker_loop() {
                t_is_io_worker = true;
                for (;;) {
                    std::function<void()> operation;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                        if (stopping_ && queue_.empty()) {
                            t_is_io_worker = false;
                            return;
                        }
                        operation = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    operation();
                }
            }

            mutable std::mutex mutex_;
            std::mutex join_mutex_;
            std::condition_variable ready_;
            std::deque<std::function<void()>> queue_;
            std::vector<std::thread> workers_;
            const unsigned max_pending_;
            bool stopping_ = false;
        };

        class IoService {
          public:
            [[nodiscard]] bool configure(const IoHandlerConfig &config) {
                std::lock_guard lock(mutex_);
                if (started_ || shutdown_started_ || !valid_config(config)) {
                    return false;
                }
                config_ = config;
                return true;
            }

            BlockingIoPool &pool() {
                std::lock_guard lock(mutex_);
                if (shutdown_started_) {
                    throw std::runtime_error("Async IO handler has been shut down");
                }
                started_ = true;
                if (!pool_) {
                    pool_ = std::make_unique<BlockingIoPool>(config_);
                }
                return *pool_;
            }

            [[nodiscard]] bool shutdown() {
                if (t_is_io_worker) {
                    return false;
                }

                BlockingIoPool *active_pool = nullptr;
                {
                    std::unique_lock lock(mutex_);
                    if (shutdown_started_) {
                        shutdown_complete_cv_.wait(lock, [this] { return shutdown_complete_; });
                        return true;
                    }
                    shutdown_started_ = true;
                    active_pool = pool_.get();
                }

                if (active_pool) {
                    active_pool->shutdown();
                }

                {
                    std::lock_guard lock(mutex_);
                    shutdown_complete_ = true;
                }
                shutdown_complete_cv_.notify_all();
                return true;
            }

          private:
            std::mutex mutex_;
            std::condition_variable shutdown_complete_cv_;
            IoHandlerConfig config_{};
            std::unique_ptr<BlockingIoPool> pool_;
            bool started_ = false;
            bool shutdown_started_ = false;
            bool shutdown_complete_ = false;
        };

        IoService &io_service() {
            static IoService service;
            return service;
        }
    } // namespace

    bool IoHandler::configure(const IoHandlerConfig &config) {
        return io_service().configure(config);
    }

    void IoHandler::enqueue(std::function<void()> operation) {
        io_service().pool().enqueue(std::move(operation));
    }

    IoBackend IoHandler::backend() noexcept {
        return IoBackend::BlockingThreadPool;
    }

    bool IoHandler::is_worker_thread() noexcept {
        return t_is_io_worker;
    }

    unsigned IoHandler::worker_count() {
        return io_service().pool().worker_count();
    }

    unsigned IoHandler::pending_count() {
        return io_service().pool().pending_count();
    }

    unsigned IoHandler::pending_capacity() {
        return io_service().pool().pending_capacity();
    }

    bool IoHandler::shutdown() {
        return io_service().shutdown();
    }
} // namespace SFT::Async

#endif
