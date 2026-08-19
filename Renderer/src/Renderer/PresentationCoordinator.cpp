#include <Renderer/PresentationCoordinator.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param name Name used to identify or label the target.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    PresentationCoordinator::PresentationCoordinator(std::string name)
        : name_(name), thread_(std::move(name)) {}

    /// Performs the enqueue operation for `Renderer` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param lock_wait_ms `lock_wait_ms` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>> PresentationCoordinator::enqueue(
        RHI::RhiDevice *device, const RHI::PresentDesc &desc, f64 *lock_wait_ms) {
        ZoneScopedN("PresentationCoordinator::PresentationCoordinator");
        queue_depth_.fetch_add(1, std::memory_order_acq_rel);
        return thread_.run([this, device, desc, lock_wait_ms]() -> RHI::RhiExpected<RHI::PresentOutcome> {
            queue_depth_.fetch_sub(1, std::memory_order_acq_rel);
            return device->present(desc, lock_wait_ms);
        });
    }

} // namespace SFT::Renderer

namespace SFT::Renderer {

    /// Returns the current or globally available queue depth value.
    ///
    /// @return Returns the current queue depth value.
    /// @note This function does not throw exceptions.
    u32 PresentationCoordinator::queue_depth() const noexcept { return queue_depth_.load(std::memory_order_acquire); }

    /// Returns the current or globally available name value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const std::string &PresentationCoordinator::name() const noexcept { return name_; }

} // namespace SFT::Renderer

