#include <Engine/src/Engine/EcsEvents.hpp>


namespace SFT::Engine {

    bool KeyboardEvent::pressed() const noexcept { return action == ButtonAction::Pressed; }

    bool KeyboardEvent::released() const noexcept { return action == ButtonAction::Released; }

    void PlatformEventInbox::push(Platform::Windowing::WindowId window, Platform::Windowing::WindowEvent event) {
        pending_.push_back(WindowEvent{.window = window, .event = std::move(event)});
    }

    std::vector<WindowEvent> PlatformEventInbox::drain() noexcept {
        std::vector<WindowEvent> result;
        result.swap(pending_);




        pending_.reserve(result.capacity());
        return result;
    }

    bool PlatformEventInbox::empty() const noexcept { return pending_.empty(); }

} // namespace SFT::Engine

