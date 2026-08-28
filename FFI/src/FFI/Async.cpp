/// C ABI implementation of background task spawning.
///
/// `Async::Scheduler::spawn` is a template returning a `TaskHandle<R>`, which cannot cross a C
/// boundary. This layer spawns a fixed `void()` shape wrapping the caller's function pointer and
/// owns the resulting handle, exposing it by token like an imported glTF scene.
///
/// The one real hazard is waiting from a worker thread: the task being waited on may be queued
/// behind the waiter on the very pool the waiter is occupying, so the wait would never complete.
/// `sturdy_async_wait` refuses that rather than deadlocking, which mirrors how `Schedule::run`
/// treats the same mistake.

#include <Foundation/Foundation.hpp>

#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <Async/Scheduler.hpp>
#include <Async/Task.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u64;

    /// Task handles this ABI owns, keyed by the token handed to the caller.
    ///
    /// A `unique_ptr` so the address is stable while the map changes shape — the handle table holds
    /// a raw pointer to the entry.
    std::mutex g_task_mutex;
    std::map<u64, std::unique_ptr<SFT::Async::TaskHandle<void>>> g_tasks;

    /// Resolves a task handle to the engine-side handle it refers to.
    ///
    /// @param task Handle produced by `sturdy_async_spawn`.
    /// @param out_handle Receives the borrowed handle on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_task(SturdyTask task,
                                            SFT::Async::TaskHandle<void> **out_handle) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(task.token, HandleKind::Task, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_handle = static_cast<SFT::Async::TaskHandle<void> *>(pointer);
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_async_is_running(SturdyBool *out_running) {
    return guarded([&]() -> SturdyResult {
        if (out_running == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *out_running = SFT::Async::Scheduler::is_running() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_worker_count(uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        // Reported as zero rather than as a failure when the scheduler has not started: "no workers
        // yet" is a state a caller can act on, not an error.
        *out_count = SFT::Async::Scheduler::is_running() ? SFT::Async::Scheduler::worker_count() : 0u;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_is_worker_thread(SturdyBool *out_worker) {
    return guarded([&]() -> SturdyResult {
        if (out_worker == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *out_worker = SFT::Async::Scheduler::is_worker_thread() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_spawn(SturdyTaskFn task,
                                                void *user_data,
                                                SturdyTaskWeight weight,
                                                SturdyTask *out_task) {
    return guarded([&]() -> SturdyResult {
        if (task == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "task body must not be null");
        }

        SFT::Async::TaskWeight engine_weight = SFT::Async::TaskWeight::Light;
        switch (weight) {
        case STURDY_TASK_LIGHT:
            engine_weight = SFT::Async::TaskWeight::Light;
            break;
        case STURDY_TASK_HEAVY:
            engine_weight = SFT::Async::TaskWeight::Heavy;
            break;
        case STURDY_TASK_WEIGHT_FORCE_U32:
        default:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized task weight");
        }

        if (!SFT::Async::Scheduler::is_running()) {
            SFT::Async::Scheduler::initialize();
        }

        auto handle = SFT::Async::Scheduler::spawn(
            [task, user_data]() noexcept {
                // The firewall has to be here as well as at the entry points: this runs on a worker
                // thread, so an exception escaping it would unwind out of the scheduler rather than
                // out of an ABI call, where nothing is left to catch it.
                try {
                    task(user_data);
                } catch (...) {
                }
            },
            engine_weight);

        // A caller that does not want a handle still gets its task run; there is simply nothing to
        // observe or release.
        if (out_task == nullptr) {
            return STURDY_OK;
        }

        auto owned = std::make_unique<SFT::Async::TaskHandle<void>>(std::move(handle));
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::Task, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_task_mutex};
            g_tasks.emplace(token, std::move(owned));
        }
        out_task->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_is_done(SturdyTask task, SturdyBool *out_done) {
    return guarded([&]() -> SturdyResult {
        if (out_done == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Async::TaskHandle<void> *handle = nullptr;
        const SturdyResult resolved = resolve_task(task, &handle);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_done = handle->is_done() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_wait(SturdyTask task) {
    return guarded([&]() -> SturdyResult {
        // Checked before resolving the handle: waiting from a worker is wrong regardless of which
        // task is named, and reporting the real problem beats reporting a handle error.
        if (SFT::Async::Scheduler::is_worker_thread()) {
            return set_error(STURDY_ERROR_BUSY,
                             "cannot wait on a task from a scheduler worker thread; poll with "
                             "sturdy_async_is_done instead");
        }

        SFT::Async::TaskHandle<void> *handle = nullptr;
        const SturdyResult resolved = resolve_task(task, &handle);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        handle->wait();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_async_release(SturdyTask task) {
    return guarded([&]() -> SturdyResult {
        SFT::Async::TaskHandle<void> *handle = nullptr;
        const SturdyResult resolved = resolve_task(task, &handle);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // Revoked before the entry is erased, so a concurrent resolve sees an expired handle rather
        // than a pointer about to be freed. The task itself keeps running: the scheduler holds its
        // own reference to the shared state, so dropping this handle cannot strand it.
        revoke_handle(task.token);
        const std::lock_guard<std::mutex> lock{g_task_mutex};
        g_tasks.erase(task.token);
        return STURDY_OK;
    });
}

} // extern "C"
