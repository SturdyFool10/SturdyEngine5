#pragma once

#include <Foundation/src/Foundation.hpp>

#include <atomic>
#include <string>

#include <Async/src/Async.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    /// Centralizes vkQueuePresentKHR issuance (via RHI::RhiDevice::present) for every window whose
    /// swapchain presents through the same native queue, instead of each window owning its own
    /// present thread (the design this replaces -- see Renderer's graphics_presentation_coordinator_/
    /// compute_presentation_coordinator_). One coordinator instance per distinct presentation queue
    /// class (Graphics/Compute); both are constructed unconditionally at Renderer construction (cheap
    /// -- an idle DedicatedThread costs nothing) so there is never a lazy-create race between two
    /// windows' render threads enqueuing onto the same coordinator concurrently.
    ///
    /// Runs on a single dedicated thread (Async::DedicatedThread), so every present request for a
    /// given native queue is naturally ordered FIFO with no possibility of two callers racing
    /// VulkanQueue::submission_lock_ from independent per-window threads with no ordering guarantee
    /// relative to each other, the way the earlier per-window present_thread design allowed. Requests
    /// enqueued close together in time are processed back-to-back with no artificial gap
    /// (DedicatedThread's own worker loop already does this -- it re-checks its queue immediately
    /// after finishing a task, before ever going idle), which is the "batch presentations ready
    /// within a small scheduling interval" behavior called for. True single-call multi-swapchain
    /// batching (VkPresentInfoKHR's pSwapchains array can hold more than one swapchain) is
    /// deliberately NOT implemented here: it would require restructuring RHI::RhiDevice::present's
    /// signature to accept a span of requests for a call that isn't itself the bottleneck (the
    /// GPU-completion wait inside vkQueuePresentKHR is -- see the original present-thread decoupling
    /// work this coordinator supersedes), and batching is optional ("may batch") in the design this
    /// implements, not a hard requirement.
    class PresentationCoordinator {
      public:
        explicit PresentationCoordinator(std::string name);

        PresentationCoordinator(const PresentationCoordinator &) = delete;
        PresentationCoordinator &operator=(const PresentationCoordinator &) = delete;
        PresentationCoordinator(PresentationCoordinator &&) = delete;
        PresentationCoordinator &operator=(PresentationCoordinator &&) = delete;

        /// Enqueues one present request, returning a handle the caller drains later (same contract
        /// RHI::RhiDevice::present itself has: device->present(desc, lock_wait_ms) is called exactly
        /// once, on this coordinator's own thread, in FIFO order relative to every other request this
        /// coordinator has queued -- including ones enqueued by other windows). `device` and
        /// `lock_wait_ms` must both outlive the call (same pointer-stability contract the
        /// uncoordinated device->present() already required of its callers).
        [[nodiscard]] Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>> enqueue(
            RHI::RhiDevice *device, const RHI::PresentDesc &desc, f64 *lock_wait_ms);

        /// Requests accepted but not yet issued to the driver. Polled by Renderer for Tracy plotting
        /// (once per resolution point, not per-frame per window -- see the coordinator-owned-counter
        /// guidance for presentation-coordinator queue depth).
        [[nodiscard]] u32 queue_depth() const noexcept;

        [[nodiscard]] const std::string &name() const noexcept;

      private:
        std::string name_;
        Async::DedicatedThread thread_;
        std::atomic<u32> queue_depth_{0};
    };

} // namespace SFT::Renderer
