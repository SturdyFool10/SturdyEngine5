#pragma once

// Async streams (tokio-stream / futures::Stream): a sequence whose items arrive over time, with the usual adapters.
//
//   auto total = co_await stream_of(paths)                      // Stream<std::string>
//                          .map_concurrent(read_file_text, 8)    // up to 8 IO operations in flight
//                          .par_map(parse, 0)                    // CPU work on the scheduler, 2x cores in flight
//                          .filter([](auto& doc) { return doc.valid; })
//                          .fold(0, [](int n, auto&) { return n + 1; });
//
// A Stream is single-consumer: call `next()` again only after the previous handle completed (`co_await`, the
// terminals and every adapter do). `next()` resolves to an empty optional at the end of the stream and fails with the
// exception of whatever went wrong upstream.
//
// Adapters that take a plain callable (`map`, `filter`, ...) run it inline on the thread that delivers the upstream item
// (a reactor or timer thread for network/timer streams), so keep them cheap; use `par_map` / `map_concurrent` for
// anything heavy.

#include <Async/Channel.hpp>
#include <Async/Promise.hpp>
#include <Async/Runtime.hpp>
#include <Async/Sync.hpp>
#include <Async/Timer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SFT::Async {

    template <typename T>
    class Stream;

    namespace Detail {

        template <typename T>
        struct StreamNode {
            virtual ~StreamNode() = default;
            virtual TaskHandle<std::optional<T>> next() = 0;
        };

        template <typename U>
        struct StepOut {
            enum class Kind { Emit, Skip, End } kind = Kind::Skip;
            std::optional<U> value;
            static StepOut emit(U v) { return {Kind::Emit, std::optional<U>(std::move(v))}; }
            static StepOut skip() { return {Kind::Skip, std::nullopt}; }
            static StepOut end() { return {Kind::End, std::nullopt}; }
        };

        /// Pulls upstream items and feeds them to `step(std::optional<T>) -> StepOut<U>` until it emits or ends.
        /// `step` also sees the upstream end (an empty optional) once, and may emit a final item then (chunking).
        /// Loops without recursion while upstream answers immediately, so long runs of skipped items cost no stack.
        template <typename T, typename U, typename Step>
        class StepNode final : public StreamNode<U>, public std::enable_shared_from_this<StepNode<T, U, Step>> {
          public:
            StepNode(std::shared_ptr<StreamNode<T>> upstream, Step step) : upstream_(std::move(upstream)), step_(std::move(step)) {}

            TaskHandle<std::optional<U>> next() override {
                Promise<std::optional<U>> promise;
                auto handle = promise.handle();
                if (finished_) {
                    (void)promise.set_value(std::optional<U>{});
                    return handle;
                }
                run(promise);
                return handle;
            }

          private:
            void run(Promise<std::optional<U>> promise) {
                for (;;) {
                    auto pulled = upstream_->next();
                    if (!pulled.is_done()) {
                        pulled.on_done([self = this->shared_from_this(), pulled, promise]() mutable { self->resume(pulled, promise); });
                        return;
                    }
                    if (!consume(pulled, promise)) return;
                }
            }

            void resume(TaskHandle<std::optional<T>> pulled, Promise<std::optional<U>> promise) {
                if (consume(pulled, promise)) run(promise);
            }

            /// Returns true when the caller should pull again (the item was skipped).
            bool consume(TaskHandle<std::optional<T>> pulled, Promise<std::optional<U>> &promise) {
                try {
                    auto item = pulled.wait();
                    const bool upstream_ended = !item.has_value();
                    auto out = step_(std::move(item));
                    if (upstream_ended) finished_ = true;
                    switch (out.kind) {
                        case StepOut<U>::Kind::Emit:
                            (void)promise.set_value(std::move(out.value));
                            return false;
                        case StepOut<U>::Kind::End:
                            finished_ = true;
                            (void)promise.set_value(std::optional<U>{});
                            return false;
                        case StepOut<U>::Kind::Skip:
                            if (finished_) {
                                (void)promise.set_value(std::optional<U>{});
                                return false;
                            }
                            return true;
                    }
                } catch (...) {
                    (void)promise.set_exception(std::current_exception());
                }
                return false;
            }

            std::shared_ptr<StreamNode<T>> upstream_;
            Step step_;
            bool finished_ = false;
        };

        template <typename T, typename U, typename Step>
        std::shared_ptr<StreamNode<U>> make_step(std::shared_ptr<StreamNode<T>> upstream, Step step) {
            return std::make_shared<StepNode<T, U, Step>>(std::move(upstream), std::move(step));
        }

        /// Source: values pulled from a callable returning `TaskHandle<optional<T>>`.
        template <typename T, typename Fn>
        struct FnNode final : StreamNode<T> {
            explicit FnNode(Fn fn) : fn_(std::move(fn)) {}
            TaskHandle<std::optional<T>> next() override { return fn_(); }
            Fn fn_;
        };

        /// Runs a bounded number of per-item operations concurrently. `launch(T) -> TaskHandle<U>`.
        /// Ordered: results come out in input order. Unordered: in completion order (faster stages overtake).
        template <typename T, typename U, typename Launch>
        class ConcurrentNode final : public StreamNode<U>, public std::enable_shared_from_this<ConcurrentNode<T, U, Launch>> {
            struct Slot {
                bool finished = false;
                std::optional<U> value;
                std::exception_ptr error;
            };
            struct Inner {
                std::deque<std::shared_ptr<Slot>> order;     ///< in flight or finished-but-undelivered, input order
                std::deque<std::shared_ptr<Slot>> completed; ///< finished slots in completion order (unordered mode)
                bool upstream_ended = false;
                bool pulling = false;
                std::optional<Promise<std::optional<U>>> consumer;
            };

          public:
            ConcurrentNode(std::shared_ptr<StreamNode<T>> upstream, Launch launch, std::size_t limit, bool ordered)
                : upstream_(std::move(upstream)), launch_(std::move(launch)), limit_((std::max)(limit, std::size_t{1})), ordered_(ordered) {}

            TaskHandle<std::optional<U>> next() override {
                Promise<std::optional<U>> promise;
                auto handle = promise.handle();
                {
                    auto guard = state_.lock();
                    if (guard->consumer) throw std::logic_error("Stream::next() called again before the previous call completed");
                    guard->consumer = promise;
                }
                pump();
                deliver();
                return handle;
            }

          private:
            void pump() {
                for (;;) {
                    {
                        auto guard = state_.lock();
                        if (guard->pulling || guard->upstream_ended || guard->order.size() >= limit_) return;
                        guard->pulling = true;
                    }
                    auto pulled = upstream_->next();
                    if (!pulled.is_done()) {
                        pulled.on_done([self = this->shared_from_this(), pulled]() mutable {
                            self->on_pulled(pulled);
                            self->pump();
                            self->deliver();
                        });
                        return;
                    }
                    on_pulled(pulled);
                }
            }

            void on_pulled(TaskHandle<std::optional<T>> pulled) {
                std::optional<T> item;
                std::exception_ptr error;
                try {
                    item = pulled.wait();
                } catch (...) {
                    error = std::current_exception();
                }
                if (error) { // an upstream failure becomes a failed slot, and the stream ends after it
                    auto slot = std::make_shared<Slot>();
                    slot->finished = true;
                    slot->error = error;
                    auto guard = state_.lock();
                    guard->order.push_back(slot);
                    guard->completed.push_back(slot);
                    guard->upstream_ended = true;
                    guard->pulling = false;
                    return;
                }
                if (!item) {
                    auto guard = state_.lock();
                    guard->upstream_ended = true;
                    guard->pulling = false;
                    return;
                }
                auto slot = std::make_shared<Slot>();
                TaskHandle<U> work;
                try {
                    work = launch_(std::move(*item));
                } catch (...) {
                    slot->finished = true;
                    slot->error = std::current_exception();
                    auto guard = state_.lock();
                    guard->order.push_back(slot);
                    guard->completed.push_back(slot);
                    guard->pulling = false;
                    return;
                }
                {
                    auto guard = state_.lock();
                    guard->order.push_back(slot);
                    guard->pulling = false;
                }
                work.on_done([self = this->shared_from_this(), slot, work]() mutable {
                    try {
                        slot->value.emplace(work.wait());
                    } catch (...) {
                        slot->error = std::current_exception();
                    }
                    {
                        auto guard = self->state_.lock();
                        slot->finished = true;
                        guard->completed.push_back(slot);
                    }
                    self->deliver();
                    self->pump();
                });
            }

            void deliver() {
                std::optional<Promise<std::optional<U>>> consumer;
                std::shared_ptr<Slot> slot;
                bool end = false;
                {
                    auto guard = state_.lock();
                    if (!guard->consumer) return;
                    if (ordered_) {
                        if (!guard->order.empty() && guard->order.front()->finished) {
                            slot = guard->order.front();
                            guard->order.pop_front();
                            std::erase(guard->completed, slot);
                        }
                    } else if (!guard->completed.empty()) {
                        slot = guard->completed.front();
                        guard->completed.pop_front();
                        std::erase(guard->order, slot);
                    }
                    if (!slot) {
                        if (guard->order.empty() && guard->upstream_ended && !guard->pulling) end = true;
                        else return;
                    }
                    consumer = std::move(guard->consumer);
                    guard->consumer.reset();
                }
                if (slot) {
                    if (slot->error) (void)consumer->set_exception(slot->error);
                    else (void)consumer->set_value(std::optional<U>(std::move(slot->value)));
                } else if (end) {
                    (void)consumer->set_value(std::optional<U>{});
                }
                pump();
            }

            std::shared_ptr<StreamNode<T>> upstream_;
            Launch launch_;
            std::size_t limit_;
            bool ordered_;
            Mutex<Inner> state_;
        };

        template <typename T>
        struct ChainNode final : StreamNode<T>, std::enable_shared_from_this<ChainNode<T>> {
            ChainNode(std::shared_ptr<StreamNode<T>> a, std::shared_ptr<StreamNode<T>> b) : first(std::move(a)), second(std::move(b)) {}
            TaskHandle<std::optional<T>> next() override {
                if (first_done) return second->next();
                Promise<std::optional<T>> promise;
                auto handle = promise.handle();
                auto pulled = first->next();
                pulled.on_done([self = this->shared_from_this(), pulled, promise]() mutable {
                    try {
                        auto item = pulled.wait();
                        if (item) return (void)promise.set_value(std::move(item));
                        self->first_done = true;
                        auto rest = self->second->next();
                        rest.on_done([rest, promise]() mutable {
                            try {
                                (void)promise.set_value(rest.wait());
                            } catch (...) {
                                (void)promise.set_exception(std::current_exception());
                            }
                        });
                    } catch (...) {
                        (void)promise.set_exception(std::current_exception());
                    }
                });
                return handle;
            }
            std::shared_ptr<StreamNode<T>> first, second;
            bool first_done = false;
        };

        template <typename T, typename U>
        struct ZipNode final : StreamNode<std::pair<T, U>> {
            ZipNode(std::shared_ptr<StreamNode<T>> a, std::shared_ptr<StreamNode<U>> b) : left(std::move(a)), right(std::move(b)) {}
            TaskHandle<std::optional<std::pair<T, U>>> next() override {
                auto both = when_all(left->next(), right->next());
                return map_inline(std::move(both), [](std::tuple<std::optional<T>, std::optional<U>> pair) -> std::optional<std::pair<T, U>> {
                    auto &[a, b] = pair;
                    if (!a || !b) return std::nullopt;
                    return std::pair<T, U>(std::move(*a), std::move(*b));
                });
            }
            std::shared_ptr<StreamNode<T>> left;
            std::shared_ptr<StreamNode<U>> right;
        };

        /// Interleaves two streams by readiness: whichever has an item first goes first.
        template <typename T>
        class MergeNode final : public StreamNode<T>, public std::enable_shared_from_this<MergeNode<T>> {
            struct Side {
                std::shared_ptr<StreamNode<T>> node;
                bool pulling = false;
                bool ended = false;
            };
            struct Inner {
                std::deque<std::pair<std::optional<T>, std::exception_ptr>> ready;
                std::optional<Promise<std::optional<T>>> consumer;
            };

          public:
            MergeNode(std::shared_ptr<StreamNode<T>> a, std::shared_ptr<StreamNode<T>> b) : sides_{Side{std::move(a)}, Side{std::move(b)}} {}

            TaskHandle<std::optional<T>> next() override {
                Promise<std::optional<T>> promise;
                auto handle = promise.handle();
                {
                    auto guard = state_.lock();
                    guard->consumer = promise;
                }
                pump();
                deliver();
                return handle;
            }

          private:
            void pump() {
                for (std::size_t i = 0; i < 2; ++i) {
                    {
                        auto guard = state_.lock();
                        if (sides_[i].pulling || sides_[i].ended || !guard->consumer) continue; // pull only while someone waits
                        sides_[i].pulling = true;
                    }
                    auto pulled = sides_[i].node->next();
                    pulled.on_done([self = this->shared_from_this(), i, pulled]() mutable {
                        std::optional<T> item;
                        std::exception_ptr error;
                        try {
                            item = pulled.wait();
                        } catch (...) {
                            error = std::current_exception();
                        }
                        {
                            auto guard = self->state_.lock();
                            self->sides_[i].pulling = false;
                            if (!item && !error) self->sides_[i].ended = true;
                            else guard->ready.emplace_back(std::move(item), error);
                        }
                        self->deliver();
                    });
                }
            }

            void deliver() {
                std::optional<Promise<std::optional<T>>> consumer;
                std::optional<std::pair<std::optional<T>, std::exception_ptr>> item;
                bool end = false;
                {
                    auto guard = state_.lock();
                    if (!guard->consumer) return;
                    if (!guard->ready.empty()) {
                        item = std::move(guard->ready.front());
                        guard->ready.pop_front();
                    } else if (sides_[0].ended && sides_[1].ended) {
                        end = true;
                    } else {
                        return;
                    }
                    consumer = std::move(guard->consumer);
                    guard->consumer.reset();
                }
                if (item && item->second) (void)consumer->set_exception(item->second);
                else if (item) (void)consumer->set_value(std::move(item->first));
                else if (end) (void)consumer->set_value(std::optional<T>{});
            }

            Side sides_[2];
            Mutex<Inner> state_;
        };

        template <typename T>
        TaskHandle<std::vector<T>> collect_impl(std::shared_ptr<StreamNode<T>> node) {
            std::vector<T> out;
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                out.push_back(std::move(*item));
            }
            co_return out;
        }

        template <typename T, typename Fn>
        TaskHandle<void> for_each_impl(std::shared_ptr<StreamNode<T>> node, Fn fn) {
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                if constexpr (std::is_void_v<std::invoke_result_t<Fn &, T>>) {
                    fn(std::move(*item));
                } else {
                    co_await fn(std::move(*item)); // an async body: finishes before the next item is pulled
                }
            }
        }

        template <typename T, typename A, typename Fn>
        TaskHandle<A> fold_impl(std::shared_ptr<StreamNode<T>> node, A init, Fn fn) {
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                init = fn(std::move(init), std::move(*item));
            }
            co_return init;
        }

        template <typename T>
        TaskHandle<std::size_t> count_impl(std::shared_ptr<StreamNode<T>> node) {
            std::size_t n = 0;
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                ++n;
            }
            co_return n;
        }

        template <typename T>
        TaskHandle<std::optional<T>> last_impl(std::shared_ptr<StreamNode<T>> node) {
            std::optional<T> last;
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                last = std::move(item);
            }
            co_return last;
        }

        template <typename T>
        TaskHandle<void> forward_impl(std::shared_ptr<StreamNode<T>> node, Sender<T> sender) {
            for (;;) {
                auto item = co_await node->next();
                if (!item) break;
                co_await sender.send(std::move(*item)); // waits when the channel is full: back-pressure reaches the source
            }
        }

    } // namespace Detail

    template <typename T>
    class Stream {
      public:
        using value_type = T;

        Stream() = default;
        explicit Stream(std::shared_ptr<Detail::StreamNode<T>> node) : node_(std::move(node)) {}

        [[nodiscard]] TaskHandle<std::optional<T>> next() { return node_->next(); }
        [[nodiscard]] explicit operator bool() const noexcept { return node_ != nullptr; }
        [[nodiscard]] const std::shared_ptr<Detail::StreamNode<T>> &node() const noexcept { return node_; }

        // ---- cheap adapters (run inline where the upstream item is delivered) ----
        template <typename Fn>
        [[nodiscard]] auto map(Fn fn) && {
            using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
            return Stream<U>(Detail::make_step<T, U>(std::move(node_), [fn](std::optional<T> item) mutable {
                return item ? Detail::StepOut<U>::emit(fn(std::move(*item))) : Detail::StepOut<U>::end();
            }));
        }
        template <typename Pred>
        [[nodiscard]] Stream<T> filter(Pred pred) && {
            return Stream<T>(Detail::make_step<T, T>(std::move(node_), [pred](std::optional<T> item) mutable {
                if (!item) return Detail::StepOut<T>::end();
                return pred(*item) ? Detail::StepOut<T>::emit(std::move(*item)) : Detail::StepOut<T>::skip();
            }));
        }
        template <typename Fn>
        [[nodiscard]] auto filter_map(Fn fn) && {
            using Opt = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
            using U = typename Opt::value_type;
            return Stream<U>(Detail::make_step<T, U>(std::move(node_), [fn](std::optional<T> item) mutable {
                if (!item) return Detail::StepOut<U>::end();
                auto out = fn(std::move(*item));
                return out ? Detail::StepOut<U>::emit(std::move(*out)) : Detail::StepOut<U>::skip();
            }));
        }
        /// The first `n` items. Stops without pulling an (n+1)th, so it is safe on streams that may never produce more.
        [[nodiscard]] Stream<T> take(std::size_t n) && {
            struct Node final : Detail::StreamNode<T> {
                Node(std::shared_ptr<Detail::StreamNode<T>> up, std::size_t count) : upstream(std::move(up)), remaining(count) {}
                TaskHandle<std::optional<T>> next() override {
                    if (remaining == 0) return ready(std::optional<T>{});
                    --remaining;
                    return upstream->next();
                }
                std::shared_ptr<Detail::StreamNode<T>> upstream;
                std::size_t remaining;
            };
            return Stream<T>(std::make_shared<Node>(std::move(node_), n));
        }
        [[nodiscard]] Stream<T> skip(std::size_t n) && {
            return Stream<T>(Detail::make_step<T, T>(std::move(node_), [n](std::optional<T> item) mutable {
                if (!item) return Detail::StepOut<T>::end();
                if (n > 0) {
                    --n;
                    return Detail::StepOut<T>::skip();
                }
                return Detail::StepOut<T>::emit(std::move(*item));
            }));
        }
        template <typename Pred>
        [[nodiscard]] Stream<T> take_while(Pred pred) && {
            return Stream<T>(Detail::make_step<T, T>(std::move(node_), [pred](std::optional<T> item) mutable {
                if (!item || !pred(*item)) return Detail::StepOut<T>::end();
                return Detail::StepOut<T>::emit(std::move(*item));
            }));
        }
        [[nodiscard]] auto enumerate() && {
            using U = std::pair<std::size_t, T>;
            return Stream<U>(Detail::make_step<T, U>(std::move(node_), [index = std::size_t{0}](std::optional<T> item) mutable {
                return item ? Detail::StepOut<U>::emit(U(index++, std::move(*item))) : Detail::StepOut<U>::end();
            }));
        }
        template <typename Fn>
        [[nodiscard]] Stream<T> inspect(Fn fn) && {
            return Stream<T>(Detail::make_step<T, T>(std::move(node_), [fn](std::optional<T> item) mutable {
                if (!item) return Detail::StepOut<T>::end();
                fn(*item);
                return Detail::StepOut<T>::emit(std::move(*item));
            }));
        }
        /// Groups items into vectors of `n` (the last may be shorter).
        [[nodiscard]] Stream<std::vector<T>> chunks(std::size_t n) && {
            n = (std::max)(n, std::size_t{1});
            return Stream<std::vector<T>>(Detail::make_step<T, std::vector<T>>(std::move(node_), [n, buffer = std::vector<T>()](std::optional<T> item) mutable {
                using Out = Detail::StepOut<std::vector<T>>;
                if (!item) return buffer.empty() ? Out::end() : Out::emit(std::exchange(buffer, {}));
                buffer.push_back(std::move(*item));
                if (buffer.size() < n) return Out::skip();
                return Out::emit(std::exchange(buffer, {}));
            }));
        }

        // ---- combining ----
        [[nodiscard]] Stream<T> chain(Stream<T> other) && {
            return Stream<T>(std::make_shared<Detail::ChainNode<T>>(std::move(node_), std::move(other.node_)));
        }
        template <typename U>
        [[nodiscard]] Stream<std::pair<T, U>> zip(Stream<U> other) && {
            return Stream<std::pair<T, U>>(std::make_shared<Detail::ZipNode<T, U>>(std::move(node_), std::move(other.node())));
        }
        /// Items from both streams as they become ready; ends when both ended.
        [[nodiscard]] Stream<T> merge(Stream<T> other) && {
            return Stream<T>(std::make_shared<Detail::MergeNode<T>>(std::move(node_), std::move(other.node_)));
        }

        // ---- concurrency ----
        /// `fn(item)` returns a TaskHandle (an IO call, a request, a coroutine); up to `concurrency` run at once.
        /// `ordered` keeps input order (a slow item delays later ones); unordered yields results as they finish.
        template <typename Fn>
        [[nodiscard]] auto map_concurrent(Fn fn, std::size_t concurrency, bool ordered = true) && {
            using Handle = std::invoke_result_t<Fn &, T>;
            using U = decltype(std::declval<Handle &>().wait());
            return Stream<U>(std::make_shared<Detail::ConcurrentNode<T, U, Fn>>(std::move(node_), std::move(fn), concurrency, ordered));
        }
        /// CPU-bound `fn(item) -> U` runs on runtime `Rt`'s workers, up to `concurrency` items at once (0 = twice the
        /// worker count). Pipelines IO and compute: while the scheduler crunches item k, items k+1... are being read.
        template <AsyncRuntime Rt = DefaultRuntime, typename Fn>
        [[nodiscard]] auto par_map(Fn fn, std::size_t concurrency = 0, bool ordered = true) && {
            using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
            if (concurrency == 0) concurrency = (std::max)(std::size_t{2}, current_num_threads<Rt>() * 2);
            auto launch = [fn](T item) { return Rt::spawn([fn, item = std::move(item)]() mutable { return fn(std::move(item)); }); };
            return Stream<U>(std::make_shared<Detail::ConcurrentNode<T, U, decltype(launch)>>(std::move(node_), std::move(launch), concurrency, ordered));
        }
        /// Sequential async stage: `fn(item)` returns a TaskHandle, finished before the next item is pulled.
        template <typename Fn>
        [[nodiscard]] auto then(Fn fn) && {
            return std::move(*this).map_concurrent(std::move(fn), 1, true);
        }

        // ---- streams of futures ----
        /// For a `Stream<TaskHandle<U>>`: awaits up to `n` of the handles at once, yielding results in stream order.
        template <typename U = T>
            requires requires { typename Detail::HandleValue<U>::type; }
        [[nodiscard]] auto buffered(std::size_t n) && {
            return std::move(*this).map_concurrent([](T handle) { return handle; }, n, true);
        }
        /// Like `buffered`, but results come out as they finish, whatever their position.
        template <typename U = T>
            requires requires { typename Detail::HandleValue<U>::type; }
        [[nodiscard]] auto buffer_unordered(std::size_t n) && {
            return std::move(*this).map_concurrent([](T handle) { return handle; }, n, false);
        }

        /// Batches of up to `n` items, emitted early (with whatever has arrived) once the batch's first item has waited
        /// `max_wait`. Never emits an empty batch. The classic shape for "flush every 100 records or every 50 ms".
        [[nodiscard]] Stream<std::vector<T>> chunks_timeout(std::size_t n, Clock::duration max_wait) && {
            using Batch = std::vector<T>;
            struct Node final : Detail::StreamNode<Batch>, std::enable_shared_from_this<Node> {
                struct Inner {
                    Batch batch;
                    bool pulling = false;
                    bool upstream_ended = false;
                    bool timed_out = false;
                    std::exception_ptr error;
                    std::uint64_t epoch = 0;
                    std::optional<Promise<std::optional<Batch>>> consumer;
                };
                Node(std::shared_ptr<Detail::StreamNode<T>> up, std::size_t count, Clock::duration wait)
                    : upstream(std::move(up)), limit((std::max)(count, std::size_t{1})), max_wait(wait) {}

                TaskHandle<std::optional<Batch>> next() override {
                    Promise<std::optional<Batch>> promise;
                    auto handle = promise.handle();
                    {
                        auto guard = state.lock();
                        if (guard->consumer) throw std::logic_error("Stream::next() called again before the previous call completed");
                        guard->consumer = promise;
                    }
                    pump();
                    deliver();
                    return handle;
                }

                void pump() {
                    for (;;) {
                        {
                            auto guard = state.lock();
                            // Pull only while someone is waiting and the batch has room.
                            if (!guard->consumer || guard->pulling || guard->upstream_ended || guard->batch.size() >= limit) return;
                            guard->pulling = true;
                        }
                        auto pulled = upstream->next();
                        if (!pulled.is_done()) {
                            pulled.on_done([self = this->shared_from_this(), pulled]() mutable {
                                self->on_pulled(pulled);
                                self->deliver();
                                self->pump();
                            });
                            return;
                        }
                        on_pulled(pulled);
                        deliver();
                    }
                }

                void on_pulled(TaskHandle<std::optional<T>> pulled) {
                    std::optional<T> item;
                    std::exception_ptr failure;
                    try {
                        item = pulled.wait();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    std::uint64_t arm_for = 0;
                    {
                        auto guard = state.lock();
                        guard->pulling = false;
                        if (failure) {
                            guard->error = failure;
                            guard->upstream_ended = true;
                        } else if (!item) {
                            guard->upstream_ended = true;
                        } else {
                            if (guard->batch.empty()) arm_for = ++guard->epoch; // a new batch starts its clock with its first item
                            guard->batch.push_back(std::move(*item));
                        }
                    }
                    if (arm_for != 0) {
                        auto timer = sleep_for(max_wait);
                        timer.on_done([self = this->shared_from_this(), timer, arm_for]() mutable {
                            {
                                auto guard = self->state.lock();
                                if (guard->epoch == arm_for && !guard->batch.empty()) guard->timed_out = true;
                            }
                            self->deliver();
                        });
                    }
                }

                void deliver() {
                    std::optional<Promise<std::optional<Batch>>> consumer;
                    std::optional<Batch> batch;
                    std::exception_ptr error;
                    bool end = false;
                    {
                        auto guard = state.lock();
                        if (!guard->consumer) return;
                        const bool full = guard->batch.size() >= limit;
                        if (full || (guard->timed_out && !guard->batch.empty()) || (guard->upstream_ended && !guard->batch.empty())) {
                            batch = std::move(guard->batch);
                            guard->batch.clear();
                            guard->timed_out = false;
                            ++guard->epoch; // invalidates the clock of the batch just taken
                        } else if (guard->upstream_ended && guard->error) {
                            error = std::exchange(guard->error, nullptr);
                        } else if (guard->upstream_ended) {
                            end = true;
                        } else {
                            return;
                        }
                        consumer = std::move(guard->consumer);
                        guard->consumer.reset();
                    }
                    if (batch) (void)consumer->set_value(std::move(batch));
                    else if (error) (void)consumer->set_exception(error);
                    else if (end) (void)consumer->set_value(std::optional<Batch>{});
                }

                std::shared_ptr<Detail::StreamNode<T>> upstream;
                std::size_t limit;
                Clock::duration max_wait;
                Mutex<Inner> state;
            };
            return Stream<Batch>(std::make_shared<Node>(std::move(node_), n, max_wait));
        }

        // ---- timing ----
        /// At most one item per `period`.
        [[nodiscard]] Stream<T> throttle(Clock::duration period) && {
            struct Node final : Detail::StreamNode<T>, std::enable_shared_from_this<Node> {
                Node(std::shared_ptr<Detail::StreamNode<T>> up, Clock::duration p) : upstream(std::move(up)), period(p) {}
                TaskHandle<std::optional<T>> next() override {
                    Promise<std::optional<T>> promise;
                    auto handle = promise.handle();
                    auto pulled = upstream->next();
                    pulled.on_done([self = this->shared_from_this(), pulled, promise]() mutable {
                        try {
                            auto item = pulled.wait();
                            if (!item) return (void)promise.set_value(std::move(item));
                            const auto due = (std::max)(Clock::now(), self->last_emit + self->period);
                            self->last_emit = due;
                            if (due <= Clock::now()) return (void)promise.set_value(std::move(item));
                            auto held = std::make_shared<std::optional<T>>(std::move(item));
                            auto wait = sleep_until(due);
                            wait.on_done([wait, promise, held]() mutable { // the timer only fails when cancelled; either way the item goes out
                                (void)promise.set_value(std::move(*held));
                            });
                        } catch (...) {
                            (void)promise.set_exception(std::current_exception());
                        }
                    });
                    return handle;
                }
                std::shared_ptr<Detail::StreamNode<T>> upstream;
                Clock::duration period;
                Clock::time_point last_emit = Clock::now() - std::chrono::hours(24);
            };
            return Stream<T>(std::make_shared<Node>(std::move(node_), period));
        }
        /// An item that takes longer than `limit` to arrive fails that `next()` with `TimeoutError`. The slow pull is
        /// still outstanding afterwards, so treat the stream as finished.
        [[nodiscard]] Stream<T> timeout_each(Clock::duration limit) && {
            struct Node final : Detail::StreamNode<T> {
                Node(std::shared_ptr<Detail::StreamNode<T>> up, Clock::duration l) : upstream(std::move(up)), limit(l) {}
                TaskHandle<std::optional<T>> next() override { return timeout(upstream->next(), limit); }
                std::shared_ptr<Detail::StreamNode<T>> upstream;
                Clock::duration limit;
            };
            return Stream<T>(std::make_shared<Node>(std::move(node_), limit));
        }

        // ---- terminals ----
        [[nodiscard]] TaskHandle<std::vector<T>> collect() && { return Detail::collect_impl<T>(std::move(node_)); }
        /// `fn(item)` may return void or a TaskHandle (awaited before the next item).
        template <typename Fn>
        [[nodiscard]] TaskHandle<void> for_each(Fn fn) && {
            return Detail::for_each_impl<T, Fn>(std::move(node_), std::move(fn));
        }
        template <typename A, typename Fn>
        [[nodiscard]] TaskHandle<A> fold(A init, Fn fn) && {
            return Detail::fold_impl<T, A, Fn>(std::move(node_), std::move(init), std::move(fn));
        }
        [[nodiscard]] TaskHandle<std::size_t> count() && { return Detail::count_impl<T>(std::move(node_)); }
        [[nodiscard]] TaskHandle<std::optional<T>> last() && { return Detail::last_impl<T>(std::move(node_)); }
        /// Pushes every item into `sender` (waiting when it is full) until the stream ends.
        [[nodiscard]] TaskHandle<void> forward_to(Sender<T> sender) && { return Detail::forward_impl<T>(std::move(node_), std::move(sender)); }

      private:
        std::shared_ptr<Detail::StreamNode<T>> node_;
    };

    // =============================================================================================
    // Sources
    // =============================================================================================

    /// A stream whose items come from `fn() -> TaskHandle<std::optional<T>>` (empty optional ends it).
    template <typename Fn>
    [[nodiscard]] auto stream_from(Fn fn) {
        using T = typename Detail::HandleValue<std::invoke_result_t<Fn &>>::type::value_type;
        return Stream<T>(std::make_shared<Detail::FnNode<T, Fn>>(std::move(fn)));
    }

    /// The items of a range (copied into the stream), already available: no waiting.
    template <std::ranges::input_range R>
    [[nodiscard]] auto stream_of(R &&range) {
        using T = std::ranges::range_value_t<R>;
        auto items = std::make_shared<std::vector<T>>();
        for (auto &&item : range) items->emplace_back(std::forward<decltype(item)>(item));
        return stream_from([items, index = std::size_t{0}]() mutable -> TaskHandle<std::optional<T>> {
            if (index >= items->size()) return ready(std::optional<T>{});
            return ready(std::optional<T>(std::move((*items)[index++])));
        });
    }

    template <typename T>
    [[nodiscard]] Stream<T> stream_empty() {
        return stream_from([]() -> TaskHandle<std::optional<T>> { return ready(std::optional<T>{}); });
    }

    template <typename T>
    [[nodiscard]] Stream<T> stream_once(T value) {
        return stream_from([value = std::optional<T>(std::move(value))]() mutable -> TaskHandle<std::optional<T>> { return ready(std::exchange(value, std::nullopt)); });
    }

    /// Calls `fn()` for every item, forever (`take` it, or end it with an exception).
    template <typename Fn>
    [[nodiscard]] auto stream_repeat_with(Fn fn) {
        using T = std::remove_cvref_t<std::invoke_result_t<Fn &>>;
        return stream_from([fn]() mutable -> TaskHandle<std::optional<T>> { return ready(std::optional<T>(fn())); });
    }

    /// Everything arriving on a channel; ends when the channel is closed and drained.
    template <typename T>
    [[nodiscard]] Stream<T> stream_of_channel(Receiver<T> receiver) {
        return stream_from([receiver]() mutable { return receiver.recv(); });
    }

    /// One item per period (the tick time). Missed ticks are skipped, never delivered in a burst.
    [[nodiscard]] inline Stream<Clock::time_point> stream_interval(Clock::duration period) {
        auto ticker = std::make_shared<Interval>(period);
        return stream_from([ticker]() mutable {
            return Detail::map_inline(ticker->tick(), [] { return std::optional<Clock::time_point>(Clock::now()); });
        });
    }

} // namespace SFT::Async
