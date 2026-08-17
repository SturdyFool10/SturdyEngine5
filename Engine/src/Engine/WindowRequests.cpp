#include <Engine/src/Engine/WindowRequests.hpp>


namespace SFT::Engine {

    /// Returns the current or globally available view value.
    ///
    /// @return Returns the current view value.
    /// @note This function does not throw exceptions.
    Platform::Windowing::WindowConfig OwnedWindowConfig::view() const noexcept {
        Platform::Windowing::WindowConfig result = config;
        result.title = title.c_str();
        return result;
    }

    /// Spawns the supplied asynchronous work.
    ///
    /// @param config Configuration values controlling the operation.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WindowRequestId WindowRequests::spawn(const Platform::Windowing::WindowConfig &config,
                                        Platform::Windowing::WindowFactory factory) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(SpawnWindowRequest{id, OwnedWindowConfig{config}, factory});
        return id;
    }

    /// Closes the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WindowRequestId WindowRequests::close(Platform::Windowing::WindowId window) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(CloseWindowRequest{id, window});
        return id;
    }

    /// Recreates primary window using the supplied arguments and current state.
    ///
    /// @param config Configuration values controlling the operation.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WindowRequestId WindowRequests::recreate_primary_window(const Platform::Windowing::WindowConfig &config,
                                                           Platform::Windowing::WindowFactory factory) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(RecreatePrimaryWindowRequest{id, OwnedWindowConfig{config}, factory});
        return id;
    }

    /// Sets the cursor icon for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param icon `icon` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_cursor_icon(Platform::Windowing::WindowId window, Platform::Windowing::CursorIcon icon) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetCursorIconRequest{window, icon});
    }

    /// Sets the fullscreen for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_fullscreen(Platform::Windowing::WindowId window, Platform::Windowing::WindowMode mode) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetFullscreenRequest{window, mode});
    }

    /// Sets the decorated for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param decorated `decorated` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_decorated(Platform::Windowing::WindowId window, bool decorated) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetDecoratedRequest{window, decorated});
    }

    /// Sets the transparent for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param transparent `transparent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_transparent(Platform::Windowing::WindowId window, bool transparent) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTransparentRequest{window, transparent});
    }

    /// Sets the blur for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_blur(Platform::Windowing::WindowId window, Platform::Windowing::WindowEffectKind kind, bool enabled) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetBlurRequest{window, kind, enabled});
    }

    /// Sets the text input area for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param area `area` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_text_input_area(Platform::Windowing::WindowId window, Platform::Windowing::TextInputArea area) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTextInputAreaRequest{window, area});
    }

    /// Sets the text input active for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param active `active` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_text_input_active(Platform::Windowing::WindowId window, bool active) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTextInputActiveRequest{window, active});
    }

    /// Drains the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @return Returns the current drain value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<WindowRequest> WindowRequests::drain() {
        auto guard = state_.lock();
        vector<WindowRequest> result;
        result.swap(guard->pending);
        return result;
    }

    /// Performs the complete operation for `Engine` using the supplied arguments.
    ///
    /// @param completion `completion` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::complete(WindowRequestCompletion completion) {
        auto guard = state_.lock();
        guard->completions.push_back(std::move(completion));
    }

    /// Returns the current or globally available take completions value.
    ///
    /// @return Returns the current take completions value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<WindowRequestCompletion> WindowRequests::take_completions() {
        auto guard = state_.lock();
        vector<WindowRequestCompletion> result;
        result.swap(guard->completions);
        return result;
    }

    /// Reports whether this `Engine` has pending.
    ///
    /// @return Returns the current has pending value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool WindowRequests::has_pending() const {
        auto guard = state_.lock();
        return !guard->pending.empty();
    }

} // namespace SFT::Engine


namespace SFT::Engine {

    /// Converts the `Engine` to `bool`.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    WindowRequestId::operator bool() const noexcept { return value != 0; }

    /// Performs the owned window config operation for `Engine` using the supplied arguments.
    ///
    /// @note This function does not throw exceptions.
    OwnedWindowConfig::OwnedWindowConfig() noexcept { config.title = nullptr; }

    /// Performs the owned window config operation for `Engine` using the supplied arguments.
    ///
    /// @param source Source value or resource.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    OwnedWindowConfig::OwnedWindowConfig(const Platform::Windowing::WindowConfig &source)
        : title(source.title != nullptr ? source.title : ""), config(source) {
        config.title = nullptr;
    }

} // namespace SFT::Engine

