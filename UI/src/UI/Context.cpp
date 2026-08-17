#include <UI/src/UI/Context.hpp>


namespace SFT::UI {

    usize OutlineCacheKeyHash::operator()(const OutlineCacheKey &key) const noexcept {
        u64 hashed = key.font_id;
        hashed ^= static_cast<u64>(key.glyph_id) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
        return static_cast<usize>(hashed);
    }

    Core::Extent2D FrameSnapshot::viewport_extent() const noexcept {
        return Core::Extent2D{full_viewport_scissor_.width, full_viewport_scissor_.height};
    }

    std::span<const QuadDraw> FrameSnapshot::quads() const noexcept { return quads_; }

    void Context::set_scroll_settings(const ScrollSettings &settings) noexcept { scroll_settings_ = settings; }

    const ScrollSettings &Context::scroll_settings() const noexcept { return scroll_settings_; }

    void Context::set_cursor_management_enabled(bool enabled) noexcept { cursor_management_enabled_ = enabled; }

    bool Context::cursor_management_enabled() const noexcept { return cursor_management_enabled_; }

    CursorIcon Context::desired_cursor() const noexcept { return desired_cursor_; }

    glm::vec2 Context::pointer_position() const noexcept { return pointer_position_; }

    bool Context::pointer_is_down() const noexcept { return pointer_down_; }

    bool Context::pointer_pressed_this_frame() const noexcept { return pointer_pressed_this_frame_; }

    bool Context::pointer_released_this_frame() const noexcept { return pointer_released_this_frame_; }

    bool Context::pointer_cancelled_this_frame() const noexcept { return pointer_cancelled_this_frame_; }

    bool Context::pointer_captured() const noexcept { return !pointer_capture_id_.empty(); }

    bool Context::focused() const noexcept { return !focused_id_.empty(); }

    FrameSnapshot Context::finish_frame(glm::vec2) { return finish_frame(); }

} // namespace SFT::UI


namespace SFT::UI {

    ElementScope::ElementScope(Clay_Context *context, vector<i32> *z_stack) noexcept
    : context_(context), z_stack_(z_stack) {}

} // namespace SFT::UI

