#include <Engine/WindowRequests.hpp>


namespace SFT::Engine {

    /// Returns the current or globally available view value.
    ///
    /// @return Returns the current view value.
    /// @note This function does not throw exceptions.
    WindowManager::WindowConfig OwnedWindowConfig::view() const noexcept {
        WindowManager::WindowConfig result = config;
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
    WindowRequestId WindowRequests::spawn(const WindowManager::WindowConfig &config,
                                        WindowManager::WindowFactory factory) {
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
    WindowRequestId WindowRequests::close(WindowManager::WindowId window) {
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
    WindowRequestId WindowRequests::recreate_primary_window(const WindowManager::WindowConfig &config,
                                                           WindowManager::WindowFactory factory) {
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
    void WindowRequests::set_cursor_icon(WindowManager::WindowId window, WindowManager::CursorIcon icon) {
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
    void WindowRequests::set_fullscreen(WindowManager::WindowId window, WindowManager::WindowMode mode) {
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
    void WindowRequests::set_decorated(WindowManager::WindowId window, bool decorated) {
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
    void WindowRequests::set_transparent(WindowManager::WindowId window, bool transparent) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTransparentRequest{window, transparent});
    }

    /// Sets the relative mouse mode for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_relative_mouse_mode(WindowManager::WindowId window, bool enabled) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetRelativeMouseModeRequest{window, enabled});
    }

    /// Sets the mouse locked for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param locked `locked` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_mouse_locked(WindowManager::WindowId window, bool locked) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetMouseLockedRequest{window, locked});
    }

    /// Sets the cursor grabbed for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param grabbed `grabbed` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_cursor_grabbed(WindowManager::WindowId window, bool grabbed) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetCursorGrabbedRequest{window, grabbed});
    }

    /// Sets the blur for this `Engine`.
    ///
    /// @param window Window used or affected by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WindowRequests::set_blur(WindowManager::WindowId window, WindowManager::WindowEffectKind kind, bool enabled) {
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
    void WindowRequests::set_text_input_area(WindowManager::WindowId window, WindowManager::TextInputArea area) {
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
    void WindowRequests::set_text_input_active(WindowManager::WindowId window, bool active) {
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
    OwnedWindowConfig::OwnedWindowConfig(const WindowManager::WindowConfig &source)
        : title(source.title != nullptr ? source.title : ""), config(source) {
        config.title = nullptr;
    }

} // namespace SFT::Engine

