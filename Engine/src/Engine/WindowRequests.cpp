#include <Engine/src/Engine/WindowRequests.hpp>


namespace SFT::Engine {

    Platform::Windowing::WindowConfig OwnedWindowConfig::view() const noexcept {
        Platform::Windowing::WindowConfig result = config;
        result.title = title.c_str();
        return result;
    }

    WindowRequestId WindowRequests::spawn(const Platform::Windowing::WindowConfig &config,
                                        Platform::Windowing::WindowFactory factory) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(SpawnWindowRequest{id, OwnedWindowConfig{config}, factory});
        return id;
    }

    WindowRequestId WindowRequests::close(Platform::Windowing::WindowId window) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(CloseWindowRequest{id, window});
        return id;
    }

    WindowRequestId WindowRequests::recreate_primary_window(const Platform::Windowing::WindowConfig &config,
                                                           Platform::Windowing::WindowFactory factory) {
        auto guard = state_.lock();
        const WindowRequestId id{guard->next_id++};
        guard->pending.emplace_back(RecreatePrimaryWindowRequest{id, OwnedWindowConfig{config}, factory});
        return id;
    }

    void WindowRequests::set_cursor_icon(Platform::Windowing::WindowId window, Platform::Windowing::CursorIcon icon) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetCursorIconRequest{window, icon});
    }

    void WindowRequests::set_fullscreen(Platform::Windowing::WindowId window, Platform::Windowing::WindowMode mode) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetFullscreenRequest{window, mode});
    }

    void WindowRequests::set_decorated(Platform::Windowing::WindowId window, bool decorated) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetDecoratedRequest{window, decorated});
    }

    void WindowRequests::set_transparent(Platform::Windowing::WindowId window, bool transparent) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTransparentRequest{window, transparent});
    }

    void WindowRequests::set_blur(Platform::Windowing::WindowId window, Platform::Windowing::WindowEffectKind kind, bool enabled) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetBlurRequest{window, kind, enabled});
    }

    void WindowRequests::set_text_input_area(Platform::Windowing::WindowId window, Platform::Windowing::TextInputArea area) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTextInputAreaRequest{window, area});
    }

    void WindowRequests::set_text_input_active(Platform::Windowing::WindowId window, bool active) {
        auto guard = state_.lock();
        guard->pending.emplace_back(SetTextInputActiveRequest{window, active});
    }

    vector<WindowRequest> WindowRequests::drain() {
        auto guard = state_.lock();
        vector<WindowRequest> result;
        result.swap(guard->pending);
        return result;
    }

    void WindowRequests::complete(WindowRequestCompletion completion) {
        auto guard = state_.lock();
        guard->completions.push_back(std::move(completion));
    }

    vector<WindowRequestCompletion> WindowRequests::take_completions() {
        auto guard = state_.lock();
        vector<WindowRequestCompletion> result;
        result.swap(guard->completions);
        return result;
    }

    bool WindowRequests::has_pending() const {
        auto guard = state_.lock();
        return !guard->pending.empty();
    }

} // namespace SFT::Engine


namespace SFT::Engine {

    WindowRequestId::operator bool() const noexcept { return value != 0; }

    OwnedWindowConfig::OwnedWindowConfig() noexcept { config.title = nullptr; }

    OwnedWindowConfig::OwnedWindowConfig(const Platform::Windowing::WindowConfig &source)
        : title(source.title != nullptr ? source.title : ""), config(source) {
        config.title = nullptr;
    }

} // namespace SFT::Engine

