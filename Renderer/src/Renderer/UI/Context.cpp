#include <Renderer/UI/Context.hpp>


namespace SFT::UI {

    /// Invokes the callable behavior provided by `UI`.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize OutlineCacheKeyHash::operator()(const OutlineCacheKey &key) const noexcept {
        u64 hashed = key.font_id;
        hashed ^= static_cast<u64>(key.glyph_id) + 0x9e3779b97f4a7c15ULL + (hashed << 6) + (hashed >> 2);
        return static_cast<usize>(hashed);
    }

    /// Returns the current or globally available viewport extent value.
    ///
    /// @return Returns the current viewport extent value.
    /// @note This function does not throw exceptions.
    Core::Extent2D FrameSnapshot::viewport_extent() const noexcept {
        return Core::Extent2D{full_viewport_scissor_.width, full_viewport_scissor_.height};
    }

    /// Returns the current or globally available quads value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const QuadDraw> FrameSnapshot::quads() const noexcept { return quads_; }

    /// Returns the current or globally available strokes value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const StrokeDraw> FrameSnapshot::strokes() const noexcept { return strokes_; }

    /// Returns the current or globally available fills value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const FillQuadDraw> FrameSnapshot::fills() const noexcept { return fills_; }

    /// Returns the current or globally available sectors value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const SectorDraw> FrameSnapshot::sectors() const noexcept { return sectors_; }

    /// Returns the current or globally available custom strokes value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const CustomStrokeDraw> FrameSnapshot::custom_strokes() const noexcept { return custom_strokes_; }

    /// Sets the scroll settings for this `UI`.
    ///
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::set_scroll_settings(const ScrollSettings &settings) noexcept { scroll_settings_ = settings; }

    /// Scrolls settings using the supplied arguments and current state.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const ScrollSettings &Context::scroll_settings() const noexcept { return scroll_settings_; }

    /// Sets the cursor management enabled for this `UI`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Context::set_cursor_management_enabled(bool enabled) noexcept { cursor_management_enabled_ = enabled; }

    /// Returns the current or globally available cursor management enabled value.
    ///
    /// @return Returns the current cursor management enabled value.
    /// @note This function does not throw exceptions.
    bool Context::cursor_management_enabled() const noexcept { return cursor_management_enabled_; }

    /// Returns the current or globally available desired cursor value.
    ///
    /// @return Returns the current desired cursor value.
    /// @note This function does not throw exceptions.
    CursorIcon Context::desired_cursor() const noexcept { return desired_cursor_; }

    /// Returns the current or globally available pointer position value.
    ///
    /// @return Returns the current pointer position value.
    /// @note This function does not throw exceptions.
    glm::vec2 Context::pointer_position() const noexcept { return pointer_position_; }

    /// Reports whether pointer is down.
    ///
    /// @return Returns the current pointer is down value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_is_down() const noexcept { return pointer_down_; }

    /// Returns the current or globally available pointer pressed this frame value.
    ///
    /// @return Returns the current pointer pressed this frame value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_pressed_this_frame() const noexcept { return pointer_pressed_this_frame_; }

    /// Returns the current or globally available pointer released this frame value.
    ///
    /// @return Returns the current pointer released this frame value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_released_this_frame() const noexcept { return pointer_released_this_frame_; }

    /// Returns the current or globally available pointer cancelled this frame value.
    ///
    /// @return Returns the current pointer cancelled this frame value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_cancelled_this_frame() const noexcept { return pointer_cancelled_this_frame_; }

    /// Returns the current or globally available pointer captured value.
    ///
    /// @return Returns the current pointer captured value.
    /// @note This function does not throw exceptions.
    bool Context::pointer_captured() const noexcept { return !pointer_capture_id_.empty(); }

    /// Returns the current or globally available focused value.
    ///
    /// @return Returns the current focused value.
    /// @note This function does not throw exceptions.
    bool Context::focused() const noexcept { return !focused_id_.empty(); }

    /// Performs the finish frame operation for `UI` using the supplied arguments.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    FrameSnapshot Context::finish_frame(glm::vec2) { return finish_frame(); }

} // namespace SFT::UI


namespace SFT::UI {

    /// Performs the element scope operation for `UI` using the supplied arguments.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param z_stack `z_stack` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    ElementScope::ElementScope(Clay_Context *context, vector<i32> *z_stack) noexcept
    : context_(context), z_stack_(z_stack) {}

} // namespace SFT::UI

