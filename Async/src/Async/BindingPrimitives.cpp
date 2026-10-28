#include <Async/BindingInternal.hpp>

#include <Async/Arc.hpp>
#include <Async/IoHandler.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Scheduler.hpp>
#include <Async/SpscRingBuffer.hpp>

#include <array>
#include <cstring>

namespace SFT::Async::Binding {

    using namespace Detail;

    namespace {
        constexpr const char *kSyncMutex = "sync_mutex";
        constexpr const char *kRing = "spsc_ring";
        constexpr const char *kArc = "arc";
        constexpr const char *kPromise = "promise";
        constexpr const char *kSender = "exec_sender";
        constexpr const char *kCancelToken = "cancel_token";
    } // namespace

    // =================================================================================================================
    // Blocking mutex
    // =================================================================================================================
    ObjectPtr sync_mutex_new(Bytes initial, const std::string &debug_name) {
        auto mutex = std::make_shared<Async::Mutex<Bytes>>(std::move(initial));
        if (!debug_name.empty()) mutex->set_debug_name(debug_name.c_str());
        return make_object<std::shared_ptr<Async::Mutex<Bytes>>>(kSyncMutex, std::move(mutex));
    }
    ObjectPtr sync_mutex_lock(const ObjectPtr &mutex) {
        auto shared = get<std::shared_ptr<Async::Mutex<Bytes>>>(mutex, kSyncMutex);
        SyncGuardState state{shared, std::nullopt};
        state.guard.emplace(shared->lock());
        return make_object<SyncGuardState>(kSyncGuard, std::move(state));
    }
    ObjectPtr sync_mutex_try_lock(const ObjectPtr &mutex) {
        auto shared = get<std::shared_ptr<Async::Mutex<Bytes>>>(mutex, kSyncMutex);
        auto guard = shared->try_lock();
        if (!guard) return {};
        SyncGuardState state{shared, std::nullopt};
        state.guard.emplace(std::move(*guard));
        return make_object<SyncGuardState>(kSyncGuard, std::move(state));
    }

    // =================================================================================================================
    // SPSC ring
    // =================================================================================================================
    namespace {
        struct RingBase {
            virtual ~RingBase() = default;
            virtual RingStatus push(const Bytes &item) = 0;
            virtual RingStatus pop(Bytes *out) = 0;
            virtual std::uint64_t size() const = 0;
            virtual std::uint64_t capacity() const = 0;
            std::uint64_t max_item = 0;
        };

        template <std::size_t N>
        struct Ring final : RingBase {
            struct Slot {
                std::uint32_t size;
                std::array<std::uint8_t, N> data;
            };
            explicit Ring(std::uint64_t capacity) : ring(static_cast<usize>(capacity)) { max_item = N; }
            RingStatus push(const Bytes &item) override {
                if (item.size() > N) return RingStatus::TooLarge;
                Slot slot;
                slot.size = static_cast<std::uint32_t>(item.size());
                if (!item.empty()) std::memcpy(slot.data.data(), item.data(), item.size());
                return ring.try_push(slot) ? RingStatus::Ok : RingStatus::Full;
            }
            RingStatus pop(Bytes *out) override {
                Slot slot;
                if (!ring.try_pop(slot)) return RingStatus::Empty;
                if (out) out->assign(slot.data.begin(), slot.data.begin() + slot.size);
                return RingStatus::Ok;
            }
            std::uint64_t size() const override { return ring.size(); }
            std::uint64_t capacity() const override { return ring.capacity(); }
            SpscRingBuffer<Slot> ring;
        };

        RingBase &ring_of(const ObjectPtr &object) { return *get<std::shared_ptr<RingBase>>(object, kRing); }
    } // namespace

