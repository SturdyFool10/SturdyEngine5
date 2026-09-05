#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <vector>

namespace SFT::Core::WebGpu {

    // WebGPU's execution model is why this file looks the way it does. There is one queue, and work
    // submitted to it is guaranteed to complete in submission order. There are no semaphores, no
    // fences, and no barriers in the API at all — the implementation infers every dependency from
    // resource usage and inserts whatever the underlying driver needs.
    //
    // So the RHI's synchronisation primitives are *satisfied* here rather than emulated: a caller
    // that waits on a semaphore in the right order gets the ordering it asked for, because the queue
    // already provides it. Semaphores are modelled as plain counters the submit path advances, which
    // is exactly what they mean on a single in-order queue; a wait for a value not yet submitted
    // falls through to a device wait, since ordering alone can't tell it that. Fences, unlike
    // semaphores, need to answer "has the GPU actually finished this submission yet", which ordering
    // doesn't give you either -- so each one tracks the real `WGPUFuture` from
    // `wgpuQueueOnSubmittedWorkDone`, and `wait_fences` polls or blocks on it via
    // `wgpuInstanceWaitAny` (see there for why `timeout_ns == 0` matters on Web specifically).

    /// Submits recorded command buffers.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult WebGpuDevice::submit(const rhi::SubmitDesc &desc) {
        std::vector<WGPUCommandBuffer> buffers;
        buffers.reserve(desc.command_buffers.size());
        for (rhi::CommandBufferHandle handle : desc.command_buffers) {
            WGPUCommandBuffer *buffer = command_buffers_.find(handle);
            if (buffer == nullptr) {
                return std::unexpected(webgpu_error("submit", "unknown command buffer handle"));
            }
            buffers.push_back(*buffer);
        }

        if (!buffers.empty()) {
            wgpuQueueSubmit(queue_, buffers.size(), buffers.data());
            // Whatever push-constant slices these command buffers named are now owned by this
            // submission and cannot be reused until it completes.
            mark_push_constants_submitted();
        }

        // Command buffers are single-use in WebGPU: submitting consumes them, so their handles are
        // retired here rather than leaving dangling entries the caller could resubmit.
        for (rhi::CommandBufferHandle handle : desc.command_buffers) {
            command_buffers_.erase(handle, [](WGPUCommandBuffer &buffer) {
                wgpuCommandBufferRelease(buffer);
            });
        }

