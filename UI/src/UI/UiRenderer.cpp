#include <UI/src/UI/UiRenderer.hpp>


namespace SFT::UI {

    /// Reads the requested data from the associated source.
    ///
    /// @return Returns the current ready value.
    /// @note This function does not throw exceptions.
    bool UiRenderer::ready() const noexcept { return ready_; }

    /// Returns the current or globally available generation value.
    ///
    /// @return Returns the current generation value.
    /// @note This function does not throw exceptions.
    u64 UiRenderer::generation() const noexcept { return generation_.load(std::memory_order_acquire); }

} // namespace SFT::UI


namespace SFT::UI {

    /// Performs the UI renderer operation for `UI` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function does not throw exceptions.
    UiRenderer::UiRenderer(UiRenderer &&other) noexcept { *this = std::move(other); }

} // namespace SFT::UI