    ObjectPtr ring_new(std::uint64_t capacity, std::uint64_t max_item_bytes) {
        if (capacity == 0 || capacity > (1ull << 24)) throw BindingError(IoErrorCode::InvalidArgument, "ring capacity must be between 1 and 16 Mi items");
        std::shared_ptr<RingBase> ring;
        if (max_item_bytes <= 32) ring = std::make_shared<Ring<32>>(capacity);
        else if (max_item_bytes <= 128) ring = std::make_shared<Ring<128>>(capacity);
        else if (max_item_bytes <= 512) ring = std::make_shared<Ring<512>>(capacity);
        else if (max_item_bytes <= 2048) ring = std::make_shared<Ring<2048>>(capacity);
        else if (max_item_bytes <= 8192) ring = std::make_shared<Ring<8192>>(capacity);
        else if (max_item_bytes <= 65536) ring = std::make_shared<Ring<65536>>(capacity);
        else throw BindingError(IoErrorCode::InvalidArgument, "ring items are limited to 64 KiB");
        return make_object<std::shared_ptr<RingBase>>(kRing, std::move(ring));
    }
    RingStatus ring_try_push(const ObjectPtr &ring, const Bytes &item) { return ring_of(ring).push(item); }
    RingStatus ring_try_pop(const ObjectPtr &ring, Bytes *out) { return ring_of(ring).pop(out); }
    std::vector<Bytes> ring_drain(const ObjectPtr &ring, std::uint64_t max_items) {
        std::vector<Bytes> items;
        RingBase &r = ring_of(ring);
        Bytes item;
        while ((max_items == 0 || items.size() < max_items) && r.pop(&item) == RingStatus::Ok) items.push_back(std::move(item));
        return items;
    }
    std::uint64_t ring_size(const ObjectPtr &ring) { return ring_of(ring).size(); }
    std::uint64_t ring_capacity(const ObjectPtr &ring) { return ring_of(ring).capacity(); }
    std::uint64_t ring_max_item_bytes(const ObjectPtr &ring) { return ring_of(ring).max_item; }

    // =================================================================================================================
    // Arc
    // =================================================================================================================
    namespace {
        struct ArcBlob {
            Bytes bytes;
            void *pointer = nullptr;
            std::function<void()> destroy;
            ArcBlob() = default;
            ArcBlob(const ArcBlob &) = delete;
            ArcBlob &operator=(const ArcBlob &) = delete;
            ~ArcBlob() {
                if (destroy) destroy();
            }
        };
        Async::Arc<ArcBlob> &arc_of(const ObjectPtr &object) { return get<Async::Arc<ArcBlob>>(object, kArc); }
    } // namespace

    ObjectPtr arc_new(Bytes data) {
        auto arc = Async::make_arc<ArcBlob>();
        arc->bytes = std::move(data);
        return make_object<Async::Arc<ArcBlob>>(kArc, std::move(arc));
    }
    ObjectPtr arc_new_foreign(void *pointer, std::function<void()> destroy) {
        auto arc = Async::make_arc<ArcBlob>();
        arc->pointer = pointer;
        arc->destroy = std::move(destroy);
        return make_object<Async::Arc<ArcBlob>>(kArc, std::move(arc));
    }
    ObjectPtr arc_clone(const ObjectPtr &arc) { return make_object<Async::Arc<ArcBlob>>(kArc, arc_of(arc)); }
    std::uint64_t arc_use_count(const ObjectPtr &arc) { return static_cast<std::uint64_t>(arc_of(arc).use_count()); }
    bool arc_ptr_eq(const ObjectPtr &a, const ObjectPtr &b) { return arc_of(a) == arc_of(b); }
    Bytes arc_bytes(const ObjectPtr &arc) { return arc_of(arc)->bytes; }
    void *arc_pointer(const ObjectPtr &arc) { return arc_of(arc)->pointer; }

    // =================================================================================================================
    // Promise
    // =================================================================================================================
    namespace {
        struct PromiseState {
            OpPtr op;
            ~PromiseState() {
                if (op && !op->done()) op->fail(IoErrorCode::Closed, "the promise was dropped before it was fulfilled");
            }
        };
        PromiseState &promise_of(const ObjectPtr &object) { return *get<std::shared_ptr<PromiseState>>(object, kPromise); }
    } // namespace

    PromiseEnds promise_new() {
        auto state = std::make_shared<PromiseState>();
        state->op = std::make_shared<Op>();
        PromiseEnds ends;
        ends.future = state->op;
        ends.promise = make_object<std::shared_ptr<PromiseState>>(kPromise, std::move(state));
        return ends;
    }
    bool promise_set_value(const ObjectPtr &promise, std::int64_t number, Bytes bytes, std::string text) {
        auto &op = *promise_of(promise).op;
        if (op.done()) return false;
        Value value;
        value.status = OpStatus::Ok;
        value.number = number;
        value.bytes = std::move(bytes);
        value.text = std::move(text);
        op.complete(std::move(value));
        return true;
    }
    bool promise_set_error(const ObjectPtr &promise, IoErrorCode code, std::string message) {
        auto &op = *promise_of(promise).op;
        if (op.done()) return false;
        op.fail(code, std::move(message));
        return true;
    }
    bool promise_cancel(const ObjectPtr &promise) {
        auto &op = *promise_of(promise).op;
        if (op.done()) return false;
        op.mark_cancelled();
        return true;
    }
    bool promise_is_completed(const ObjectPtr &promise) { return promise_of(promise).op->done(); }
    OpPtr op_ready(std::int64_t number, Bytes bytes, std::string text) {
        auto op = std::make_shared<Op>();
        Value value;
        value.status = OpStatus::Ok;
        value.number = number;
        value.bytes = std::move(bytes);
        value.text = std::move(text);
        op->complete(std::move(value));
        return op;
    }
    OpPtr op_failed(IoErrorCode code, std::string message) { return failed_op(code, std::move(message)); }

