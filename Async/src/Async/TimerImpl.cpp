#include <Async/Mutex.hpp>
#include <Async/Timer.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace SFT::Async {

#if defined(STURDY_PLATFORM_WEB)

    // No threads on Web: a timer degenerates to a blocking sleep on the (only) thread.
    void Detail::schedule_at(Clock::time_point deadline, std::function<void()> fn, CancellationToken token,
                             std::function<void()> on_cancel) {
        std::this_thread::sleep_until(deadline);
        if (token.is_cancelled() && on_cancel) {
            on_cancel();
        } else {
            fn();
        }
    }

#else

    namespace {

        struct Entry {
            Clock::time_point deadline;
            std::uint64_t sequence;
            std::function<void()> fn;
            CancellationToken token;
            std::function<void()> on_cancel;
        };

        struct Later {
            bool operator()(const Entry &a, const Entry &b) const noexcept {
                return a.deadline != b.deadline ? a.deadline > b.deadline : a.sequence > b.sequence;
            }
        };

        /// One thread, one deadline heap. Entries with a cancellation token make the thread re-check at
        /// `kCancelPoll` so cancellation is prompt without per-token callbacks.
        class TimerService {
          public:
            TimerService() : thread_([this]() {
                Detail::t_callback_thread = true;
                run();
            }) {}

            ~TimerService() {
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                wake_.notify_all();
                thread_.join();
            }

            void add(Entry entry) {
                {
                    std::lock_guard lock(mutex_);
                    entry.sequence = next_sequence_++;
                    const bool new_token = entry.token.can_be_cancelled();
                    any_tokens_ = any_tokens_ || new_token;
                    const bool earliest = heap_.empty() || entry.deadline < heap_.top().deadline;
                    heap_.push(std::move(entry));
                    if (!earliest && !new_token) {
                        return;
                    }
                }
                wake_.notify_all();
            }

          private:
            static constexpr auto kCancelPoll = std::chrono::milliseconds(10);

            void run() {
                std::unique_lock lock(mutex_);
                while (!stopping_) {
                    if (heap_.empty()) {
                        wake_.wait(lock);
                        continue;
                    }
                    // Deliver everything due, plus anything cancelled early.
                    const auto now = Clock::now();
                    std::vector<Entry> fire;
                    std::vector<Entry> keep;
                    bool tokens = false;
                    while (!heap_.empty()) {
                        const Entry &top = heap_.top();
                        const bool cancelled = top.token.is_cancelled();
                        if (top.deadline <= now || cancelled) {
                            fire.push_back(std::move(const_cast<Entry &>(top)));
                            heap_.pop();
                        } else {
                            break;
                        }
                    }
                    // Cancelled entries buried behind the head are found by this sweep.
                    if (any_tokens_) {
                        std::vector<Entry> all;
                        while (!heap_.empty()) {
                            all.push_back(std::move(const_cast<Entry &>(heap_.top())));
                            heap_.pop();
                        }
                        for (Entry &e : all) {
                            if (e.token.is_cancelled()) {
                                fire.push_back(std::move(e));
                            } else {
                                tokens = tokens || e.token.can_be_cancelled();
                                keep.push_back(std::move(e));
                            }
                        }
                        for (Entry &e : keep) {
                            heap_.push(std::move(e));
                        }
                    }
                    any_tokens_ = tokens;
                    if (!fire.empty()) {
                        lock.unlock();
                        for (Entry &e : fire) {
                            if (e.token.is_cancelled() && e.on_cancel) {
                                e.on_cancel();
                            } else {
                                e.fn();
                            }
                        }
                        lock.lock();
                        continue;
                    }
                    if (heap_.empty()) {
                        continue;
                    }
                    auto wake_at = heap_.top().deadline;
                    if (any_tokens_) {
                        wake_at = (std::min)(wake_at, Clock::now() + kCancelPoll);
                    }
                    wake_.wait_until(lock, wake_at);
                }
            }

            std::mutex mutex_;
            std::condition_variable wake_;
            std::priority_queue<Entry, std::vector<Entry>, Later> heap_;
            std::uint64_t next_sequence_ = 0;
            bool any_tokens_ = false;
            bool stopping_ = false;
            std::thread thread_;
        };

        TimerService &service() {
            static TimerService instance;
            return instance;
        }

    } // namespace

    void Detail::schedule_at(Clock::time_point deadline, std::function<void()> fn, CancellationToken token,
                             std::function<void()> on_cancel) {
        service().add(Entry{deadline, 0, std::move(fn), std::move(token), std::move(on_cancel)});
    }

#endif

} // namespace SFT::Async
