#include <Async/BindingInternal.hpp>

#include <Async/Affinity.hpp>
#include <Async/Cancellation.hpp>
#include <Async/Channel.hpp>
#include <Async/Context.hpp>
#include <Async/IoHandler.hpp>
#include <Async/MainThread.hpp>
#include <Async/Net.hpp>
#include <Async/Parallel.hpp>
#include <Async/Scheduler.hpp>
#include <Async/Signal.hpp>
#include <Async/Sync.hpp>
#include <Async/Tasks.hpp>
#include <Async/ThreadPool.hpp>
#include <Async/Timer.hpp>
#include <Async/Topology.hpp>

#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>

namespace SFT::Async::Binding {

    using namespace Detail;

    namespace {
        constexpr const char *kCancelSource = "cancel_source";
        constexpr const char *kCancelToken = "cancel_token";
        constexpr const char *kChanSender = "channel_sender";
        constexpr const char *kChanReceiver = "channel_receiver";
        constexpr const char *kOneshotSender = "oneshot_sender";
        constexpr const char *kOneshotReceiver = "oneshot_receiver";
        constexpr const char *kWatchSender = "watch_sender";
        constexpr const char *kWatchReceiver = "watch_receiver";
        constexpr const char *kBroadcastSender = "broadcast_sender";
        constexpr const char *kBroadcastReceiver = "broadcast_receiver";
        constexpr const char *kSemaphore = "semaphore";
        constexpr const char *kPermit = "permit";
        constexpr const char *kMutex = "async_mutex";
        constexpr const char *kMutexGuard = "mutex_guard";
        constexpr const char *kRwLock = "async_rwlock";
        constexpr const char *kReadGuard = "rwlock_read_guard";
        constexpr const char *kWriteGuard = "rwlock_write_guard";
        constexpr const char *kNotify = "notify";
        constexpr const char *kBarrier = "barrier";
        constexpr const char *kPool = "pool";
        constexpr const char *kJoinSet = "join_set";
        constexpr const char *kLocalSet = "local_set";
        constexpr const char *kDedicated = "dedicated_thread";
        constexpr const char *kTaskLocal = "task_local";
        constexpr const char *kInterval = "interval";

        bool is_kind(const ObjectPtr &object, const char *kind) { return object && std::strcmp(object->kind(), kind) == 0; }

        /// Keeps `registration` alive exactly as long as `op` lives or is pending.
        void hold_until_done(const OpPtr &op, std::shared_ptr<CancellationRegistration> registration) {
            op->on_complete([registration = std::move(registration)]() mutable { registration.reset(); });
        }

        /// Running a task body: exceptions become the task's error.
        TaskOutput run_task(const TaskFn &fn, const CancellationToken &token) {
            try {
                return fn(make_object<CancellationToken>(kCancelToken, token));
            } catch (const std::exception &error) {
                TaskOutput out;
                out.error = error.what();
                return out;
            } catch (...) {
                TaskOutput out;
                out.error = "task failed";
                return out;
            }
        }
        void fill_output(TaskOutput &&out, Value &v) {
            if (!out.error.empty()) throw std::runtime_error(out.error);
            v.number = out.number;
            v.bytes = std::move(out.bytes);
            v.text = std::move(out.text);
        }
    } // namespace

    // =================================================================================================================
    // Cancellation
    // =================================================================================================================
    ObjectPtr cancel_source_new() { return make_object<CancellationSource>(kCancelSource, CancellationSource{}); }
    bool cancel_source_cancel(const ObjectPtr &source) { return get<CancellationSource>(source, kCancelSource).cancel(); }
    bool cancel_source_is_cancelled(const ObjectPtr &source) { return get<CancellationSource>(source, kCancelSource).is_cancelled(); }
    ObjectPtr cancel_source_token(const ObjectPtr &source) { return make_object<CancellationToken>(kCancelToken, get<CancellationSource>(source, kCancelSource).token()); }
    bool cancel_token_is_cancelled(const ObjectPtr &token) { return get<CancellationToken>(token, kCancelToken).is_cancelled(); }

