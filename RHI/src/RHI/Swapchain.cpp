#include <RHI/Swapchain.hpp>

namespace SFT::RHI {

    /// Selects present mode that best satisfies the supplied requirements.
    ///
    /// @param supported `supported` value used by the operation.
    /// @param strategy `strategy` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] PresentMode choose_present_mode(std::span<const PresentMode> supported,
                                                          PresentStrategy strategy) noexcept {
        for (PresentMode candidate : present_mode_preference(strategy)) {
            if (std::ranges::contains(supported, candidate)) {
                return candidate;
            }
        }
        return PresentMode::Fifo;
    }

} // namespace SFT::RHI

