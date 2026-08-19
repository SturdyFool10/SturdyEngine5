#pragma once

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <string>

#include <Async/Async.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {


    class PresentationCoordinator {
      public:
        /// Constructs a `PresentationCoordinator` from the supplied initialization values.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit PresentationCoordinator(std::string name);

        /// Disables this construction form for `PresentationCoordinator`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        PresentationCoordinator(const PresentationCoordinator &) = delete;
        /// Assigns a new value to this `PresentationCoordinator`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        PresentationCoordinator &operator=(const PresentationCoordinator &) = delete;
        /// Disables this construction form for `PresentationCoordinator`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        PresentationCoordinator(PresentationCoordinator &&) = delete;
        /// Assigns a new value to this `PresentationCoordinator`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        PresentationCoordinator &operator=(PresentationCoordinator &&) = delete;


        /// Performs the enqueue operation for `PresentationCoordinator` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param lock_wait_ms `lock_wait_ms` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Async::TaskHandle<RHI::RhiExpected<RHI::PresentOutcome>> enqueue(
            RHI::RhiDevice *device, const RHI::PresentDesc &desc, f64 *lock_wait_ms);


        /// Returns the current or globally available queue depth value.
        ///
        /// @return Returns the current queue depth value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 queue_depth() const noexcept;

        /// Returns the current or globally available name value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::string &name() const noexcept;

      private:
        std::string name_;
        Async::DedicatedThread thread_;
        std::atomic<u32> queue_depth_{0};
    };

} // namespace SFT::Renderer
