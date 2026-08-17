#include <UI/src/UI/UiRenderer.hpp>


namespace SFT::UI {

    bool UiRenderer::ready() const noexcept { return ready_; }

    u64 UiRenderer::generation() const noexcept { return generation_.load(std::memory_order_acquire); }

} // namespace SFT::UI


namespace SFT::UI {

    UiRenderer::UiRenderer(UiRenderer &&other) noexcept { *this = std::move(other); }

} // namespace SFT::UI

