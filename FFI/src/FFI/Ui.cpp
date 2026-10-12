/// C ABI implementation of the immediate-mode UI.
///
/// The engine's UI is built from RAII scopes — `Context::element` returns an `ElementScope` whose
/// destructor closes the element — which has no equivalent in C. This layer keeps the open scopes on
/// a stack so a caller can pair explicit `begin_element`/`end_element` calls instead, and reports an
/// unbalanced tree rather than letting it corrupt the layout.
///
/// It also owns the per-frame orchestration a C++ caller writes by hand: ready the UI renderer,
/// begin a layout sized to the surface, finish it into a snapshot, and attach the snapshot to the
/// frame as an overlay. Those five steps have to happen in order and in the right places, so
/// wrapping them in `sturdy_ui_begin`/`sturdy_ui_end` is what keeps a foreign caller from getting
/// the sequence subtly wrong.
///
/// State lives in one process-wide session rather than per engine: a UI frame is bounded by
/// begin/end on the thread building it, and only one can be open at a time.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include <Engine/Engine.hpp>
#include <Engine/ForeignUi.hpp>
#include <Renderer/UI/UI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    // The session itself (the per-thread element stack, the engine's ScreenUi, the font registry and the begin/end orchestration) lives in
    // `Engine/ForeignUi.hpp`, shared with CxxApi so every binding behaves identically; this file translates ABI structs and result codes.
    namespace E = SFT::Engine;

    /// Maps the shared UI layer's error class onto this ABI's result codes.
    [[nodiscard]] SturdyResult to_result(const E::ForeignUiError &failure) {
        switch (failure.kind) {
        case E::ForeignUiErrorKind::Busy:
            return set_error(STURDY_ERROR_BUSY, failure.message);
        case E::ForeignUiErrorKind::NotAvailable:
            return set_error(STURDY_ERROR_NOT_AVAILABLE, failure.message);
        case E::ForeignUiErrorKind::InvalidArgument:
            break;
        }
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, failure.message);
    }

    /// Copies an ABI element into the shared description.
    [[nodiscard]] E::ForeignUiElement to_foreign_element(const SturdyUiElement &element) {
        E::ForeignUiElement out;
        out.width_kind = static_cast<E::ForeignUiSizing>(element.width_kind);
        out.width_value = element.width_value;
        out.height_kind = static_cast<E::ForeignUiSizing>(element.height_kind);
        out.height_value = element.height_value;
        out.padding[0] = element.padding_left;
        out.padding[1] = element.padding_right;
        out.padding[2] = element.padding_top;
        out.padding[3] = element.padding_bottom;
        out.child_gap = element.child_gap;
        out.top_to_bottom = element.direction == STURDY_UI_DIRECTION_TOP_TO_BOTTOM;
        for (int i = 0; i < 4; ++i) {
            out.background[i] = element.background[i];
            out.corner_radius[i] = element.corner_radius[i];
        }
        if (element.id != nullptr) {
            out.id = element.id;
        }
        return out;
    }

    /// Converts an ABI color to the engine's sRGB color.
    ///
    /// @param rgba Four channels, non-linear sRGB with straight alpha.
    ///
    /// @return The engine-side color.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::UI::Color to_engine_color(const float *rgba) noexcept {
        return SFT::UI::Color{static_cast<SFT::f64>(rgba[0]), static_cast<SFT::f64>(rgba[1]),
                              static_cast<SFT::f64>(rgba[2]), static_cast<SFT::f64>(rgba[3])};
    }

    /// Translates an ABI sizing mode into the engine's axis descriptor.
    ///
    /// @param kind Sizing mode.
    /// @param value Size, interpreted per mode.
    /// @param out_axis Receives the translated axis.
    ///
    /// @return Returns `true` when `kind` is recognized; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool to_engine_axis(SturdyUiSizing kind, float value, SFT::UI::SizingAxis *out_axis) noexcept {
        switch (kind) {
        case STURDY_UI_SIZING_FIT:
            *out_axis = SFT::UI::SizingAxis::fit();
            return true;
        case STURDY_UI_SIZING_GROW:
            *out_axis = SFT::UI::SizingAxis::grow();
            return true;
        case STURDY_UI_SIZING_FIXED:
            *out_axis = SFT::UI::SizingAxis::fixed(value);
            return true;
        case STURDY_UI_SIZING_PERCENT:
            *out_axis = SFT::UI::SizingAxis::percent(value);
            return true;
        case STURDY_UI_SIZING_FORCE_U32:
        default:
            return false;
        }
    }

    /// Reports the open session's context, or a failure explaining why there is none.
    ///
    /// @param out_context Receives the borrowed context on success.
    ///
    /// @return `STURDY_OK`, or `STURDY_ERROR_BUSY` when no UI frame is open.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult require_session(SFT::UI::Context **out_context) noexcept {
        SFT::UI::Context *context = E::foreign_ui_context();
        if (context == nullptr) {
            return set_error(STURDY_ERROR_BUSY,
                             "no UI frame is open on this thread; call sturdy_ui_begin first");
        }
        *out_context = context;
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_ui_element_init(SturdyUiElement *element) {
    return guarded([&]() -> SturdyResult {
        if (element == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "element must not be null");
        }
        *element = SturdyUiElement{};
        element->struct_size = static_cast<uint32_t>(sizeof(SturdyUiElement));
        element->width_kind = STURDY_UI_SIZING_FIT;
        element->height_kind = STURDY_UI_SIZING_FIT;
        element->direction = STURDY_UI_DIRECTION_LEFT_TO_RIGHT;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_text_style_init(SturdyUiTextStyle *style) {
    return guarded([&]() -> SturdyResult {
        if (style == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "style must not be null");
        }
        *style = SturdyUiTextStyle{};
        style->struct_size = static_cast<uint32_t>(sizeof(SturdyUiTextStyle));
        style->font_size = 16;
        style->color[0] = 1.0f;
        style->color[1] = 1.0f;
        style->color[2] = 1.0f;
        style->color[3] = 1.0f;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_register_font(SturdyEngine engine,
                                                     const char *source,
                                                     uint32_t font_id) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto registered = E::register_foreign_ui_font(*resolved_engine, source, font_id);
        return registered ? STURDY_OK : to_result(registered.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_begin(SturdyEngine engine, const SturdyFrameInput *input) {
    return guarded([&]() -> SturdyResult {
        if (input == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "frame input must not be null");
        }
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto begun = E::begin_foreign_ui_frame(*resolved_engine, input->framebuffer_width, input->framebuffer_height, input->delta_seconds);
        return begun ? STURDY_OK : to_result(begun.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_end(SturdyEngine engine, SturdyFrame frame) {
    return guarded([&]() -> SturdyResult {
        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            // The handle is bad but a frame is open: finish and discard it so the next frame starts clean.
            E::discard_foreign_ui_frame();
            return resolved;
        }

        // A zeroed frame handle means "discard": the layout is still finished, so the next frame
        // starts clean, but nothing is attached.
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        if (frame.token != 0) {
            void *pointer = nullptr;
            const SturdyResult frame_resolved =
                SFT::Ffi::resolve_handle(frame.token, SFT::Ffi::HandleKind::Frame, &pointer);
            if (frame_resolved != STURDY_OK) {
                E::discard_foreign_ui_frame();
                return frame_resolved;
            }
            parameters = static_cast<SFT::Engine::RenderFrameParameters *>(pointer);
        }

        const auto ended = E::end_foreign_ui_frame(*resolved_engine, parameters);
        return ended ? STURDY_OK : to_result(ended.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_begin_element(SturdyEngine engine,
                                                     const SturdyUiElement *element) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (element == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "element must not be null");
        }
        if (element->struct_size != sizeof(SturdyUiElement)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiElement size does not match this engine build");
        }

        const auto begun = E::begin_foreign_ui_element(to_foreign_element(*element));
        return begun ? STURDY_OK : to_result(begun.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_end_element(SturdyEngine engine) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        const auto ended = E::end_foreign_ui_element();
        return ended ? STURDY_OK : to_result(ended.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_text(SturdyEngine engine,
                                            const char *text,
                                            const SturdyUiTextStyle *style) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (text == nullptr || style == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text and style must not be null");
        }
        if (style->struct_size != sizeof(SturdyUiTextStyle)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiTextStyle size does not match this engine build");
        }

        E::ForeignUiTextStyle foreign_style;
        foreign_style.font_id = style->font_id;
        foreign_style.font_size = style->font_size;
        for (int i = 0; i < 4; ++i) {
            foreign_style.color[i] = style->color[i];
        }
        const auto drawn = E::foreign_ui_text(text, foreign_style);
        return drawn ? STURDY_OK : to_result(drawn.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_hovered(SturdyEngine engine,
                                               const char *id,
                                               SturdyBool *out_hovered) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (id == nullptr || out_hovered == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "id and output pointer must not be null");
        }
        const auto hovered = E::foreign_ui_hovered(id);
        if (!hovered) {
            return to_result(hovered.error());
        }
        *out_hovered = *hovered ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_clicked(SturdyEngine engine,
                                               const char *id,
                                               SturdyBool *out_clicked) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (id == nullptr || out_clicked == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "id and output pointer must not be null");
        }
        const auto clicked = E::foreign_ui_clicked(id);
        if (!clicked) {
            return to_result(clicked.error());
        }
        *out_clicked = *clicked ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_set_scale_mode(SturdyEngine engine, SturdyUiScaleMode mode) {
    return guarded([&]() -> SturdyResult {
        E::ForeignUiScaleMode engine_mode{};
        switch (mode) {
        case STURDY_UI_SCALE_MODE_CONTENT_SCALE:
            engine_mode = E::ForeignUiScaleMode::ContentScale;
            break;
        case STURDY_UI_SCALE_MODE_RAW_PIXELS:
            engine_mode = E::ForeignUiScaleMode::RawPixels;
            break;
        case STURDY_UI_SCALE_MODE_FORCE_U32:
        default:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown UI scale mode");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const auto set = E::set_foreign_ui_scale_mode(*resolved_engine, engine_mode);
        return set ? STURDY_OK : to_result(set.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_position(SturdyEngine engine,
                                                        float *out_x,
                                                        float *out_y) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        const auto position = E::foreign_ui_pointer_position();
        if (!position) {
            return to_result(position.error());
        }
        if (out_x != nullptr) {
            *out_x = position->x;
        }
        if (out_y != nullptr) {
            *out_y = position->y;
        }
        return STURDY_OK;
    });
}

namespace {

    /// Converts an ABI graph scale to the engine's scale transform.
    ///
    /// @param axis Source axis description.
    /// @param out_scale Receives the translated scale.
    ///
    /// @return Returns `true` when `axis->scale` is recognized; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool to_engine_scale(const SturdyUiGraphAxis &axis, SFT::UI::ScaleTransform *out_scale) noexcept {
        SFT::UI::ScaleTransform scale{};
        scale.log_base = axis.log_base;
        scale.symlog_linear_threshold = axis.symlog_linear_threshold;
        switch (axis.scale) {
        case STURDY_UI_GRAPH_SCALE_LINEAR:
            scale.kind = SFT::UI::ScaleKind::Linear;
            break;
        case STURDY_UI_GRAPH_SCALE_LOG:
            scale.kind = SFT::UI::ScaleKind::Log;
            break;
        case STURDY_UI_GRAPH_SCALE_SYMLOG:
            scale.kind = SFT::UI::ScaleKind::Symlog;
            break;
        case STURDY_UI_GRAPH_SCALE_FORCE_U32:
        default:
            return false;
        }
        *out_scale = scale;
        return true;
    }

    /// Converts an ABI graph axis to the engine's axis config.
    ///
    /// @param axis Source axis description.
    /// @param out_axis Receives the translated axis.
    ///
    /// @return Returns `true` when every field translates cleanly; otherwise `false`.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] bool to_engine_graph_axis(const SturdyUiGraphAxis &axis, SFT::UI::AxisConfig *out_axis) {
        SFT::UI::AxisConfig engine_axis{};
        if (!to_engine_scale(axis, &engine_axis.scale)) {
            return false;
        }
        if (axis.has_min == STURDY_TRUE) {
            engine_axis.min = axis.min;
        }
        if (axis.has_max == STURDY_TRUE) {
            engine_axis.max = axis.max;
        }
        engine_axis.autoscale_padding_percent = axis.autoscale_padding_percent;
        engine_axis.target_tick_count = axis.target_tick_count;
        engine_axis.show_gridlines = axis.show_gridlines == STURDY_TRUE;
        engine_axis.show_minor_gridlines = axis.show_minor_gridlines == STURDY_TRUE;
        engine_axis.axis_color = to_engine_color(axis.axis_color);
        engine_axis.gridline_color = to_engine_color(axis.gridline_color);
        engine_axis.minor_gridline_color = to_engine_color(axis.minor_gridline_color);
        if (axis.title != nullptr) {
            engine_axis.title = axis.title;
        }
        if (axis.categories != nullptr && axis.category_count != 0) {
            engine_axis.is_categorical = true;
            engine_axis.categories.reserve(axis.category_count);
            for (uint32_t i = 0; i < axis.category_count; ++i) {
                engine_axis.categories.emplace_back(axis.categories[i] != nullptr ? axis.categories[i] : "");
            }
        }
        *out_axis = std::move(engine_axis);
        return true;
    }

} // namespace

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_desc_init(SturdyUiGraphDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc must not be null");
        }
        *desc = SturdyUiGraphDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyUiGraphDesc));
        desc->type = STURDY_UI_GRAPH_TYPE_LINE;
        (void)sturdy_ui_graph_axis_init(&desc->x_axis);
        (void)sturdy_ui_graph_axis_init(&desc->y_axis);
        desc->bar_group_gap_fraction = 0.2f;
        desc->bar_series_gap_fraction = 0.08f;
        desc->pie_gap_degrees = 0.0f;
        desc->pie_feather_px = 1.0f;
        desc->background[0] = 0.07f;
        desc->background[1] = 0.08f;
        desc->background[2] = 0.1f;
        desc->background[3] = 1.0f;
        desc->corner_radius[0] = desc->corner_radius[1] = desc->corner_radius[2] = desc->corner_radius[3] = 6.0f;
        desc->axis_margin_left = 48.0f;
        desc->axis_margin_bottom = 24.0f;
        desc->axis_margin_top = 10.0f;
        desc->axis_margin_right = 10.0f;
        desc->font_id = 0;
        desc->label_font_size = 11;
        desc->title_font_size = 12;
        desc->show_legend = STURDY_TRUE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_axis_init(SturdyUiGraphAxis *axis) {
    return guarded([&]() -> SturdyResult {
        if (axis == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "axis must not be null");
        }
        *axis = SturdyUiGraphAxis{};
        axis->struct_size = static_cast<uint32_t>(sizeof(SturdyUiGraphAxis));
        axis->scale = STURDY_UI_GRAPH_SCALE_LINEAR;
        axis->log_base = 10.0;
        axis->symlog_linear_threshold = 1.0;
        axis->autoscale_padding_percent = 0.05f;
        axis->target_tick_count = 6;
        axis->show_gridlines = STURDY_TRUE;
        axis->axis_color[0] = 0.45f;
        axis->axis_color[1] = 0.47f;
        axis->axis_color[2] = 0.53f;
        axis->axis_color[3] = 1.0f;
        axis->gridline_color[0] = 1.0f;
        axis->gridline_color[1] = 1.0f;
        axis->gridline_color[2] = 1.0f;
        axis->gridline_color[3] = 0.06f;
        axis->minor_gridline_color[0] = 1.0f;
        axis->minor_gridline_color[1] = 1.0f;
        axis->minor_gridline_color[2] = 1.0f;
        axis->minor_gridline_color[3] = 0.03f;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_graph_draw(SturdyEngine engine, const SturdyUiElement *element,
                                                  const SturdyUiGraphDesc *desc) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (element == nullptr || desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "element and desc must not be null");
        }
        if (element->struct_size != sizeof(SturdyUiElement)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiElement size does not match this engine build");
        }
        if (desc->struct_size != sizeof(SturdyUiGraphDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyUiGraphDesc size does not match this engine build");
        }

        SFT::UI::Context *context = nullptr;
        const SturdyResult session = require_session(&context);
        if (session != STURDY_OK) {
            return session;
        }

        SFT::UI::SizingAxis width{};
        SFT::UI::SizingAxis height{};
        if (!to_engine_axis(element->width_kind, element->width_value, &width) ||
            !to_engine_axis(element->height_kind, element->height_value, &height)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized sizing mode");
        }
        SFT::UI::ElementDecl decl{};
        decl.sizing = SFT::UI::Sizing{width, height};
        if (element->id != nullptr) {
            decl.id = SFT::UString{element->id};
        }

        SFT::UI::GraphDesc graph_desc{};
        switch (desc->type) {
        case STURDY_UI_GRAPH_TYPE_LINE:
            graph_desc.type = SFT::UI::GraphType::Line;
            break;
        case STURDY_UI_GRAPH_TYPE_AREA:
            graph_desc.type = SFT::UI::GraphType::Area;
            break;
        case STURDY_UI_GRAPH_TYPE_BAR:
            graph_desc.type = SFT::UI::GraphType::Bar;
            break;
        case STURDY_UI_GRAPH_TYPE_SCATTER:
            graph_desc.type = SFT::UI::GraphType::Scatter;
            break;
        case STURDY_UI_GRAPH_TYPE_PIE:
            graph_desc.type = SFT::UI::GraphType::Pie;
            break;
        case STURDY_UI_GRAPH_TYPE_FORCE_U32:
        default:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized graph type");
        }
        if (!to_engine_graph_axis(desc->x_axis, &graph_desc.x_axis) ||
            !to_engine_graph_axis(desc->y_axis, &graph_desc.y_axis)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized axis scale");
        }

        graph_desc.series.reserve(desc->series_count);
        for (uint32_t i = 0; i < desc->series_count; ++i) {
            const SturdyUiGraphSeriesRef &series = desc->series[i];
            if (series.struct_size != sizeof(SturdyUiGraphSeriesRef)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                                 "SturdyUiGraphSeriesRef size does not match this engine build");
            }
            graph_desc.series.push_back(SFT::UI::SeriesRef{
                .name = series.name != nullptr ? series.name : std::string{},
                .x = std::span<const SFT::f64>{series.x_data, series.x_data != nullptr ? series.x_count : 0u},
                .y = std::span<const SFT::f64>{series.y_data, series.y_data != nullptr ? series.y_count : 0u},
                .color = to_engine_color(series.color),
                .line_width = series.line_width > 0.0f ? series.line_width : 2.0f,
                .feather_px = series.feather_px,
                .area_fill_opacity = series.area_fill_opacity,
                .marker_radius = series.marker_radius,
            });
        }

        graph_desc.bar_stack_mode = desc->bar_stack_mode == STURDY_UI_GRAPH_BAR_STACKED
                                        ? SFT::UI::BarStackMode::Stacked
                                        : SFT::UI::BarStackMode::Grouped;
        graph_desc.bar_group_gap_fraction = desc->bar_group_gap_fraction;
        graph_desc.bar_series_gap_fraction = desc->bar_series_gap_fraction;

        graph_desc.pie_slices.reserve(desc->pie_slice_count);
        for (uint32_t i = 0; i < desc->pie_slice_count; ++i) {
            const SturdyUiGraphPieSlice &slice = desc->pie_slices[i];
            if (slice.struct_size != sizeof(SturdyUiGraphPieSlice)) {
                return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                                 "SturdyUiGraphPieSlice size does not match this engine build");
            }
            graph_desc.pie_slices.push_back(SFT::UI::PieSlice{
                .name = slice.name != nullptr ? slice.name : std::string{},
                .value = slice.value,
                .color = to_engine_color(slice.color),
            });
        }
        graph_desc.pie_style.hole_ratio = desc->pie_hole_ratio;
        graph_desc.pie_style.start_angle_degrees =
            desc->pie_start_angle_degrees != 0.0f ? desc->pie_start_angle_degrees : -90.0f;
        graph_desc.pie_style.gap_degrees = desc->pie_gap_degrees;
        graph_desc.pie_style.feather_px = desc->pie_feather_px > 0.0f ? desc->pie_feather_px : 1.0f;

        graph_desc.background_color = to_engine_color(desc->background);
        graph_desc.corner_radius = SFT::UI::CornerRadius{
            desc->corner_radius[0], desc->corner_radius[1], desc->corner_radius[2], desc->corner_radius[3]};
        if (desc->axis_margin_left > 0.0f) graph_desc.axis_margin_left = desc->axis_margin_left;
        if (desc->axis_margin_bottom > 0.0f) graph_desc.axis_margin_bottom = desc->axis_margin_bottom;
        if (desc->axis_margin_top > 0.0f) graph_desc.axis_margin_top = desc->axis_margin_top;
        if (desc->axis_margin_right > 0.0f) graph_desc.axis_margin_right = desc->axis_margin_right;
        graph_desc.font_id = static_cast<SFT::UI::FontId>(desc->font_id);
        if (desc->label_font_size != 0) graph_desc.label_font_size = static_cast<SFT::u16>(desc->label_font_size);
        if (desc->title_font_size != 0) graph_desc.title_font_size = static_cast<SFT::u16>(desc->title_font_size);
        graph_desc.show_legend = desc->show_legend == STURDY_TRUE;

        SFT::UI::GraphState state{};
        (void)SFT::UI::graph(*context, decl, graph_desc, state);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_down(SturdyEngine engine, SturdyBool *out_down) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (out_down == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const auto down = E::foreign_ui_pointer_down();
        if (!down) {
            return to_result(down.error());
        }
        *out_down = *down ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

} // extern "C"

namespace SFT::Ffi {

    /// Drops the on-screen UI this ABI created. Called from the runtime's shutdown, before the engine is destroyed
    /// (the UI holds a system in the engine's schedule and GPU objects on its device).
    void release_ui_state() noexcept { SFT::Engine::release_foreign_ui(); }

} // namespace SFT::Ffi
