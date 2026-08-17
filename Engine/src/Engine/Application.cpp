#include <Engine/src/Engine/Application.hpp>


namespace SFT::Engine {

    void Application::LiveResizeState::publish(Platform::Windowing::WindowExtent extent) noexcept {
        const u64 packed = (static_cast<u64>(extent.x) << 32U) | static_cast<u64>(extent.y);
        pending_extent.store(packed, std::memory_order_release);
    }

    optional<Platform::Windowing::WindowExtent> Application::LiveResizeState::consume() noexcept {
        const u64 packed = pending_extent.exchange(0, std::memory_order_acq_rel);
        if (packed == 0) {
            return std::nullopt;
        }
        return Platform::Windowing::WindowExtent{
            static_cast<u32>(packed >> 32U),
            static_cast<u32>(packed & 0xFFFFFFFFU),
        };
    }

} // namespace SFT::Engine

