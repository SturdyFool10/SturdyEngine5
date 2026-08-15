// Semaphores, fences, and queue submission.
//
// The RHI's timeline semaphore maps onto ID3D12Fence with no emulation whatsoever — both are a
// monotonically increasing u64 that a queue or the host can signal and either can wait on. The host
// *fence* is the one primitive D3D12 lacks: there is no binary fence, so FenceRecord models one as a
// monotonic fence plus the value that currently counts as "signaled" (see D3D12Device.hpp).
//
// One D3D12 rule shapes submit() and is worth stating: waits and signals are queue operations issued
// *around* ExecuteCommandLists, not part of it. Vulkan's VkSubmitInfo carries them in one call;
// D3D12 needs Wait(), then ExecuteCommandLists(), then Signal(), in that order, on the same queue.
// The ordering is what makes the wait apply to the submitted work rather than to whatever came next.
#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {

        // Nanoseconds -> the millisecond granularity WaitForSingleObject takes, rounding up so a
        // sub-millisecond timeout still waits rather than degenerating into a poll.
        [[nodiscard]] DWORD to_wait_milliseconds(u64 timeout_ns) noexcept {
            if (timeout_ns == rhi::wait_forever) {
                return INFINITE;
            }
            const u64 milliseconds = (timeout_ns + 999'999ull) / 1'000'000ull;
            return milliseconds >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(milliseconds);
        }

    } // namespace

    // ─── Semaphores (timelines) ──────────────────────────────────────────────────

    rhi::RhiExpected<rhi::SemaphoreHandle> D3D12Device::create_semaphore(const rhi::SemaphoreDesc &desc) {
        SemaphoreRecord record{};
        if (const HRESULT hr = device_->CreateFence(desc.initial_value, D3D12_FENCE_FLAG_NONE,
                                                    IID_PPV_ARGS(&record.fence));
            FAILED(hr)) {
            return hresult_error(hr, "create_semaphore (CreateFence)");
        }
        set_debug_name(record.fence.Get(), desc.label);
        return semaphores_.insert(std::move(record));
    }

    void D3D12Device::destroy_semaphore(rhi::SemaphoreHandle handle) noexcept { semaphores_.erase(handle); }

    rhi::RhiExpected<u64> D3D12Device::semaphore_value(rhi::SemaphoreHandle handle) const {
        const SemaphoreRecord *record = semaphores_.find(handle);
        if (record == nullptr) {
            return invalid_argument("semaphore_value: unknown semaphore handle.");
        }
        return record->fence->GetCompletedValue();
    }

    rhi::RhiResult D3D12Device::wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns) {
        ZoneScopedN("D3D12Device::wait_semaphore");
        const SemaphoreRecord *record = semaphores_.find(handle);
        if (record == nullptr) {
            return invalid_argument("wait_semaphore: unknown semaphore handle.");
        }
        if (record->fence->GetCompletedValue() >= value) {
            return {};
        }

        // A per-call event rather than a cached one: wait_semaphore() is documented callable from any
        // thread on the same handle, and an event shared between two concurrent waiters would let one
        // consume the other's signal.
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return operation_failed("wait_semaphore: CreateEventW failed.");
        }
        struct EventGuard {
            HANDLE handle;
            ~EventGuard() { CloseHandle(handle); }
        } guard{event};

        if (const HRESULT hr = record->fence->SetEventOnCompletion(value, event); FAILED(hr)) {
            return hresult_error(hr, "wait_semaphore (SetEventOnCompletion)");
        }
        if (WaitForSingleObject(event, to_wait_milliseconds(timeout_ns)) == WAIT_TIMEOUT) {
            return rhi::rhi_error(rhi::RhiErrorCode::NotReady, "wait_semaphore: timed out.");
        }
        return {};
    }

    rhi::RhiResult D3D12Device::signal_semaphore(rhi::SemaphoreHandle handle, u64 value) {
        const SemaphoreRecord *record = semaphores_.find(handle);
        if (record == nullptr) {
            return invalid_argument("signal_semaphore: unknown semaphore handle.");
        }
        if (const HRESULT hr = record->fence->Signal(value); FAILED(hr)) {
            return hresult_error(hr, "signal_semaphore (Signal)");
        }
        return {};
    }

    // ─── Fences ──────────────────────────────────────────────────────────────────

    rhi::RhiExpected<rhi::FenceHandle> D3D12Device::create_fence(const rhi::FenceDesc &desc) {
        FenceRecord record{};
        if (const HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&record.fence));
            FAILED(hr)) {
            return hresult_error(hr, "create_fence (CreateFence)");
        }
        record.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (record.event == nullptr) {
            return operation_failed("create_fence: CreateEventW failed.");
        }
        record.signaled_without_gpu = desc.signaled;
        set_debug_name(record.fence.Get(), desc.label);
        return fences_.insert(std::move(record));
    }

    void D3D12Device::destroy_fence(rhi::FenceHandle handle) noexcept {
        if (auto record = fences_.extract(handle)) {
            if (record->event != nullptr) {
                CloseHandle(record->event);
            }
        }
    }

    rhi::RhiExpected<bool> D3D12Device::wait_fences(span<const rhi::FenceHandle> fences, bool wait_all,
                                                     u64 timeout_ns) {
        ZoneScopedN("D3D12Device::wait_fences");
        if (fences.empty()) {
            return true;
        }

        // Every fence's event is armed first, then all of them are waited on together, so `wait_all ==
        // false` really is "whichever completes first" rather than a serial walk that would block on
        // the first fence regardless.
        std::vector<HANDLE> events;
        events.reserve(fences.size());
        bool any_already_signaled = false;

        for (rhi::FenceHandle handle : fences) {
            FenceRecord *record = fences_.find(handle);
            if (record == nullptr) {
                return invalid_argument("wait_fences: unknown fence handle.");
            }
            if (record->signaled_without_gpu) {
                any_already_signaled = true;
                continue;
            }
            if (record->wait_value == 0) {
                // Nothing has been submitted with this fence, so nothing will ever signal it. Vulkan
                // would block forever here; reporting the caller error is strictly more useful than
                // hanging, and it cannot be confused with a timeout because a timeout returns `false`
                // rather than an error.
                return invalid_argument("wait_fences: the fence has not been armed by any submission "
                                        "(it was never passed to submit(), or has been reset since).");
            }
            if (record->fence->GetCompletedValue() >= record->wait_value) {
                any_already_signaled = true;
                continue;
            }
            if (const HRESULT hr = record->fence->SetEventOnCompletion(record->wait_value, record->event);
                FAILED(hr)) {
                return hresult_error(hr, "wait_fences (SetEventOnCompletion)");
            }
            events.push_back(record->event);
        }

        if (events.empty()) {
            return true;
        }
        if (!wait_all && any_already_signaled) {
            return true;
        }

        const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                                    wait_all ? TRUE : FALSE, to_wait_milliseconds(timeout_ns));
        if (result == WAIT_TIMEOUT) {
            // Not an error, and explicitly not permission to reclaim anything the fence protects — see
            // RhiDevice::wait_fences()'s own doc comment on why this distinction has to survive.
            return false;
        }
        if (result == WAIT_FAILED) {
            return operation_failed("wait_fences: WaitForMultipleObjects failed.");
        }
        return true;
    }

    rhi::RhiResult D3D12Device::reset_fences(span<const rhi::FenceHandle> fences) {
        for (rhi::FenceHandle handle : fences) {
            FenceRecord *record = fences_.find(handle);
            if (record == nullptr) {
                return invalid_argument("reset_fences: unknown fence handle.");
            }
            // The underlying ID3D12Fence is monotonic and is deliberately *not* rewound — rewinding a
            // fence another queue may still be waiting on is undefined. Resetting only clears the
            // armed state; the next submission arms it with a fresh, higher value.
            record->wait_value = 0;
            record->signaled_without_gpu = false;
        }
        return {};
    }

    // ─── Submission ──────────────────────────────────────────────────────────────

    rhi::RhiResult D3D12Device::submit(const rhi::SubmitDesc &desc) {
        ZoneScopedN("D3D12Device::submit");
        if (auto valid = validate_queue_lane(desc.queue, "submit"); !valid) {
            return valid;
        }
        ID3D12CommandQueue *queue = queue_for_lane(desc.queue);
        if (queue == nullptr) {
            return operation_failed("submit: this device has no queue for the requested lane.");
        }

        // Waits precede ExecuteCommandLists so they gate the work being submitted (see this file's
        // header comment). A queue wait is a GPU-side wait: it returns immediately on the CPU.
        for (const rhi::QueueSemaphoreWait &wait : desc.waits) {
            const SemaphoreRecord *semaphore = semaphores_.find(wait.semaphore);
            if (semaphore == nullptr) {
                return invalid_argument("submit: a wait names an unknown semaphore handle.");
            }
            // `stages` is intentionally dropped: D3D12 queue waits are whole-queue scoped, with no
            // stage-specific variant. QueueSemaphoreWait documents exactly this ("APIs without
            // stage-specific waits ignore this and wait at queue scope"), and widening a wait is
            // always safe — it can only reduce overlap, never admit a race.
            (void)wait.stages;
            if (const HRESULT hr = queue->Wait(semaphore->fence.Get(), wait.value); FAILED(hr)) {
                return hresult_error(hr, "submit (Wait)");
            }
        }

        std::vector<ID3D12CommandList *> lists;
        lists.reserve(desc.command_buffers.size());
        for (rhi::CommandBufferHandle handle : desc.command_buffers) {
            CommandBufferRecord *record = command_buffers_.find(handle);
            if (record == nullptr) {
                return invalid_argument("submit: unknown command buffer handle.");
            }
            if (record->list_type != to_d3d12(desc.queue.queue)) {
                return invalid_argument("submit: a command buffer recorded for one queue class cannot be submitted "
                                        "to another (D3D12 command list types are fixed at creation).");
            }
            lists.push_back(record->list.Get());
        }
        if (!lists.empty()) {
            queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        }

        for (const rhi::QueueSemaphoreSignal &signal : desc.signals) {
            const SemaphoreRecord *semaphore = semaphores_.find(signal.semaphore);
            if (semaphore == nullptr) {
                return invalid_argument("submit: a signal names an unknown semaphore handle.");
            }
            (void)signal.stages; // Same queue-scope reasoning as waits above.
            if (const HRESULT hr = queue->Signal(semaphore->fence.Get(), signal.value); FAILED(hr)) {
                return hresult_error(hr, "submit (Signal)");
            }
        }

        if (desc.fence.is_valid()) {
            FenceRecord *fence = fences_.find(desc.fence);
            if (fence == nullptr) {
                return invalid_argument("submit: unknown fence handle.");
            }
            fence->wait_value = ++fence->next_value;
            fence->signaled_without_gpu = false;
            if (const HRESULT hr = queue->Signal(fence->fence.Get(), fence->wait_value); FAILED(hr)) {
                return hresult_error(hr, "submit (fence Signal)");
            }
        }

        // `presented_textures` needs no handling here. Under Vulkan it exists to drive the internal
        // binary render-finished semaphore vkQueuePresentKHR must wait on; DXGI has no such object —
        // Present() is ordered against the queue that owns the swapchain by the runtime itself, so
        // declaring the intent is enough and there is nothing further to synchronize.
        (void)desc.presented_textures;
        (void)desc.flags;
        return {};
    }

} // namespace SFT::D3D12
