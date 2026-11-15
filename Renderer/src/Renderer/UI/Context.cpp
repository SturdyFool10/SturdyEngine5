#include <algorithm>
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

    std::vector<std::shared_ptr<FrameSnapshot>> FrameSnapshot::split_layers(std::shared_ptr<FrameSnapshot> base) {
        if (!base) {
            return {};
        }
        // Hoisted groups: top-level, with a layer. Their paint-order ranges cannot overlap, so a draw belongs to at most one.
        std::vector<usize> hoisted;
        std::vector<u8> layer_values;
        for (usize i = 0; i < base->effect_groups_.size(); ++i) {
            const EffectGroupDraw &group = base->effect_groups_[i];
            if (group.parent < 0 && group.effect != nullptr && group.effect->layer > 0) {
                hoisted.push_back(i);
                if (std::ranges::find(layer_values, group.effect->layer) == layer_values.end()) {
                    layer_values.push_back(group.effect->layer);
                }
            }
        }
        if (hoisted.empty()) {
            return {std::move(base)};
        }
        std::ranges::sort(layer_values);
        std::ranges::sort(hoisted, [&](usize a, usize b) { return base->effect_groups_[a].paint.order < base->effect_groups_[b].paint.order; });

        // Index 0 is the lowest pass; index k + 1 is layer_values[k].
        std::vector<std::shared_ptr<FrameSnapshot>> pieces;
        for (usize k = 0; k <= layer_values.size(); ++k) {
            auto piece = std::make_shared<FrameSnapshot>();
            piece->full_viewport_scissor_ = base->full_viewport_scissor_;
            piece->storage_owner_ = base;
            pieces.push_back(std::move(piece));
        }
        const auto piece_for_layer = [&](u8 layer) {
            return 1 + static_cast<usize>(std::ranges::find(layer_values, layer) - layer_values.begin());
        };
        const auto piece_of_order = [&](u32 order) -> usize {
            auto it = std::upper_bound(hoisted.begin(), hoisted.end(), order,
                                       [&](u32 value, usize group) { return value < base->effect_groups_[group].paint.order; });
            if (it == hoisted.begin()) {
                return 0;
            }
            const EffectGroupDraw &candidate = base->effect_groups_[*(it - 1)];
            return order > candidate.paint.order && order < candidate.last_order ? piece_for_layer(candidate.effect->layer) : 0;
        };
        for (const QuadDraw &draw : base->quads_) pieces[piece_of_order(draw.paint.order)]->quads_.push_back(draw);
        for (const StrokeDraw &draw : base->strokes_) pieces[piece_of_order(draw.paint.order)]->strokes_.push_back(draw);
        for (const FillQuadDraw &draw : base->fills_) pieces[piece_of_order(draw.paint.order)]->fills_.push_back(draw);
        for (const SectorDraw &draw : base->sectors_) pieces[piece_of_order(draw.paint.order)]->sectors_.push_back(draw);
        for (const CustomDraw &draw : base->custom_draws_) pieces[piece_of_order(draw.paint.order)]->custom_draws_.push_back(draw);
        for (const CustomStrokeDraw &draw : base->custom_strokes_) pieces[piece_of_order(draw.paint.order)]->custom_strokes_.push_back(draw);
        for (usize i = 0; i < base->glyphs_.size(); ++i) {
            FrameSnapshot &piece = *pieces[piece_of_order(base->glyph_paint_[i].order)];
            piece.glyphs_.push_back(base->glyphs_[i]);
            piece.glyph_scissors_.push_back(base->glyph_scissors_[i]);
            piece.glyph_paint_.push_back(base->glyph_paint_[i]);
        }
        // Groups: a hoisted group and the groups inside it go to its piece, every other group stays in the lowest one. Indices are
        // re-made per piece (a group always comes after its parent in `effect_groups_`, which keeps that true).
        std::vector<i32> new_index(base->effect_groups_.size(), -1);
        for (usize i = 0; i < base->effect_groups_.size(); ++i) {
            EffectGroupDraw group = base->effect_groups_[i];
            const bool is_hoisted = std::ranges::find(hoisted, i) != hoisted.end();
            // A group is inside a hoisted one when its own order falls in that group's range; a hoisted group itself sits on its start.
            const usize piece_index = is_hoisted ? piece_for_layer(group.effect->layer) : piece_of_order(group.paint.order);
            FrameSnapshot &piece = *pieces[piece_index];
            group.parent = is_hoisted || group.parent < 0 ? -1 : new_index[static_cast<usize>(group.parent)];
            new_index[i] = static_cast<i32>(piece.effect_groups_.size());
            piece.effect_groups_.push_back(group);
        }
        // Layer markers have done their job; a piece's remaining groups are the ones with passes to run.
        for (auto &piece : pieces) {
            piece->effect_groups_ = flatten_effect_groups(piece->effect_groups_);
        }
        return pieces;
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

    std::span<const EffectGroupDraw> FrameSnapshot::effect_groups() const noexcept { return effect_groups_; }

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
    ElementScope::ElementScope(SEClay_Context *context, vector<ZFrame> *z_stack) noexcept
    : context_(context), z_stack_(z_stack) {}

} // namespace SFT::UI

