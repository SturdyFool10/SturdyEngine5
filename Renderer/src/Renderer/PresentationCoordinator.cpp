#include "PresentationCoordinator.hpp"

namespace SFT::Renderer {

    PresentationCoordinator::PresentationCoordinator(std::string name)
        : name_(name), thread_(std::move(name)) {}

    Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>> PresentationCoordinator::enqueue(
        RHI::RhiDevice *device, const RHI::PresentDesc &desc, f64 *lock_wait_ms) {
        queue_depth_.fetch_add(1, std::memory_order_acq_rel);
        return thread_.run([this, device, desc, lock_wait_ms]() -> RHI::RhiExpected<RHI::PresentOutcome> {
            queue_depth_.fetch_sub(1, std::memory_order_acq_rel);
            return device->present(desc, lock_wait_ms);
        });
    }

} // namespace SFT::Renderer