    // =================================================================================================================
    // Execution senders
    // =================================================================================================================
    namespace {
        using StartFn = std::function<TaskHandle<TaskOutput>(CancellationToken)>;
        struct SenderState {
            StartFn start;
        };

        TaskOutput guarded_stage(const std::function<TaskOutput()> &stage) {
            try {
                return stage();
            } catch (const std::exception &error) {
                TaskOutput out;
                out.error = error.what();
                return out;
            } catch (...) {
                TaskOutput out;
                out.error = "the stage failed";
                return out;
            }
        }

        TaskHandle<TaskOutput> spawn_stage(Lane lane, std::function<TaskOutput()> body) {
            if (lane == Lane::Blocking) return IoHandler::submit(std::move(body));
            return Scheduler::spawn(std::move(body), lane == Lane::Heavy ? TaskWeight::Heavy : TaskWeight::Light);
        }

        SenderState &sender_of(const ObjectPtr &object) { return *get<std::shared_ptr<SenderState>>(object, kSender); }
    } // namespace

    ObjectPtr exec_just(TaskFn fn, Lane lane) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a stage function is required");
        auto state = std::make_shared<SenderState>();
        state->start = [fn = std::move(fn), lane](CancellationToken token) {
            return spawn_stage(lane, [fn, token] { return guarded_stage([&] { return fn(make_object<CancellationToken>(kCancelToken, token)); }); });
        };
        return make_object<std::shared_ptr<SenderState>>(kSender, std::move(state));
    }

    ObjectPtr exec_then(const ObjectPtr &sender, ThenFn fn, Lane lane) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a stage function is required");
        auto parent = get<std::shared_ptr<SenderState>>(sender, kSender);
        auto state = std::make_shared<SenderState>();
        state->start = [parent, fn = std::move(fn), lane](CancellationToken token) {
            auto upstream = parent->start(token);
            Promise<TaskOutput> next;
            auto result = next.handle();
            upstream.on_done([upstream, next, fn, lane, token]() mutable {
                TaskOutput input;
                try {
                    input = upstream.wait();
                } catch (...) {
                    (void)next.set_exception(std::current_exception());
                    return;
                }
                if (!input.error.empty()) { // an upstream failure skips this stage
                    (void)next.set_value(std::move(input));
                    return;
                }
                auto stage = spawn_stage(lane, [fn, token, input = std::move(input)] {
                    return guarded_stage([&] { return fn(make_object<CancellationToken>(kCancelToken, token), input); });
                });
                stage.on_done([stage, next]() mutable {
                    try {
                        (void)next.set_value(stage.wait());
                    } catch (...) {
                        (void)next.set_exception(std::current_exception());
                    }
                });
            });
            return result;
        };
        return make_object<std::shared_ptr<SenderState>>(kSender, std::move(state));
    }

    OpPtr exec_submit(const ObjectPtr &sender) {
        auto state = get<std::shared_ptr<SenderState>>(sender, kSender);
        return launch([state](CancellationToken token) { return state->start(std::move(token)); }, [](TaskOutput &&out, Value &value) {
            if (!out.error.empty()) throw std::runtime_error(out.error);
            value.number = out.number;
            value.bytes = std::move(out.bytes);
            value.text = std::move(out.text);
        });
    }

    TaskOutput exec_sync_wait(const ObjectPtr &sender) {
        auto op = exec_submit(sender);
        op->wait(-1);
        const Value &value = op->result();
        if (value.status != OpStatus::Ok) throw BindingError(value.code, value.error.empty() ? "the chain did not finish" : value.error);
        TaskOutput out;
        out.number = value.number;
        out.bytes = value.bytes;
        out.text = value.text;
        return out;
    }

} // namespace SFT::Async::Binding