        // Signals are satisfied immediately: the queue is in-order, so anything that waits on these
        // values later is already ordered behind the work just submitted.
        for (const rhi::QueueSemaphoreSignal &signal : desc.signals) {
            if (u64 *value = semaphore_values_.find(signal.semaphore); value != nullptr) {
                *value = signal.value;
            }
        }
        if (desc.fence.value != 0) {
            if (FenceState *state = fences_.find(desc.fence); state != nullptr) {
                // The fence becomes signalled when this submission's work actually finishes on the
                // GPU, not the instant it is attached here -- that distinction is exactly what lets
                // wait_fences answer "not yet" instead of always being trivially true.
                WGPUQueueWorkDoneCallbackInfo info{};
                info.mode = WGPUCallbackMode_WaitAnyOnly;
                info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *, void *) {};
                state->pending_future = wgpuQueueOnSubmittedWorkDone(queue_, info);
                state->has_pending = true;
                state->signaled = false;
            }
        }
        return {};
    }

    /// Creates a semaphore.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::SemaphoreHandle> WebGpuDevice::create_semaphore(const rhi::SemaphoreDesc &desc) {
        return semaphore_values_.insert(u64{desc.initial_value});
    }

    /// Destroys a semaphore.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_semaphore(rhi::SemaphoreHandle handle) noexcept {
        semaphore_values_.erase(handle, [](u64 &) {});
    }

    /// Returns a timeline semaphore's current value.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<u64> WebGpuDevice::semaphore_value(rhi::SemaphoreHandle handle) const {
        const u64 *value = semaphore_values_.find(handle);
        if (value == nullptr) {
            return std::unexpected(webgpu_error("semaphore_value", "unknown semaphore handle"));
        }
        return *value;
    }

    /// Waits for a semaphore to reach `value`.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param value `value` value used by the operation.
    /// @param timeout_ns `timeout_ns` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns) {
        (void)timeout_ns;
        u64 *current = semaphore_values_.find(handle);
        if (current == nullptr) {
            return std::unexpected(webgpu_error("wait_semaphore", "unknown semaphore handle"));
        }
        if (*current >= value) {
            return {};
        }
        // The value has not been signalled yet, which on an in-order queue can only mean the work
        // that would signal it has not been submitted. Draining the queue is the strongest thing
        // available and is never wrong, only conservative.
        wait_idle();
        *current = value;
        return {};
    }

    /// Signals a semaphore from the host.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param value `value` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::signal_semaphore(rhi::SemaphoreHandle handle, u64 value) {
        u64 *current = semaphore_values_.find(handle);
        if (current == nullptr) {
            return std::unexpected(webgpu_error("signal_semaphore", "unknown semaphore handle"));
        }
        *current = value;
        return {};
    }

    /// Creates a fence.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::FenceHandle> WebGpuDevice::create_fence(const rhi::FenceDesc &desc) {
        return fences_.insert(FenceState{.signaled = desc.signaled});
    }

    /// Destroys a fence.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_fence(rhi::FenceHandle handle) noexcept {
        fences_.erase(handle, [](FenceState &) {});
    }

    /// Waits for fences to be signalled.
    ///
    /// `timeout_ns == 0` is a real, non-blocking poll here: it is forwarded straight to
    /// `wgpuInstanceWaitAny`, which -- per the Emscripten WebGPU port's own implementation -- only
    /// goes through Asyncify suspension when `timeoutNS > 0`; a zero timeout is answered
    /// synchronously from already-known future state. That distinction is what makes it safe to poll
    /// this once per frame from inside a `requestAnimationFrame` callback without the suspend/resume
    /// nesting that an unconditional blocking wait caused there.
    ///
    /// @param fences `fences` value used by the operation.
    /// @param wait_all `wait_all` value used by the operation.
    /// @param timeout_ns `timeout_ns` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<bool> WebGpuDevice::wait_fences(span<const rhi::FenceHandle> fences, bool wait_all,
                                                     u64 timeout_ns) {
        if (fences.empty()) {
            return true;
        }

        std::vector<WGPUFutureWaitInfo> waits;
        std::vector<FenceState *> pending_states;
        waits.reserve(fences.size());
        pending_states.reserve(fences.size());

        std::size_t signaled_count = 0;
        for (rhi::FenceHandle handle : fences) {
            FenceState *state = fences_.find(handle);
            if (state == nullptr) {
                return std::unexpected(webgpu_error("wait_fences", "unknown fence handle"));
            }
            if (state->signaled) {
                ++signaled_count;
            } else if (state->has_pending) {
                waits.push_back(WGPUFutureWaitInfo{.future = state->pending_future, .completed = 0});
                pending_states.push_back(state);
            }
            // Neither signalled nor pending means nothing has been submitted against this fence
            // since it was created/reset; it can only become signalled by a future submit(), so
            // "not ready" is already the correct answer without waiting on anything.
        }

        if (!waits.empty()) {
            // Every real call site in this codebase waits on exactly one fence at a time, so the only
            // timeouts that matter in practice are 0 (poll) and wait_forever (block). A single
            // wgpuInstanceWaitAny call already gives exact "any" semantics for any timeout; for "all"
            // with more than one still-pending fence and an unbounded timeout, loop until every one
            // completes rather than stopping at the first.
            do {
                const WGPUWaitStatus status =
                    wgpuInstanceWaitAny(instance_, waits.size(), waits.data(), timeout_ns);
                std::size_t remaining = 0;
                for (std::size_t i = 0; i < waits.size(); ++i) {
                    if (waits[i].completed) {
                        pending_states[i]->signaled = true;
                        pending_states[i]->has_pending = false;
                        ++signaled_count;
                    } else {
                        waits[remaining] = waits[i];
                        pending_states[remaining] = pending_states[i];
                        ++remaining;
                    }
                }
                waits.resize(remaining);
                pending_states.resize(remaining);
                if (status != WGPUWaitStatus_Success) {
                    break;
                }
            } while (wait_all && !waits.empty() && timeout_ns == rhi::wait_forever);
        }

        return wait_all ? signaled_count == fences.size() : signaled_count > 0;
    }

    /// Resets fences to the unsignalled state.
    ///
    /// @param fences `fences` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::reset_fences(span<const rhi::FenceHandle> fences) {
        for (rhi::FenceHandle handle : fences) {
            if (FenceState *state = fences_.find(handle); state != nullptr) {
                *state = FenceState{};
            }
        }
        return {};
    }

    /// Destroys a command buffer.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept {
        command_buffers_.erase(handle, [](WGPUCommandBuffer &buffer) { wgpuCommandBufferRelease(buffer); });
    }

    /// Takes ownership of a finished Dawn command buffer.
    ///
    /// @param command_buffer `command_buffer` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    rhi::CommandBufferHandle WebGpuDevice::store_command_buffer(WGPUCommandBuffer command_buffer) {
        return command_buffers_.insert(std::move(command_buffer));
    }

    /// Destroys a render bundle.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept {
        render_bundles_.erase(handle, [](WGPURenderBundle &bundle) { wgpuRenderBundleRelease(bundle); });
    }

    /// Takes ownership of a finished Dawn render bundle.
    ///
    /// @param bundle `bundle` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    rhi::RenderBundleHandle WebGpuDevice::store_render_bundle(WGPURenderBundle bundle) {
        return render_bundles_.insert(std::move(bundle));
    }

    /// Resolves a render bundle handle to the Dawn bundle behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPURenderBundle WebGpuDevice::lookup_render_bundle(rhi::RenderBundleHandle handle) noexcept {
        WGPURenderBundle *bundle = render_bundles_.find(handle);
        return bundle != nullptr ? *bundle : nullptr;
    }

} // namespace SFT::Core::WebGpu