    OpPtr cancel_token_wait(const ObjectPtr &token) {
        auto &t = get<CancellationToken>(token, kCancelToken);
        auto op = std::make_shared<Op>();
        auto registration = std::make_shared<CancellationRegistration>();
        *registration = t.on_cancel([weak = std::weak_ptr<Op>(op)] {
            if (auto o = weak.lock()) o->complete(Value{});
        });
        hold_until_done(op, registration);
        return op;
    }

    void op_link_cancel(const OpPtr &op, const ObjectPtr &token) {
        auto &t = get<CancellationToken>(token, kCancelToken);
        auto registration = std::make_shared<CancellationRegistration>();
        *registration = t.on_cancel([weak = std::weak_ptr<Op>(op)] {
            if (auto o = weak.lock()) o->cancel();
        });
        hold_until_done(op, registration);
    }

    OpPtr op_with_timeout(const OpPtr &op, std::int64_t timeout_ms) {
        auto out = std::make_shared<Op>();
        CancellationSource timer_cancel;
        op->on_complete([out, op, timer_cancel]() mutable {
            (void)timer_cancel.cancel();
            out->complete(Value(op->result()));
        });
        // Cancelling the new op cancels the wrapped one.
        auto registration = std::make_shared<CancellationRegistration>();
        *registration = out->token().on_cancel([weak = std::weak_ptr<Op>(op)] {
            if (auto o = weak.lock()) o->cancel();
        });
        hold_until_done(out, registration);
        auto timer = SFT::Async::sleep_for(ms(timeout_ms), timer_cancel.token());
        timer.on_done([out, op, timer]() mutable {
            try {
                timer.wait();
            } catch (...) {
                return; // cancelled because the op finished first
            }
            if (out->done()) return;
            out->fail(IoErrorCode::TimedOut, "the operation timed out");
            op->cancel();
        });
        return out;
    }

    // =================================================================================================================
    // Channels
    // =================================================================================================================
    ChannelEnds channel_create(std::uint64_t capacity) {
        auto pair = channel<Bytes>(static_cast<std::size_t>(capacity));
        return {make_object<Sender<Bytes>>(kChanSender, std::move(pair.sender)), make_object<Receiver<Bytes>>(kChanReceiver, std::move(pair.receiver))};
    }
    ObjectPtr channel_sender_clone(const ObjectPtr &sender) { return make_object<Sender<Bytes>>(kChanSender, get<Sender<Bytes>>(sender, kChanSender)); }
    ObjectPtr channel_receiver_clone(const ObjectPtr &receiver) { return make_object<Receiver<Bytes>>(kChanReceiver, get<Receiver<Bytes>>(receiver, kChanReceiver)); }
    OpPtr channel_send(const ObjectPtr &sender, Bytes data) {
        auto s = get<Sender<Bytes>>(sender, kChanSender);
        return launch([s, data = std::move(data)](CancellationToken) mutable { return s.send(std::move(data)); }, [](Value &) {});
    }
    TryStatus channel_try_send(const ObjectPtr &sender, Bytes data) {
        auto result = get<Sender<Bytes>>(sender, kChanSender).try_send(std::move(data));
        if (result) return TryStatus::Ok;
        return result.error() == TrySendError::Full ? TryStatus::Full : TryStatus::Closed;
    }
    OpPtr channel_recv(const ObjectPtr &receiver) {
        auto r = get<Receiver<Bytes>>(receiver, kChanReceiver);
        return launch([r](CancellationToken token) { return r.recv(std::move(token)); }, [](std::optional<Bytes> &&message, Value &out) {
            out.number = message ? 1 : 0;
            if (message) out.bytes = std::move(*message);
        });
    }
    TryStatus channel_try_recv(const ObjectPtr &receiver, Bytes *out) {
        auto result = get<Receiver<Bytes>>(receiver, kChanReceiver).try_recv();
        if (result) {
            if (out) *out = std::move(*result);
            return TryStatus::Ok;
        }
        return result.error() == TryRecvError::Empty ? TryStatus::Empty : TryStatus::Closed;
    }
    void channel_close(const ObjectPtr &end) {
        if (is_kind(end, kChanSender)) get<Sender<Bytes>>(end, kChanSender).close();
        else get<Receiver<Bytes>>(end, kChanReceiver).close();
    }

