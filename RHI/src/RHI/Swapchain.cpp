#include <RHI/src/RHI/Swapchain.hpp>

namespace SFT::RHI {

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

