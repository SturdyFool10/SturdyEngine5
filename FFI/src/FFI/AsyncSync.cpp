/// C ABI for cancellation, channels and synchronisation primitives, tasks, the runtime, streams and the extra data-parallel operations.
/// A thin translation of the matching parts of `Async/Binding.hpp`.

#include <FFI/AsyncNetSupport.hpp>

#include <FFI/Sturdy.h>

using namespace SFT::Ffi::AsyncSupport;

namespace {

    /// Runs `body` with a temporary handle for `object` (valid only during the call).
    template <typename Body>
    void with_temporary_handle(const B::ObjectPtr &object, Body &&body) {
        const SturdyAsyncObject handle = publish_object(object);
        struct Release {
            SturdyAsyncObject handle;
            ~Release() { (void)sturdy_object_release(handle); }
        } release{handle};
        body(handle);
    }

    B::TaskFn adapt_task(SturdyTaskWorkFn work, void *user_data) {
        return [work, user_data](B::ObjectPtr cancel) {
            B::TaskOutput value;
            SturdyTaskOutput output{&value};
            with_temporary_handle(cancel, [&](SturdyAsyncObject token) { work(user_data, token, &output); });
            return value;
        };
    }

    std::function<void()> adapt_void(SturdyTaskFn fn, void *user_data) {
        return [fn, user_data] { fn(user_data); };
    }

    /// Runs a callback that fills a `SturdyBytesSink` and returns what it wrote.
    template <typename Fill>
    B::Bytes collect_sink(Fill &&fill) {
        B::Bytes bytes;
        SturdyBytesSink sink{&bytes};
        fill(&sink);
        return bytes;
    }

    SturdyResult publish_two(B::ChannelEnds ends, SturdyAsyncObject *a, SturdyAsyncObject *b) {
        *a = publish_object(std::move(ends.sender));
        *b = publish_object(std::move(ends.receiver));
        return STURDY_OK;
    }