    namespace {
        struct OneshotSender {
            Promise<Bytes> promise;
            ~OneshotSender() { (void)promise.set_exception(std::make_exception_ptr(ChannelClosed{})); } // a no-op once a value was set
        };
        struct OneshotReceiver {
            std::mutex mutex;
            std::optional<TaskHandle<Bytes>> handle;
        };
    } // namespace

    ChannelEnds oneshot_create() {
        auto sender = std::make_shared<OneshotSender>();
        auto receiver = std::make_shared<OneshotReceiver>();
        receiver->handle = sender->promise.handle();
        return {make_object<std::shared_ptr<OneshotSender>>(kOneshotSender, std::move(sender)),
                make_object<std::shared_ptr<OneshotReceiver>>(kOneshotReceiver, std::move(receiver))};
    }
    bool oneshot_send(const ObjectPtr &sender, Bytes data) { return get<std::shared_ptr<OneshotSender>>(sender, kOneshotSender)->promise.set_value(std::move(data)); }
    bool oneshot_fail(const ObjectPtr &sender, const std::string &message) {
        return get<std::shared_ptr<OneshotSender>>(sender, kOneshotSender)->promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
    }
    OpPtr oneshot_recv(const ObjectPtr &receiver) {
        auto r = get<std::shared_ptr<OneshotReceiver>>(receiver, kOneshotReceiver);
        std::optional<TaskHandle<Bytes>> handle;
        {
            std::lock_guard lock(r->mutex);
            handle = std::exchange(r->handle, std::nullopt);
        }
        if (!handle) throw BindingError(IoErrorCode::InvalidArgument, "a oneshot receiver can be waited on once");
        return launch([h = *handle](CancellationToken) { return h; }, [](Bytes &&value, Value &out) { out.bytes = std::move(value); });
    }

    ChannelEnds watch_create(Bytes initial) {
        auto pair = watch<Bytes>(std::move(initial));
        return {make_object<WatchSender<Bytes>>(kWatchSender, std::move(pair.sender)), make_object<WatchReceiver<Bytes>>(kWatchReceiver, std::move(pair.receiver))};
    }
    void watch_send(const ObjectPtr &sender, Bytes data) { get<WatchSender<Bytes>>(sender, kWatchSender).send(std::move(data)); }
    ObjectPtr watch_subscribe(const ObjectPtr &sender) {
        return make_object<WatchReceiver<Bytes>>(kWatchReceiver, get<WatchSender<Bytes>>(sender, kWatchSender).subscribe());
    }
    ObjectPtr watch_receiver_clone(const ObjectPtr &receiver) {
        return make_object<WatchReceiver<Bytes>>(kWatchReceiver, get<WatchReceiver<Bytes>>(receiver, kWatchReceiver));
    }
    Bytes watch_borrow(const ObjectPtr &receiver) { return get<WatchReceiver<Bytes>>(receiver, kWatchReceiver).borrow(); }
    Bytes watch_borrow_and_update(const ObjectPtr &receiver) { return get<WatchReceiver<Bytes>>(receiver, kWatchReceiver).borrow_and_update(); }
    bool watch_has_changed(const ObjectPtr &receiver) { return get<WatchReceiver<Bytes>>(receiver, kWatchReceiver).has_changed(); }
    OpPtr watch_changed(const ObjectPtr &receiver) {
        auto &r = get<WatchReceiver<Bytes>>(receiver, kWatchReceiver);
        return launch([&r, receiver](CancellationToken token) { return r.changed(std::move(token)); }, [](Value &) {});
    }

