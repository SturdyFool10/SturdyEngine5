#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <vector>

namespace SFT::Core::WebGpu {

    // WebGPU's execution model is why this file looks the way it does. There is one queue, and work
    // submitted to it is guaranteed to complete in submission order. There are no semaphores, no
    // fences, and no barriers in the API at all — the implementation infers every dependency from
    // resource usage and inserts whatever the underlying driver needs.
    //
    // So the RHI's synchronisation primitives are *satisfied* here rather than emulated: a caller
    // that waits on a semaphore or a fence in the right order gets the ordering it asked for,
    // because the queue already provides it. Rather than return "unsupported" and make the whole
    // backend unusable, semaphores and fences are modelled as plain counters that the submit path
    // advances, which is exactly what they mean on a single in-order queue. Timeline waits that
    // would block for work not yet submitted are the one case that cannot be honoured, and those
    // fall through to a device wait.

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
            if (bool *signaled = fences_.find(desc.fence); signaled != nullptr) {
                *signaled = true;
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
        return fences_.insert(bool{desc.signaled});
    }

    /// Destroys a fence.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_fence(rhi::FenceHandle handle) noexcept {
        fences_.erase(handle, [](bool &) {});
    }

    /// Waits for fences to be signalled.
    ///
    /// @param fences `fences` value used by the operation.
    /// @param wait_all `wait_all` value used by the operation.
    /// @param timeout_ns `timeout_ns` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<bool> WebGpuDevice::wait_fences(span<const rhi::FenceHandle> fences, bool wait_all,
                                                     u64 timeout_ns) {
        (void)wait_all;
        (void)timeout_ns;
        if (fences.empty()) {
            return true;
        }
        // A fence is signalled by the submission it was attached to. Waiting for one means waiting
        // for that submission, and the only wait WebGPU offers is for all outstanding work.
        wait_idle();
        for (rhi::FenceHandle handle : fences) {
            if (bool *signaled = fences_.find(handle); signaled != nullptr) {
                *signaled = true;
            }
        }
        return true;
    }

    /// Resets fences to the unsignalled state.
    ///
    /// @param fences `fences` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::reset_fences(span<const rhi::FenceHandle> fences) {
        for (rhi::FenceHandle handle : fences) {
            if (bool *signaled = fences_.find(handle); signaled != nullptr) {
                *signaled = false;
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
