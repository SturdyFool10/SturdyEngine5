/// C ABI for the shared-state primitives: blocking mutex, SPSC ring, Arc, Promise and execution senders.
/// A thin translation of the matching part of `Async/Binding.hpp`.

#include <FFI/AsyncNetSupport.hpp>

#include <FFI/Sturdy.h>

using namespace SFT::Ffi::AsyncSupport;


namespace {

    /// Runs a task-shaped C callback and collects what it set.
    template <typename Call>
    B::TaskOutput collect_output(Call &&call) {
        B::TaskOutput value;
        SturdyTaskOutput output{&value};
        call(&output);
        return value;
    }

    template <typename Body>
    void with_temporary_handle(const B::ObjectPtr &object, Body &&body) {
        const SturdyAsyncObject handle = publish_object(object);
        struct Release {
            SturdyAsyncObject handle;
            ~Release() { (void)sturdy_object_release(handle); }
        } release{handle};
        body(handle);
    }

    SFT::Async::IoErrorCode code_of(SturdyIoError code) {
        const int raw = static_cast<int>(code) - 1;
        if (raw < 0 || raw > static_cast<int>(SFT::Async::IoErrorCode::ProtocolError)) return SFT::Async::IoErrorCode::Unknown;
        return static_cast<SFT::Async::IoErrorCode>(raw);
    }