    ChannelEnds broadcast_create(std::uint64_t capacity) {
        auto pair = broadcast<Bytes>(static_cast<std::size_t>(capacity));
        return {make_object<BroadcastSender<Bytes>>(kBroadcastSender, std::move(pair.first)),
                make_object<BroadcastReceiver<Bytes>>(kBroadcastReceiver, std::move(pair.second))};
    }
    std::uint64_t broadcast_send(const ObjectPtr &sender, Bytes data) { return get<BroadcastSender<Bytes>>(sender, kBroadcastSender).send(std::move(data)); }
    ObjectPtr broadcast_subscribe(const ObjectPtr &sender) {
        return make_object<BroadcastReceiver<Bytes>>(kBroadcastReceiver, get<BroadcastSender<Bytes>>(sender, kBroadcastSender).subscribe());
    }
    ObjectPtr broadcast_receiver_clone(const ObjectPtr &receiver) {
        return make_object<BroadcastReceiver<Bytes>>(kBroadcastReceiver, get<BroadcastReceiver<Bytes>>(receiver, kBroadcastReceiver));
    }
    OpPtr broadcast_recv(const ObjectPtr &receiver) {
        auto &r = get<BroadcastReceiver<Bytes>>(receiver, kBroadcastReceiver);
        return launch([&r, receiver](CancellationToken) { return r.recv(); }, [](BroadcastRecv<Bytes> &&message, Value &out) {
            switch (message.kind) {
                case BroadcastRecv<Bytes>::Kind::Value:
                    out.number = 0;
                    if (message.value) out.bytes = std::move(*message.value);
                    break;
                case BroadcastRecv<Bytes>::Kind::Lagged:
                    out.number = 1;
                    out.fields.emplace_back("skipped", std::to_string(message.skipped));
                    break;
                case BroadcastRecv<Bytes>::Kind::Closed: out.number = 2; break;
            }
        });
    }

    // =================================================================================================================
    // Semaphore, locks, notify, barrier
    // =================================================================================================================
    ObjectPtr semaphore_new(std::uint64_t permits) { return make_object<Semaphore>(kSemaphore, Semaphore(static_cast<std::size_t>(permits))); }
    OpPtr semaphore_acquire(const ObjectPtr &semaphore, std::uint64_t count) {
        auto s = get<Semaphore>(semaphore, kSemaphore);
        return launch([s, count](CancellationToken token) { return s.acquire(static_cast<std::size_t>(count), std::move(token)); },
                      [](Semaphore::Permit &&permit, Value &out) { out.object = make_object<Semaphore::Permit>(kPermit, std::move(permit)); });
    }
    ObjectPtr semaphore_try_acquire(const ObjectPtr &semaphore, std::uint64_t count) {
        auto permit = get<Semaphore>(semaphore, kSemaphore).try_acquire(static_cast<std::size_t>(count));
        return permit ? make_object<Semaphore::Permit>(kPermit, std::move(*permit)) : ObjectPtr{};
    }
    void semaphore_add_permits(const ObjectPtr &semaphore, std::uint64_t count) { get<Semaphore>(semaphore, kSemaphore).add_permits(static_cast<std::size_t>(count)); }
    void semaphore_close(const ObjectPtr &semaphore) { get<Semaphore>(semaphore, kSemaphore).close(); }
    std::uint64_t semaphore_available(const ObjectPtr &semaphore) { return get<Semaphore>(semaphore, kSemaphore).available_permits(); }
    bool semaphore_is_closed(const ObjectPtr &semaphore) { return get<Semaphore>(semaphore, kSemaphore).is_closed(); }
    void permit_release(const ObjectPtr &permit) { get<Semaphore::Permit>(permit, kPermit).release(); }
    void permit_forget(const ObjectPtr &permit) { get<Semaphore::Permit>(permit, kPermit).forget(); }
    std::uint64_t permit_count(const ObjectPtr &permit) { return get<Semaphore::Permit>(permit, kPermit).count(); }