    template <typename Make>
    SturdyResult make_stream_from(SturdyAsyncObject *out_stream, Make &&make) {
        STURDY_REQUIRE(out_stream, "output pointer must not be null");
        *out_stream = publish_object(make());
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_task_output_set(SturdyTaskOutput *output, int64_t number, const uint8_t *bytes, size_t size, const char *text_in, const char *error) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(output, "output must not be null");
        STURDY_REQUIRE(output->internal, "output was not created by the engine");
        auto &value = *static_cast<B::TaskOutput *>(output->internal);
        value.number = number;
        value.bytes = bytes_of(bytes, size);
        value.text = text(text_in);
        value.error = text(error);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_bytes_sink_set(SturdyBytesSink *sink, const uint8_t *data, size_t size) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(sink, "sink must not be null");
        STURDY_REQUIRE(sink->internal, "sink was not created by the engine");
        *static_cast<B::Bytes *>(sink->internal) = bytes_of(data, size);
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Cancellation
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_cancel_source_create(SturdyAsyncObject *out_source) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_source, "output pointer must not be null");
        *out_source = publish_object(B::cancel_source_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cancel_source_cancel(SturdyAsyncObject source, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(source, object);
        const bool first = B::cancel_source_cancel(object);
        if (out_first != nullptr) *out_first = first ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cancel_source_is_cancelled(SturdyAsyncObject source, SturdyBool *out_cancelled) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_cancelled, "output pointer must not be null");
        STURDY_WITH_OBJECT(source, object);
        *out_cancelled = B::cancel_source_is_cancelled(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cancel_source_token(SturdyAsyncObject source, SturdyAsyncObject *out_token) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_token, "output pointer must not be null");
        STURDY_WITH_OBJECT(source, object);
        *out_token = publish_object(B::cancel_source_token(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cancel_token_is_cancelled(SturdyAsyncObject token, SturdyBool *out_cancelled) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_cancelled, "output pointer must not be null");
        STURDY_WITH_OBJECT(token, object);
        *out_cancelled = B::cancel_token_is_cancelled(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cancel_token_wait(SturdyAsyncObject token, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(token, object);
        return publish_op(B::cancel_token_wait(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_link_cancel(SturdyAsyncOp op, SturdyAsyncObject token) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        STURDY_WITH_OBJECT(token, object);
        B::op_link_cancel(resolved, object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_with_timeout(SturdyAsyncOp op, int64_t timeout_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        return publish_op(B::op_with_timeout(resolved, timeout_ms), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Channels
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_channel_create(uint64_t capacity, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointers must not be null");
        STURDY_REQUIRE(out_receiver, "output pointers must not be null");
        return publish_two(B::channel_create(capacity), out_sender, out_receiver);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_sender_clone(SturdyAsyncObject sender, SturdyAsyncObject *out_sender) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointer must not be null");
        STURDY_WITH_OBJECT(sender, object);
        *out_sender = publish_object(B::channel_sender_clone(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_receiver, "output pointer must not be null");
        STURDY_WITH_OBJECT(receiver, object);
        *out_receiver = publish_object(B::channel_receiver_clone(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(sender, object);
        return publish_op(B::channel_send(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_try_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyTryStatus *out_status) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_status, "output pointer must not be null");
        STURDY_WITH_OBJECT(sender, object);
        *out_status = static_cast<SturdyTryStatus>(B::channel_try_send(object, bytes_of(data, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(receiver, object);
        return publish_op(B::channel_recv(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_try_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op, SturdyTryStatus *out_status) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_REQUIRE(out_status, "output pointer must not be null");
        STURDY_WITH_OBJECT(receiver, object);
        *out_op = SturdyAsyncOp{0};
        B::Bytes message;
        const B::TryStatus status = B::channel_try_recv(object, &message);
        *out_status = static_cast<SturdyTryStatus>(status);
        if (status != B::TryStatus::Ok) return STURDY_OK;
        return publish_bytes(std::move(message), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_channel_close(SturdyAsyncObject end) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(end, object);
        B::channel_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_oneshot_create(SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointers must not be null");
        STURDY_REQUIRE(out_receiver, "output pointers must not be null");
        return publish_two(B::oneshot_create(), out_sender, out_receiver);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_oneshot_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(sender, object);
        const bool first = B::oneshot_send(object, bytes_of(data, size));
        if (out_first != nullptr) *out_first = first ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_oneshot_fail(SturdyAsyncObject sender, const char *message, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(sender, object);
        const bool first = B::oneshot_fail(object, text(message));
        if (out_first != nullptr) *out_first = first ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_oneshot_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(receiver, object);
        return publish_op(B::oneshot_recv(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointers must not be null");
        STURDY_REQUIRE(out_receiver, "output pointers must not be null");
        return publish_two(B::watch_create(bytes_of(initial, size)), out_sender, out_receiver);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_send(SturdyAsyncObject sender, const uint8_t *data, size_t size) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(sender, object);
        B::watch_send(object, bytes_of(data, size));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_subscribe(SturdyAsyncObject sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_receiver, "output pointer must not be null");
        STURDY_WITH_OBJECT(sender, object);
        *out_receiver = publish_object(B::watch_subscribe(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_receiver, "output pointer must not be null");
        STURDY_WITH_OBJECT(receiver, object);
        *out_receiver = publish_object(B::watch_receiver_clone(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_borrow(SturdyAsyncObject receiver, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(receiver, object);
        return copy_bytes_out(B::watch_borrow(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_borrow_and_update(SturdyAsyncObject receiver, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(receiver, object);
        return copy_bytes_out(B::watch_borrow_and_update(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_has_changed(SturdyAsyncObject receiver, SturdyBool *out_changed) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_changed, "output pointer must not be null");
        STURDY_WITH_OBJECT(receiver, object);
        *out_changed = B::watch_has_changed(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_value_changed(SturdyAsyncObject receiver, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(receiver, object);
        return publish_op(B::watch_changed(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_broadcast_create(uint64_t capacity, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointers must not be null");
        STURDY_REQUIRE(out_receiver, "output pointers must not be null");
        return publish_two(B::broadcast_create(capacity), out_sender, out_receiver);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_broadcast_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, uint64_t *out_receivers) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(sender, object);
        const auto receivers = B::broadcast_send(object, bytes_of(data, size));
        if (out_receivers != nullptr) *out_receivers = receivers;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_broadcast_subscribe(SturdyAsyncObject sender, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_receiver, "output pointer must not be null");
        STURDY_WITH_OBJECT(sender, object);
        *out_receiver = publish_object(B::broadcast_subscribe(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_broadcast_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_receiver, "output pointer must not be null");
        STURDY_WITH_OBJECT(receiver, object);
        *out_receiver = publish_object(B::broadcast_receiver_clone(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_broadcast_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(receiver, object);
        return publish_op(B::broadcast_recv(object), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Semaphore, locks, notify, barrier
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_semaphore_create(uint64_t permits, SturdyAsyncObject *out_semaphore) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_semaphore, "output pointer must not be null");
        *out_semaphore = publish_object(B::semaphore_new(permits));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_acquire(SturdyAsyncObject semaphore, uint64_t count, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(semaphore, object);
        return publish_op(B::semaphore_acquire(object, count), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_try_acquire(SturdyAsyncObject semaphore, uint64_t count, SturdyAsyncObject *out_permit) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_permit, "output pointer must not be null");
        STURDY_WITH_OBJECT(semaphore, object);
        *out_permit = publish_object(B::semaphore_try_acquire(object, count));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_add_permits(SturdyAsyncObject semaphore, uint64_t count) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(semaphore, object);
        B::semaphore_add_permits(object, count);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_close(SturdyAsyncObject semaphore) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(semaphore, object);
        B::semaphore_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_available(SturdyAsyncObject semaphore, uint64_t *out_permits) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_permits, "output pointer must not be null");
        STURDY_WITH_OBJECT(semaphore, object);
        *out_permits = B::semaphore_available(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_semaphore_is_closed(SturdyAsyncObject semaphore, SturdyBool *out_closed) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_closed, "output pointer must not be null");
        STURDY_WITH_OBJECT(semaphore, object);
        *out_closed = B::semaphore_is_closed(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_permit_release(SturdyAsyncObject permit) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(permit, object);
        B::permit_release(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_permit_forget(SturdyAsyncObject permit) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(permit, object);
        B::permit_forget(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_permit_count(SturdyAsyncObject permit, uint64_t *out_count) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_count, "output pointer must not be null");
        STURDY_WITH_OBJECT(permit, object);
        *out_count = B::permit_count(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_mutex_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_mutex) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_mutex, "output pointer must not be null");
        *out_mutex = publish_object(B::async_mutex_new(bytes_of(initial, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_mutex_lock(SturdyAsyncObject mutex, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(mutex, object);
        return publish_op(B::async_mutex_lock(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_mutex_try_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_guard, "output pointer must not be null");
        STURDY_WITH_OBJECT(mutex, object);
        *out_guard = publish_object(B::async_mutex_try_lock(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_lock) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_lock, "output pointer must not be null");
        *out_lock = publish_object(B::async_rwlock_new(bytes_of(initial, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_read(SturdyAsyncObject lock, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(lock, object);
        return publish_op(B::async_rwlock_read(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_write(SturdyAsyncObject lock, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(lock, object);
        return publish_op(B::async_rwlock_write(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_guard_get(SturdyAsyncObject guard, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(guard, object);
        return copy_bytes_out(B::guard_get(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_guard_set(SturdyAsyncObject guard, const uint8_t *data, size_t size) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(guard, object);
        B::guard_set(object, bytes_of(data, size));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_guard_release(SturdyAsyncObject guard) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(guard, object);
        B::guard_release(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_notify_create(SturdyAsyncObject *out_notify) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_notify, "output pointer must not be null");
        *out_notify = publish_object(B::notify_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_notify_notified(SturdyAsyncObject notify, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(notify, object);
        return publish_op(B::notify_notified(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_notify_one(SturdyAsyncObject notify) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(notify, object);
        B::notify_one(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_notify_waiters(SturdyAsyncObject notify) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(notify, object);
        B::notify_waiters(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_barrier_create(uint64_t parties, SturdyAsyncObject *out_barrier) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_barrier, "output pointer must not be null");
        *out_barrier = publish_object(B::barrier_new(parties));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_barrier_wait(SturdyAsyncObject barrier, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(barrier, object);
        return publish_op(B::barrier_wait(object), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Tasks
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_task_spawn(SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        if (lane < STURDY_LANE_LIGHT || lane > STURDY_LANE_BLOCKING) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown lane");
        return publish_op(B::task_spawn(adapt_task(work, user_data), static_cast<B::Lane>(lane)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_task_spawn_on(SturdyAsyncObject pool, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        STURDY_WITH_OBJECT(pool, object);
        return publish_op(B::task_spawn_on(object, adapt_task(work, user_data)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_join_set_create(SturdyAsyncObject *out_set) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_set, "output pointer must not be null");
        *out_set = publish_object(B::join_set_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_join_set_spawn(SturdyAsyncObject set, SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane) {
    return run([&]() -> SturdyResult {
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        if (lane < STURDY_LANE_LIGHT || lane > STURDY_LANE_BLOCKING) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown lane");
        STURDY_WITH_OBJECT(set, object);
        B::join_set_spawn(object, adapt_task(work, user_data), static_cast<B::Lane>(lane));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_join_set_join_next(SturdyAsyncObject set, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(set, object);
        return publish_op(B::join_set_join_next(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_join_set_len(SturdyAsyncObject set, uint64_t *out_len) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_len, "output pointer must not be null");
        STURDY_WITH_OBJECT(set, object);
        *out_len = B::join_set_len(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_join_set_abort_all(SturdyAsyncObject set) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(set, object);
        B::join_set_abort_all(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_local_set_create(SturdyAsyncObject *out_set) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_set, "output pointer must not be null");
        *out_set = publish_object(B::local_set_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_local_set_spawn(SturdyAsyncObject set, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        STURDY_WITH_OBJECT(set, object);
        return publish_op(B::local_set_spawn(object, adapt_task(work, user_data)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_local_set_run_pending(SturdyAsyncObject set, uint64_t *out_ran) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(set, object);
        const auto ran = B::local_set_run_pending(object);
        if (out_ran != nullptr) *out_ran = ran;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_local_set_block_on(SturdyAsyncObject set, SturdyAsyncOp op) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(set, object);
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        B::local_set_block_on(object, resolved);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_create(const char *name, SturdyAsyncObject *out_thread) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_thread, "output pointer must not be null");
        *out_thread = publish_object(B::dedicated_thread_new(text(name)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_run(SturdyAsyncObject thread, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        STURDY_WITH_OBJECT(thread, object);
        return publish_op(B::dedicated_thread_run(object, adapt_task(work, user_data)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_pin_to_core(SturdyAsyncObject thread, uint32_t core_index, SturdyBool *out_pinned) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(thread, object);
        const bool pinned = B::dedicated_thread_pin_to_core(object, core_index);
        if (out_pinned != nullptr) *out_pinned = pinned ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_pin_to_fastest_core(SturdyAsyncObject thread, SturdyBool *out_pinned) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(thread, object);
        const bool pinned = B::dedicated_thread_pin_to_fastest_core(object);
        if (out_pinned != nullptr) *out_pinned = pinned ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_task_local_create(SturdyAsyncObject *out_local) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_local, "output pointer must not be null");
        *out_local = publish_object(B::task_local_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_task_local_scope(SturdyAsyncObject local, const uint8_t *value, size_t size, SturdyTaskFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        STURDY_WITH_OBJECT(local, object);
        B::task_local_scope(object, bytes_of(value, size), adapt_void(body, user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_task_local_get(SturdyAsyncObject local, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(local, object);
        B::Bytes value;
        if (!B::task_local_get(object, &value)) return set_error(STURDY_ERROR_NOT_AVAILABLE, "no value is bound here");
        return copy_bytes_out(value, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_main_thread_run(SturdyTaskFn fn, void *user_data) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        B::main_thread_run(adapt_void(fn, user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_main_thread_pump(void) {
    return run([&]() -> SturdyResult {
        B::main_thread_pump();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_main_thread_set_wake_hook(SturdyTaskFn hook, void *user_data) {
    return run([&]() -> SturdyResult {
        B::main_thread_set_wake_hook(hook != nullptr ? adapt_void(hook, user_data) : std::function<void()>{});
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Runtime, topology, timers, signals
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_scheduler_initialize(uint32_t workers) {
    return run([&]() -> SturdyResult {
        B::scheduler_initialize(workers);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_scheduler_shutdown(void) {
    return run([&]() -> SturdyResult {
        B::scheduler_shutdown();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_io_lane_configure(uint32_t workers, uint32_t max_pending, SturdyBool *out_applied) {
    return run([&]() -> SturdyResult {
        const bool applied = B::io_lane_configure(workers, max_pending);
        if (out_applied != nullptr) *out_applied = applied ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_io_lane_stats(SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_fields(B::io_lane_stats(), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_network_reactor_configure(uint32_t threads, SturdyBool *out_applied) {
    return run([&]() -> SturdyResult {
        const bool applied = B::network_reactor_configure(threads);
        if (out_applied != nullptr) *out_applied = applied ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_topology_ranked_cores(SturdyBool physical, uint32_t *buffer, size_t capacity, size_t *out_count) {
    return run([&]() -> SturdyResult {
        const auto cores = physical != STURDY_FALSE ? B::ranked_physical_cores() : B::ranked_logical_cores();
        if (out_count != nullptr) *out_count = cores.size();
        if (buffer == nullptr) return STURDY_OK;
        const size_t count = (std::min)(capacity, cores.size());
        for (size_t i = 0; i < count; ++i) buffer[i] = cores[i];
        return count < cores.size() ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "buffer is smaller than the result") : STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_thread_pin_current(uint32_t core_index, SturdyBool *out_pinned) {
    return run([&]() -> SturdyResult {
        const bool pinned = B::pin_current_thread_to_core(core_index);
        if (out_pinned != nullptr) *out_pinned = pinned ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_timer_sleep_until(int64_t unix_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::sleep_until_unix_ms(unix_ms), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_interval_create(int64_t period_ms, SturdyAsyncObject *out_interval) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_interval, "output pointer must not be null");
        *out_interval = publish_object(B::interval_new(period_ms));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_interval_tick(SturdyAsyncObject interval, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(interval, object);
        return publish_op(B::interval_tick(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_signal_wait(const int32_t *signals, uint32_t count, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (signals == nullptr || count == 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "at least one signal is required");
        return publish_op(B::signal_wait(std::vector<std::int32_t>(signals, signals + count)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_signal_ctrl_c(SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::ctrl_c(), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Streams
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_stream_from_channel(SturdyAsyncObject receiver, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(receiver, object);
        return make_stream_from(out_stream, [&] { return B::stream_from_channel(object); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_of_items(const SturdyBytesView *items, uint32_t count, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (items == nullptr && count != 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "items must not be null");
        std::vector<B::Bytes> list;
        for (uint32_t i = 0; i < count; ++i) list.push_back(bytes_of(items[i].data, items[i].size));
        return make_stream_from(out_stream, [&] { return B::stream_of_items(list); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_empty(SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult { return make_stream_from(out_stream, [] { return B::stream_empty(); }); });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_interval(int64_t period_ms, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult { return make_stream_from(out_stream, [&] { return B::stream_interval(period_ms); }); });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_generate(SturdyStreamSourceFn source, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (source == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source must not be null");
        return make_stream_from(out_stream, [&] {
            return B::stream_generate([source, user_data]() {
                B::StreamMaybe item;
                item.value = collect_sink([&](SturdyBytesSink *sink) { item.keep = source(user_data, sink) != STURDY_FALSE; });
                return item;
            });
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_map(SturdyAsyncObject stream, SturdyStreamMapFn fn, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] {
            return B::stream_map(object, [fn, user_data](B::Bytes item) {
                return collect_sink([&](SturdyBytesSink *sink) { fn(user_data, item.data(), item.size(), sink); });
            });
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_filter(SturdyAsyncObject stream, SturdyStreamPredicateFn pred, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (pred == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "pred must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] {
            return B::stream_filter(object, [pred, user_data](const B::Bytes &item) { return pred(user_data, item.data(), item.size()) != STURDY_FALSE; });
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_filter_map(SturdyAsyncObject stream, SturdyStreamFilterMapFn fn, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] {
            return B::stream_filter_map(object, [fn, user_data](B::Bytes item) {
                B::StreamMaybe out;
                out.value = collect_sink([&](SturdyBytesSink *sink) { out.keep = fn(user_data, item.data(), item.size(), sink) != STURDY_FALSE; });
                return out;
            });
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_inspect(SturdyAsyncObject stream, SturdyStreamEachFn fn, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_inspect(object, [fn, user_data](const B::Bytes &item) { fn(user_data, item.data(), item.size()); }); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_take(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_take(object, count); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_skip(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_skip(object, count); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_take_while(SturdyAsyncObject stream, SturdyStreamPredicateFn pred, void *user_data, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (pred == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "pred must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] {
            return B::stream_take_while(object, [pred, user_data](const B::Bytes &item) { return pred(user_data, item.data(), item.size()) != STURDY_FALSE; });
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_enumerate(SturdyAsyncObject stream, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_enumerate(object); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_chunks(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_chunks(object, count); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_chunks_timeout(SturdyAsyncObject stream, uint64_t count, int64_t max_wait_ms, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_chunks_timeout(object, count, max_wait_ms); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_chain(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(first, a);
        STURDY_WITH_OBJECT(second, b);
        return make_stream_from(out_stream, [&] { return B::stream_chain(a, b); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_merge(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(first, a);
        STURDY_WITH_OBJECT(second, b);
        return make_stream_from(out_stream, [&] { return B::stream_merge(a, b); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_zip(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(first, a);
        STURDY_WITH_OBJECT(second, b);
        return make_stream_from(out_stream, [&] { return B::stream_zip(a, b); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_map_concurrent(SturdyAsyncObject stream, SturdyStreamMapFn fn, void *user_data, uint64_t concurrency, SturdyBool ordered, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] {
            return B::stream_map_concurrent(
                object,
                [fn, user_data](B::Bytes item) {
                    return collect_sink([&](SturdyBytesSink *sink) { fn(user_data, item.data(), item.size(), sink); });
                },
                concurrency, ordered != STURDY_FALSE);
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_throttle(SturdyAsyncObject stream, int64_t period_ms, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_throttle(object, period_ms); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_timeout_each(SturdyAsyncObject stream, int64_t limit_ms, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        return make_stream_from(out_stream, [&] { return B::stream_timeout_each(object, limit_ms); });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_next(SturdyAsyncObject stream, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_next(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_collect(SturdyAsyncObject stream, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_collect(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_for_each(SturdyAsyncObject stream, SturdyStreamEachFn fn, void *user_data, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_for_each(object, [fn, user_data](const B::Bytes &item) { fn(user_data, item.data(), item.size()); }), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_fold(SturdyAsyncObject stream, const uint8_t *initial, size_t initial_size, SturdyStreamFoldFn fn, void *user_data, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_fold(object, bytes_of(initial, initial_size),
                                         [fn, user_data](B::Bytes accumulator, B::Bytes item) {
                                             return collect_sink([&](SturdyBytesSink *sink) { fn(user_data, accumulator.data(), accumulator.size(), item.data(), item.size(), sink); });
                                         }),
                          out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_count(SturdyAsyncObject stream, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_count(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_last(SturdyAsyncObject stream, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::stream_last(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stream_forward(SturdyAsyncObject stream, SturdyAsyncObject sender, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        STURDY_WITH_OBJECT(sender, target);
        return publish_op(B::stream_forward(object, target), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// More data parallelism
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_par_num_threads(uint32_t *out_threads) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_threads, "output pointer must not be null");
        *out_threads = B::par_num_threads();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_join(SturdyTaskFn a, void *a_user_data, SturdyTaskFn b, void *b_user_data) {
    return run([&]() -> SturdyResult {
        if (a == nullptr || b == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "both functions are required");
        B::par_join(adapt_void(a, a_user_data), adapt_void(b, b_user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_scope(SturdyParScopeFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        B::par_scope([&](const B::ObjectPtr &scope) { with_temporary_handle(scope, [&](SturdyAsyncObject handle) { body(user_data, handle); }); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_scope_spawn(SturdyAsyncObject scope, SturdyTaskFn fn, void *user_data) {
    return run([&]() -> SturdyResult {
        if (fn == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "fn must not be null");
        STURDY_WITH_OBJECT(scope, object);
        B::par_scope_spawn(object, adapt_void(fn, user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_for_windows(uint64_t count, uint64_t window, SturdyParForFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        B::par_for_windows(count, window, [&](uint64_t begin) { body(user_data, begin); });
        return STURDY_OK;
    });
}

#define STURDY_PAR_PREDICATE(name, out_type, out_expr)                                                                                    \
    SturdyResult STURDY_ABI_CALL name(uint64_t count, SturdyParPredicateFn pred, void *user_data, out_type *out) {                        \
        return run([&]() -> SturdyResult {                                                                                                \
            STURDY_REQUIRE(out, "output pointer must not be null");                                                                       \
            if (pred == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "pred must not be null");                                \
            auto test = [&](uint64_t i) { return pred(user_data, i) != STURDY_FALSE; };                                                   \
            *out = out_expr;                                                                                                              \
            return STURDY_OK;                                                                                                             \
        });                                                                                                                               \
    }
STURDY_PAR_PREDICATE(sturdy_par_any, SturdyBool, B::par_any(count, test) ? STURDY_TRUE : STURDY_FALSE)
STURDY_PAR_PREDICATE(sturdy_par_all, SturdyBool, B::par_all(count, test) ? STURDY_TRUE : STURDY_FALSE)
STURDY_PAR_PREDICATE(sturdy_par_count, uint64_t, B::par_count(count, test))
STURDY_PAR_PREDICATE(sturdy_par_find_first, int64_t, B::par_find_first(count, test))
STURDY_PAR_PREDICATE(sturdy_par_find_any, int64_t, B::par_find_any(count, test))
#undef STURDY_PAR_PREDICATE

SturdyResult STURDY_ABI_CALL sturdy_par_map_reduce_i64(uint64_t count, SturdyParMapI64Fn map, int64_t identity, SturdyParReduceI64Fn reduce, void *user_data, int64_t *out_result) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_result, "output pointer must not be null");
        if (map == nullptr || reduce == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "map and reduce are required");
        *out_result = B::par_map_reduce_i64(count, [&](uint64_t i) { return map(user_data, i); }, identity, [&](int64_t a, int64_t b) { return reduce(user_data, a, b); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_map_reduce_f64(uint64_t count, SturdyParMapF64Fn map, double identity, SturdyParReduceF64Fn reduce, void *user_data, double *out_result) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_result, "output pointer must not be null");
        if (map == nullptr || reduce == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "map and reduce are required");
        *out_result = B::par_map_reduce_f64(count, [&](uint64_t i) { return map(user_data, i); }, identity, [&](double a, double b) { return reduce(user_data, a, b); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_sort_by(void *data, uint64_t count, uint64_t element_size, SturdyBool stable, SturdyParLessFn less, void *user_data) {
    return run([&]() -> SturdyResult {
        if (less == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "less must not be null");
        B::par_sort_by(data, count, element_size, stable != STURDY_FALSE, [&](const void *a, const void *b) { return less(user_data, a, b) != STURDY_FALSE; });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_reduce(SturdyNumericType type, SturdyReduction reduction, const void *data, uint64_t count, SturdyNumericValue *out_value, SturdyBool *out_has_value) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_value, "output pointer must not be null");
        if (type < STURDY_NUMERIC_F32 || type > STURDY_NUMERIC_U64) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown element type");
        if (reduction < STURDY_REDUCE_SUM || reduction > STURDY_REDUCE_MAX) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown reduction");
        B::NumericValue value;
        const bool has = B::par_reduce(static_cast<B::Numeric>(type), static_cast<B::Reduction>(reduction), data, count, &value);
        out_value->f64 = value.f64;
        out_value->i64 = value.i64;
        out_value->u64 = value.u64;
        if (out_has_value != nullptr) *out_has_value = has ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

} // extern "C"