    void report(SturdyBool *out, bool value) {
        if (out != nullptr) *out = value ? STURDY_TRUE : STURDY_FALSE;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_create(const uint8_t *initial, size_t size, const char *debug_name, SturdyAsyncObject *out_mutex) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_mutex, "output pointer must not be null");
        *out_mutex = publish_object(B::sync_mutex_new(bytes_of(initial, size), text(debug_name)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_guard, "output pointer must not be null");
        STURDY_WITH_OBJECT(mutex, object);
        *out_guard = publish_object(B::sync_mutex_lock(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_try_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_guard, "output pointer must not be null");
        STURDY_WITH_OBJECT(mutex, object);
        *out_guard = publish_object(B::sync_mutex_try_lock(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_create(uint64_t capacity, uint64_t max_item_bytes, SturdyAsyncObject *out_ring) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_ring, "output pointer must not be null");
        *out_ring = publish_object(B::ring_new(capacity, max_item_bytes));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_try_push(SturdyAsyncObject ring, const uint8_t *data, size_t size, SturdyRingStatus *out_status) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_status, "output pointer must not be null");
        STURDY_WITH_OBJECT(ring, object);
        *out_status = static_cast<SturdyRingStatus>(B::ring_try_push(object, bytes_of(data, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_try_pop(SturdyAsyncObject ring, uint8_t *buffer, size_t capacity, size_t *out_length, SturdyRingStatus *out_status) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_status, "output pointer must not be null");
        STURDY_WITH_OBJECT(ring, object);
        if (capacity < B::ring_max_item_bytes(object) && buffer != nullptr) return set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "the buffer must hold the ring's largest item");
        B::Bytes item;
        const auto status = B::ring_try_pop(object, &item);
        *out_status = static_cast<SturdyRingStatus>(status);
        if (status != B::RingStatus::Ok) return STURDY_OK;
        if (out_length != nullptr) *out_length = item.size();
        if (buffer != nullptr && !item.empty()) std::memcpy(buffer, item.data(), item.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_drain(SturdyAsyncObject ring, uint64_t max_items, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(ring, object);
        auto items = B::ring_drain(object, max_items);
        auto op = std::make_shared<B::Op>();
        B::Value value;
        value.status = B::OpStatus::Ok;
        value.number = static_cast<std::int64_t>(items.size());
        value.bytes = B::pack_frames(items);
        op->complete(std::move(value));
        return publish_op(std::move(op), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_size(SturdyAsyncObject ring, uint64_t *out_size) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_size, "output pointer must not be null");
        STURDY_WITH_OBJECT(ring, object);
        *out_size = B::ring_size(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_capacity(SturdyAsyncObject ring, uint64_t *out_capacity) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_capacity, "output pointer must not be null");
        STURDY_WITH_OBJECT(ring, object);
        *out_capacity = B::ring_capacity(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ring_max_item_bytes(SturdyAsyncObject ring, uint64_t *out_bytes) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_bytes, "output pointer must not be null");
        STURDY_WITH_OBJECT(ring, object);
        *out_bytes = B::ring_max_item_bytes(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_create(const uint8_t *data, size_t size, SturdyAsyncObject *out_arc) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_arc, "output pointer must not be null");
        *out_arc = publish_object(B::arc_new(bytes_of(data, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_create_foreign(void *pointer, SturdyArcDestroyFn destroy, SturdyAsyncObject *out_arc) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_arc, "output pointer must not be null");
        std::function<void()> release;
        if (destroy != nullptr) release = [destroy, pointer] { no_throw([&] { destroy(pointer); }); };
        *out_arc = publish_object(B::arc_new_foreign(pointer, std::move(release)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_clone(SturdyAsyncObject arc, SturdyAsyncObject *out_arc) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_arc, "output pointer must not be null");
        STURDY_WITH_OBJECT(arc, object);
        *out_arc = publish_object(B::arc_clone(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_use_count(SturdyAsyncObject arc, uint64_t *out_count) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_count, "output pointer must not be null");
        STURDY_WITH_OBJECT(arc, object);
        *out_count = B::arc_use_count(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_ptr_eq(SturdyAsyncObject a, SturdyAsyncObject b, SturdyBool *out_same) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_same, "output pointer must not be null");
        STURDY_WITH_OBJECT(a, left);
        STURDY_WITH_OBJECT(b, right);
        report(out_same, B::arc_ptr_eq(left, right));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_bytes(SturdyAsyncObject arc, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(arc, object);
        return copy_bytes_out(B::arc_bytes(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_arc_pointer(SturdyAsyncObject arc, void **out_pointer) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_pointer, "output pointer must not be null");
        STURDY_WITH_OBJECT(arc, object);
        *out_pointer = B::arc_pointer(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_promise_create(SturdyAsyncObject *out_promise, SturdyAsyncOp *out_future) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_promise, "output pointer must not be null");
        STURDY_OP_OUT(out_future);
        auto ends = B::promise_new();
        *out_promise = publish_object(std::move(ends.promise));
        return publish_op(std::move(ends.future), out_future);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_promise_set_value(SturdyAsyncObject promise, int64_t number, const uint8_t *data, size_t size, const char *text_in, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(promise, object);
        report(out_first, B::promise_set_value(object, number, bytes_of(data, size), text(text_in)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_promise_set_error(SturdyAsyncObject promise, SturdyIoError code, const char *message, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(promise, object);
        report(out_first, B::promise_set_error(object, code_of(code), text(message)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_promise_cancel(SturdyAsyncObject promise, SturdyBool *out_first) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(promise, object);
        report(out_first, B::promise_cancel(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_promise_is_completed(SturdyAsyncObject promise, SturdyBool *out_completed) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_completed, "output pointer must not be null");
        STURDY_WITH_OBJECT(promise, object);
        report(out_completed, B::promise_is_completed(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_ready(int64_t number, const uint8_t *data, size_t size, const char *text_in, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::op_ready(number, bytes_of(data, size), text(text_in)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_failed(SturdyIoError code, const char *message, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::op_failed(code_of(code), text(message)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_exec_just(SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane, SturdyAsyncObject *out_sender) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointer must not be null");
        if (work == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "work must not be null");
        if (lane < STURDY_LANE_LIGHT || lane > STURDY_LANE_BLOCKING) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown lane");
        *out_sender = publish_object(B::exec_just(
            [work, user_data](B::ObjectPtr cancel) {
                return collect_output([&](SturdyTaskOutput *output) { with_temporary_handle(cancel, [&](SturdyAsyncObject token) { work(user_data, token, output); }); });
            },
            static_cast<B::Lane>(lane)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_exec_then(SturdyAsyncObject sender, SturdyTaskThenFn then, void *user_data, SturdyTaskLane lane, SturdyAsyncObject *out_sender) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_sender, "output pointer must not be null");
        if (then == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "then must not be null");
        if (lane < STURDY_LANE_LIGHT || lane > STURDY_LANE_BLOCKING) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown lane");
        STURDY_WITH_OBJECT(sender, object);
        *out_sender = publish_object(B::exec_then(
            object,
            [then, user_data](B::ObjectPtr cancel, const B::TaskOutput &input) {
                SturdyTaskInput view{input.number, input.bytes.data(), input.bytes.size(), input.text.c_str()};
                return collect_output([&](SturdyTaskOutput *output) { with_temporary_handle(cancel, [&](SturdyAsyncObject token) { then(user_data, token, &view, output); }); });
            },
            static_cast<B::Lane>(lane)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_exec_submit(SturdyAsyncObject sender, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(sender, object);
        return publish_op(B::exec_submit(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_exec_sync_wait(SturdyAsyncObject sender, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(sender, object);
        auto op = B::exec_submit(object);
        (void)op->wait(-1);
        return publish_op(std::move(op), out_op);
    });
}

} // extern "C"