    namespace {
        using BytesMutex = AsyncMutex<Bytes>;
        using BytesRwLock = AsyncRwLock<Bytes>;
        using MutexGuard = std::optional<BytesMutex::Guard>;
        using ReadGuard = std::optional<BytesRwLock::ReadGuard>;
        using WriteGuard = std::optional<BytesRwLock::WriteGuard>;
    } // namespace

    ObjectPtr async_mutex_new(Bytes initial) { return make_object<BytesMutex>(kMutex, BytesMutex(std::move(initial))); }
    OpPtr async_mutex_lock(const ObjectPtr &mutex) {
        auto m = get<BytesMutex>(mutex, kMutex);
        return launch([m](CancellationToken token) { return m.lock(std::move(token)); },
                      [](BytesMutex::Guard &&guard, Value &out) { out.object = make_object<MutexGuard>(kMutexGuard, MutexGuard(std::move(guard))); });
    }
    ObjectPtr async_mutex_try_lock(const ObjectPtr &mutex) {
        auto guard = get<BytesMutex>(mutex, kMutex).try_lock();
        return guard ? make_object<MutexGuard>(kMutexGuard, MutexGuard(std::move(*guard))) : ObjectPtr{};
    }
    ObjectPtr async_rwlock_new(Bytes initial) { return make_object<BytesRwLock>(kRwLock, BytesRwLock(std::move(initial))); }
    OpPtr async_rwlock_read(const ObjectPtr &lock) {
        auto l = get<BytesRwLock>(lock, kRwLock);
        return launch([l](CancellationToken token) { return l.read(std::move(token)); },
                      [](BytesRwLock::ReadGuard &&guard, Value &out) { out.object = make_object<ReadGuard>(kReadGuard, ReadGuard(std::move(guard))); });
    }
    OpPtr async_rwlock_write(const ObjectPtr &lock) {
        auto l = get<BytesRwLock>(lock, kRwLock);
        return launch([l](CancellationToken token) { return l.write(std::move(token)); },
                      [](BytesRwLock::WriteGuard &&guard, Value &out) { out.object = make_object<WriteGuard>(kWriteGuard, WriteGuard(std::move(guard))); });
    }
    namespace {
        [[noreturn]] void guard_released() { throw BindingError(IoErrorCode::Closed, "the guard was released"); }
    } // namespace
    Bytes guard_get(const ObjectPtr &guard) {
        if (is_kind(guard, kSyncGuard)) {
            auto &g = get<SyncGuardState>(guard, kSyncGuard);
            if (!g.guard) guard_released();
            return **g.guard;
        }
        if (is_kind(guard, kMutexGuard)) {
            auto &g = get<MutexGuard>(guard, kMutexGuard);
            if (!g) guard_released();
            return **g;
        }
        if (is_kind(guard, kReadGuard)) {
            auto &g = get<ReadGuard>(guard, kReadGuard);
            if (!g) guard_released();
            return **g;
        }
        auto &g = get<WriteGuard>(guard, kWriteGuard);
        if (!g) guard_released();
        return **g;
    }
    void guard_set(const ObjectPtr &guard, Bytes data) {
        if (is_kind(guard, kSyncGuard)) {
            auto &g = get<SyncGuardState>(guard, kSyncGuard);
            if (!g.guard) guard_released();
            **g.guard = std::move(data);
            return;
        }
        if (is_kind(guard, kMutexGuard)) {
            auto &g = get<MutexGuard>(guard, kMutexGuard);
            if (!g) guard_released();
            **g = std::move(data);
            return;
        }
        auto &g = get<WriteGuard>(guard, kWriteGuard);
        if (!g) guard_released();
        **g = std::move(data);
    }
    void guard_release(const ObjectPtr &guard) {
        if (is_kind(guard, kSyncGuard)) get<SyncGuardState>(guard, kSyncGuard).guard.reset();
        else if (is_kind(guard, kMutexGuard)) get<MutexGuard>(guard, kMutexGuard).reset();
        else if (is_kind(guard, kReadGuard)) get<ReadGuard>(guard, kReadGuard).reset();
        else get<WriteGuard>(guard, kWriteGuard).reset();
    }

