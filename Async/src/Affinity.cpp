#include <Async/src/Affinity.hpp>


namespace SFT::Async {

    /// Returns the current or globally available name value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const std::string &DedicatedThread::name() const noexcept { return name_; }

} // namespace SFT::Async

