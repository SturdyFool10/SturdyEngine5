#include <Engine/EcsEvents.hpp>


namespace SFT::Engine {

    /// Returns the current or globally available pressed value.
    ///
    /// @return Returns the current pressed value.
    /// @note This function does not throw exceptions.
    bool KeyboardEvent::pressed() const noexcept { return action == ButtonAction::Pressed; }

    /// Returns the current or globally available released value.
    ///
    /// @return Returns the current released value.
    /// @note This function does not throw exceptions.
    bool KeyboardEvent::released() const noexcept { return action == ButtonAction::Released; }

    /// Adds the supplied value to the end or work queue.
    ///
    /// @param window Window used or affected by the operation.
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void PlatformEventInbox::push(WindowManager::WindowId window, WindowManager::WindowEvent event) {
        pending_.push_back(WindowEvent{.window = window, .event = std::move(event)});
    }

    /// Drains the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @return Returns the current drain value.
    /// @note This function does not throw exceptions.
    std::vector<WindowEvent> PlatformEventInbox::drain() noexcept {
        std::vector<WindowEvent> result;
        result.swap(pending_);


        pending_.reserve(result.capacity());
        return result;
    }

    /// Reports whether this `Engine` contains no elements or payload.
    ///
    /// @return Returns the current empty value.
    /// @note This function does not throw exceptions.
    bool PlatformEventInbox::empty() const noexcept { return pending_.empty(); }

} // namespace SFT::Engine