    ObjectPtr notify_new() { return make_object<Notify>(kNotify, Notify{}); }
    OpPtr notify_notified(const ObjectPtr &notify) {
        auto n = get<Notify>(notify, kNotify);
        return launch([n](CancellationToken token) { return n.notified(std::move(token)); }, [](Value &) {});
    }
    void notify_one(const ObjectPtr &notify) { get<Notify>(notify, kNotify).notify_one(); }
    void notify_waiters(const ObjectPtr &notify) { get<Notify>(notify, kNotify).notify_waiters(); }
    ObjectPtr barrier_new(std::uint64_t parties) { return make_object<Barrier>(kBarrier, Barrier(static_cast<std::size_t>(parties))); }
    OpPtr barrier_wait(const ObjectPtr &barrier) {
        auto b = get<Barrier>(barrier, kBarrier);
        return launch([b](CancellationToken) { return b.wait(); }, [](bool leader, Value &out) { out.number = leader ? 1 : 0; });
    }

    // =================================================================================================================
    // Tasks
    // =================================================================================================================
    OpPtr task_spawn(TaskFn fn, Lane lane) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a task function is required");
        return launch(
            [&](CancellationToken token) {
                auto body = [fn = std::move(fn), token]() { return run_task(fn, token); };
                if (lane == Lane::Blocking) return IoHandler::submit(std::move(body));
                return Scheduler::spawn(std::move(body), lane == Lane::Heavy ? TaskWeight::Heavy : TaskWeight::Light);
            },
            fill_output);
    }
    OpPtr task_spawn_on(const ObjectPtr &pool, TaskFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a task function is required");
        auto &p = *get<std::unique_ptr<ThreadPool>>(pool, kPool);
        return launch([&, pool](CancellationToken token) { return p.spawn([fn = std::move(fn), token]() { return run_task(fn, token); }); }, fill_output);
    }

#if !defined(STURDY_PLATFORM_WEB) // libc++ on Emscripten cannot instantiate JoinSet (a deque of std::expected<T, exception_ptr>)
    namespace {
        /// JoinSet stores `std::expected<T, exception_ptr>` in a deque; libc++ (Emscripten) cannot instantiate that for a comparison-less
        /// aggregate, so the output travels behind a pointer.
        using JoinItem = std::shared_ptr<TaskOutput>;
        struct JoinSetState {
            JoinSet<JoinItem> set;
            CancellationSource cancel;
        };
    } // namespace

    ObjectPtr join_set_new() { return make_object<std::shared_ptr<JoinSetState>>(kJoinSet, std::make_shared<JoinSetState>()); }
    void join_set_spawn(const ObjectPtr &set, TaskFn fn, Lane lane) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a task function is required");
        auto state = get<std::shared_ptr<JoinSetState>>(set, kJoinSet);
        auto body = [fn = std::move(fn), token = state->cancel.token()]() { return std::make_shared<TaskOutput>(run_task(fn, token)); };
        if (lane == Lane::Blocking) state->set.adopt(IoHandler::submit(std::move(body)));
        else state->set.adopt(Scheduler::spawn(std::move(body), lane == Lane::Heavy ? TaskWeight::Heavy : TaskWeight::Light));
    }
    OpPtr join_set_join_next(const ObjectPtr &set) {
        auto state = get<std::shared_ptr<JoinSetState>>(set, kJoinSet);
        return launch([state](CancellationToken) { return state->set.join_next(); }, [](std::optional<JoinResult<JoinItem>> &&outcome, Value &out) {
            if (!outcome) {
                out.fields.emplace_back("outcome", "empty");
                return;
            }
            if (!*outcome) {
                std::string message = "task failed";
                try {
                    std::rethrow_exception(outcome->error());
                } catch (const std::exception &error) {
                    message = error.what();
                } catch (...) {
                }
                out.fields.emplace_back("outcome", "failed");
                out.fields.emplace_back("error", std::move(message));
                return;
            }
            TaskOutput &task = ***outcome;
            if (!task.error.empty()) {
                out.fields.emplace_back("outcome", "failed");
                out.fields.emplace_back("error", task.error);
                return;
            }
            out.fields.emplace_back("outcome", "ok");
            out.number = task.number;
            out.bytes = std::move(task.bytes);
            out.text = std::move(task.text);
        });
    }
    std::uint64_t join_set_len(const ObjectPtr &set) { return get<std::shared_ptr<JoinSetState>>(set, kJoinSet)->set.len(); }
    void join_set_abort_all(const ObjectPtr &set) { (void)get<std::shared_ptr<JoinSetState>>(set, kJoinSet)->cancel.cancel(); }
#else
    ObjectPtr join_set_new() { throw BindingError(IoErrorCode::Unsupported, "join sets are not available on Web"); }
    void join_set_spawn(const ObjectPtr &, TaskFn, Lane) { throw BindingError(IoErrorCode::Unsupported, "join sets are not available on Web"); }
    OpPtr join_set_join_next(const ObjectPtr &) { throw BindingError(IoErrorCode::Unsupported, "join sets are not available on Web"); }
    std::uint64_t join_set_len(const ObjectPtr &) { throw BindingError(IoErrorCode::Unsupported, "join sets are not available on Web"); }
    void join_set_abort_all(const ObjectPtr &) { throw BindingError(IoErrorCode::Unsupported, "join sets are not available on Web"); }
#endif

    ObjectPtr local_set_new() { return make_object<LocalSet>(kLocalSet, LocalSet{}); }
    OpPtr local_set_spawn(const ObjectPtr &set, TaskFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a task function is required");
        auto &s = get<LocalSet>(set, kLocalSet);
        return launch([&, set](CancellationToken token) { return s.spawn_local([fn = std::move(fn), token]() { return run_task(fn, token); }); }, fill_output);
    }
    std::uint64_t local_set_run_pending(const ObjectPtr &set) { return get<LocalSet>(set, kLocalSet).run_pending(); }
    void local_set_block_on(const ObjectPtr &set, const OpPtr &op) {
        auto &s = get<LocalSet>(set, kLocalSet);
        Promise<void> done;
        op->on_complete([done]() mutable { (void)done.set_value(); });
        s.block_on(done.handle());
    }

    ObjectPtr dedicated_thread_new(const std::string &name) {
        return make_object<std::shared_ptr<DedicatedThread>>(kDedicated, std::make_shared<DedicatedThread>(name.empty() ? "DedicatedThread" : name));
    }
    OpPtr dedicated_thread_run(const ObjectPtr &thread, TaskFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a task function is required");
        auto t = get<std::shared_ptr<DedicatedThread>>(thread, kDedicated);
        return launch([t, fn = std::move(fn)](CancellationToken token) { return t->run([fn, token]() { return run_task(fn, token); }); }, fill_output);
    }
    bool dedicated_thread_pin_to_core(const ObjectPtr &thread, std::uint32_t core_index) {
        return get<std::shared_ptr<DedicatedThread>>(thread, kDedicated)->pin_to_core(core_index);
    }
    bool dedicated_thread_pin_to_fastest_core(const ObjectPtr &thread) { return get<std::shared_ptr<DedicatedThread>>(thread, kDedicated)->pin_to_fastest_core(); }

    ObjectPtr task_local_new() { return make_object<std::shared_ptr<TaskLocal<Bytes>>>(kTaskLocal, std::make_shared<TaskLocal<Bytes>>()); }
    void task_local_scope(const ObjectPtr &local, Bytes value, const std::function<void()> &body) {
        get<std::shared_ptr<TaskLocal<Bytes>>>(local, kTaskLocal)->scope(std::move(value), [&] { body(); });
    }
    bool task_local_get(const ObjectPtr &local, Bytes *out) {
        auto value = get<std::shared_ptr<TaskLocal<Bytes>>>(local, kTaskLocal)->get();
        if (!value) return false;
        if (out) *out = std::move(*value);
        return true;
    }

    void main_thread_run(std::function<void()> fn) { SFT::Async::run_on_main_thread(std::move(fn)); }
    void main_thread_pump() { SFT::Async::pump_main_thread(); }
    void main_thread_set_wake_hook(std::function<void()> hook) { SFT::Async::set_main_thread_wake_hook(std::move(hook)); }

    // =================================================================================================================
    // Runtime, topology, timers, signals
    // =================================================================================================================
    void scheduler_initialize(std::uint32_t workers) { Scheduler::initialize(workers); }
    void scheduler_shutdown() { Scheduler::shutdown(); }
    bool scheduler_is_running() { return Scheduler::is_running(); }
    std::uint32_t scheduler_worker_count() { return Scheduler::worker_count(); }
    bool scheduler_on_worker_thread() { return Scheduler::is_worker_thread(); }
    bool io_lane_configure(std::uint32_t workers, std::uint32_t max_pending) {
        IoHandlerConfig config;
        config.worker_count = workers;
        if (max_pending != 0) config.max_pending_operations = max_pending;
        return IoHandler::configure(config);
    }
    Fields io_lane_stats() {
        return {{"backend", IoHandler::backend() == IoBackend::Synchronous ? "synchronous" : "thread_pool"},
                {"workers", std::to_string(IoHandler::worker_count())},
                {"pending", std::to_string(IoHandler::pending_count())},
                {"capacity", std::to_string(IoHandler::pending_capacity())}};
    }
    bool network_reactor_configure(std::uint32_t threads) { return SFT::Async::configure_network_reactor(threads); }
    std::vector<std::uint32_t> ranked_logical_cores() { return SFT::Async::ranked_logical_cores(); }
    std::vector<std::uint32_t> ranked_physical_cores() { return SFT::Async::ranked_physical_cores(); }
    bool pin_current_thread_to_core(std::uint32_t core_index) { return SFT::Async::pin_current_thread_to_core(core_index); }

    OpPtr sleep_until_unix_ms(std::int64_t epoch_ms) {
        const auto wall = std::chrono::system_clock::time_point(std::chrono::milliseconds(epoch_ms));
        const auto remaining = wall - std::chrono::system_clock::now();
        const auto deadline = SFT::Async::Clock::now() + std::chrono::duration_cast<SFT::Async::Clock::duration>(remaining);
        return launch([deadline](CancellationToken token) { return SFT::Async::sleep_until(deadline, std::move(token)); }, [](Value &) {});
    }

    namespace {
        struct IntervalState {
            std::mutex mutex;
            Interval interval;
            explicit IntervalState(Clock::duration period) : interval(period) {}
        };
    } // namespace
    ObjectPtr interval_new(std::int64_t period_ms) {
        if (period_ms <= 0) throw BindingError(IoErrorCode::InvalidArgument, "an interval needs a positive period");
        return make_object<std::shared_ptr<IntervalState>>(kInterval, std::make_shared<IntervalState>(ms(period_ms)));
    }
    OpPtr interval_tick(const ObjectPtr &interval) {
        auto state = get<std::shared_ptr<IntervalState>>(interval, kInterval);
        return launch(
            [state](CancellationToken token) {
                std::lock_guard lock(state->mutex);
                return state->interval.tick(std::move(token));
            },
            [](Value &) {});
    }
    OpPtr signal_wait(const std::vector<std::int32_t> &signals) {
        return launch(
            [&](CancellationToken token) {
                std::vector<int> numbers(signals.begin(), signals.end());
                return SFT::Async::wait_for_signal(std::span<const int>(numbers), std::move(token));
            },
            [](int number, Value &out) { out.number = number; });
    }
    OpPtr ctrl_c() { return launch([](CancellationToken token) { return SFT::Async::ctrl_c(std::move(token)); }, [](int number, Value &out) { out.number = number; }); }

} // namespace SFT::Async::Binding
