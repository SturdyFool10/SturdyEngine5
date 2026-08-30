#include <Renderer/UI/Graph.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>
#pragma endregion

namespace SFT::UI {

    using std::string_view;

    namespace {

        /// Flattens every bound series' x (or y) samples into one array for AxisConfig autoscaling.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param want_x Whether the associated behavior is enabled.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<f64> collect_axis_values(const GraphDesc &desc, bool want_x) {
            vector<f64> values;
            for (const SeriesRef &series : desc.series) {
                const span<const f64> source = want_x ? series.x : series.y;
                values.insert(values.end(), source.begin(), source.end());
            }
            return values;
        }

        /// Flattens Bar-chart Y values for autoscaling — each series independently for Grouped, or
        /// each category's summed positive/negative totals for Stacked (a stacked bar's visible extent
        /// is the sum, not any individual series' value).
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param category_count Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<f64> collect_bar_y_values(const GraphDesc &desc, usize category_count) {
            vector<f64> values{0.0}; // always keep the zero baseline in view
            if (desc.bar_stack_mode == BarStackMode::Stacked) {
                for (usize c = 0; c < category_count; ++c) {
                    f64 pos_sum = 0.0;
                    f64 neg_sum = 0.0;
                    for (const SeriesRef &series : desc.series) {
                        if (c >= series.y.size()) {
                            continue;
                        }
                        const f64 v = series.y[c];
                        if (v >= 0.0) {
                            pos_sum += v;
                        } else {
                            neg_sum += v;
                        }
                    }
                    values.push_back(pos_sum);
                    values.push_back(neg_sum);
                }
            } else {
                for (const SeriesRef &series : desc.series) {
                    values.insert(values.end(), series.y.begin(), series.y.end());
                }
            }
            return values;
        }

        /// Maps a tick's value to a normalized [0,1] axis position — shared by gridline and tick-label
        /// placement so the two never disagree. A categorical tick's `value` is a category index, not
        /// a data-space number, so it bypasses `scale` entirely (each category gets an evenly spaced
        /// slot center).
        ///
        /// @param axis `axis` value used by the operation.
        /// @param resolved `resolved` value used by the operation.
        /// @param tick `tick` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 tick_normalized_t(const AxisConfig &axis, const AxisRange &resolved, const TickSpec &tick) noexcept {
            return axis.is_categorical
                       ? (axis.categories.size() > 1 ? (tick.value + 0.5) / static_cast<f64>(axis.categories.size()) : 0.5)
                       : axis.scale.to_normalized(tick.value, resolved.min, resolved.max);
        }

        /// Appends one axis's gridlines to `out_paths`, mapping tick values into plot-local pixel
        /// space.
        ///
        /// @param axis `axis` value used by the operation.
        /// @param resolved `resolved` value used by the operation.
        /// @param ticks Ticks used or affected by the operation.
        /// @param plot_size Requested or available size for the operation.
        /// @param horizontal Whether the associated behavior is enabled.
        /// @param out_paths `out_paths` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void append_gridlines(const AxisConfig &axis, const AxisRange &resolved, span<const TickSpec> ticks,
                              glm::vec2 plot_size, bool horizontal, vector<StrokePath> &out_paths) {
            for (const TickSpec &tick : ticks) {
                if (tick.is_minor ? !axis.show_minor_gridlines : !axis.show_gridlines) {
                    continue;
                }
                const f64 t = tick_normalized_t(axis, resolved, tick);
                StrokeStyle style = axis.gridline_stroke;
                style.color = tick.is_minor ? axis.minor_gridline_color : axis.gridline_color;
                if (horizontal) {
                    const f32 y = plot_size.y * (1.0f - static_cast<f32>(t));
                    out_paths.push_back(StrokePath{.points = {{0.0f, y}, {plot_size.x, y}}, .style = style});
                } else {
                    const f32 x = plot_size.x * static_cast<f32>(t);
                    out_paths.push_back(StrokePath{.points = {{x, 0.0f}, {x, plot_size.y}}, .style = style});
                }
            }
        }

        /// Maps one (x, y) data-space point into plot-local pixel space.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param x_range `x_range` value used by the operation.
        /// @param y_range `y_range` value used by the operation.
        /// @param plot_size Requested or available size for the operation.
        /// @param x Value consumed by the operation.
        /// @param y Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 to_pixel(const GraphDesc &desc, const AxisRange &x_range, const AxisRange &y_range,
                                         glm::vec2 plot_size, f64 x, f64 y) noexcept {
            const f64 tx = desc.x_axis.scale.to_normalized(x, x_range.min, x_range.max);
            const f64 ty = desc.y_axis.scale.to_normalized(y, y_range.min, y_range.max);
            return glm::vec2{plot_size.x * static_cast<f32>(tx), plot_size.y * (1.0f - static_cast<f32>(ty))};
        }

        /// Appends a subdivided-column approximation of the filled area between `series`' line and the
        /// Y=0 baseline (`baseline_px`), one thin FillQuad per subdivision so the fill tracks the same
        /// (implicitly linear) interpolation the connecting line itself draws between data points.
        ///
        /// @param series `series` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param x_range `x_range` value used by the operation.
        /// @param y_range `y_range` value used by the operation.
        /// @param plot_size Requested or available size for the operation.
        /// @param baseline_px Requested or available size for the operation.
        /// @param out_fills `out_fills` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void append_area_fill(const SeriesRef &series, const GraphDesc &desc, const AxisRange &x_range,
                              const AxisRange &y_range, glm::vec2 plot_size, f32 baseline_px,
                              vector<FillQuad> &out_fills) {
            if (series.area_fill_opacity <= 0.0f) {
                return;
            }
            Color fill_color = series.color;
            fill_color.a *= static_cast<f64>(series.area_fill_opacity);

            constexpr int subdivisions = 6;
            for (usize i = 0; i + 1 < series.x.size(); ++i) {
                const glm::vec2 p0 = to_pixel(desc, x_range, y_range, plot_size, series.x[i], series.y[i]);
                const glm::vec2 p1 = to_pixel(desc, x_range, y_range, plot_size, series.x[i + 1], series.y[i + 1]);
                for (int s = 0; s < subdivisions; ++s) {
                    const f32 t0 = static_cast<f32>(s) / static_cast<f32>(subdivisions);
                    const f32 t1 = static_cast<f32>(s + 1) / static_cast<f32>(subdivisions);
                    const f32 x0 = p0.x + (p1.x - p0.x) * t0;
                    const f32 x1 = p0.x + (p1.x - p0.x) * t1;
                    const f32 y_mid = p0.y + (p1.y - p0.y) * ((t0 + t1) * 0.5f);
                    const f32 top = std::min(baseline_px, y_mid);
                    const f32 height = std::abs(baseline_px - y_mid);
                    if (x1 - x0 <= 0.0f || height <= 0.0f) {
                        continue;
                    }
                    out_fills.push_back(FillQuad{.position = {x0, top}, .size = {x1 - x0, height}, .color = fill_color});
                }
            }
        }

        /// Draws one axis's major tick labels as small floating text elements attached to the
        /// widget's own box (see FloatingConfig's own doc comment, Style.hpp) — a handful of labels
        /// per axis is exactly the "rare overlay" case floating positioning exists for, unlike the
        /// "many overlapping lines/fills" case stroke_paths()/fill_quads() exist to avoid needing it
        /// for. Minor ticks aren't labeled (too dense to read).
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param axis `axis` value used by the operation.
        /// @param resolved `resolved` value used by the operation.
        /// @param ticks Ticks used or affected by the operation.
        /// @param plot_origin Requested or available size for the operation.
        /// @param plot_size Requested or available size for the operation.
        /// @param horizontal Whether the associated behavior is enabled.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_tick_labels(Context &ctx, const GraphDesc &desc, const AxisConfig &axis, const AxisRange &resolved,
                              span<const TickSpec> ticks, glm::vec2 plot_origin, glm::vec2 plot_size, bool horizontal) {
            const TextStyle style{.color = axis.axis_color, .font_id = desc.font_id, .font_size = desc.label_font_size};
            for (const TickSpec &tick : ticks) {
                if (tick.is_minor || tick.label.empty()) {
                    continue;
                }
                const f64 t = tick_normalized_t(axis, resolved, tick);
                if (horizontal) {
                    const f32 y = plot_origin.y + plot_size.y * (1.0f - static_cast<f32>(t));
                    (void)ctx.element(ElementDecl{
                        .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                        .floating = FloatingConfig{
                            .attach_to = FloatingAttachTo::Parent,
                            .element_attach_point = FloatingAttachPoint::RightCenter,
                            .parent_attach_point = FloatingAttachPoint::LeftTop,
                            .offset = {plot_origin.x - 6.0f, y},
                            .capture_pointer = false,
                            .clip_to = FloatingClipTo::AttachedParent,
                        },
                    });
                    ctx.text(ustr{std::string_view{tick.label}}, style);
                } else {
                    const f32 x = plot_origin.x + plot_size.x * static_cast<f32>(t);
                    (void)ctx.element(ElementDecl{
                        .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                        .floating = FloatingConfig{
                            .attach_to = FloatingAttachTo::Parent,
                            .element_attach_point = FloatingAttachPoint::CenterTop,
                            .parent_attach_point = FloatingAttachPoint::LeftTop,
                            .offset = {x, plot_origin.y + plot_size.y + 4.0f},
                            .capture_pointer = false,
                            .clip_to = FloatingClipTo::AttachedParent,
                        },
                    });
                    ctx.text(ustr{std::string_view{tick.label}}, style);
                }
            }
        }

        /// Draws a small swatch+name legend, floating in the widget's top-right corner. Entries come
        /// from `series` for every Cartesian chart type, or `pie_slices` for Pie; an unnamed entry is
        /// skipped (a caller that doesn't want a legend entry for one series just leaves its name
        /// empty, rather than needing a separate opt-out flag per entry).
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param id_prefix Uniquely identifies the calling `UI::graph()` instance — Clay element IDs
        ///        are hashed from the id string alone with no implicit parent scoping, so two graphs in
        ///        the same layout both defaulting to unprefixed "legend-row-0" etc. would collide and
        ///        corrupt Clay's element hash map for the rest of that frame (visible as "element ID
        ///        already previously declared" errors, and downstream elements losing correct clip/
        ///        scissor resolution).
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_legend(Context &ctx, const GraphDesc &desc, const string &id_prefix) {
            if (!desc.show_legend) {
                return;
            }
            struct LegendEntry {
                string_view name;
                Color color;
            };
            vector<LegendEntry> entries;
            if (desc.type == GraphType::Pie) {
                for (const PieSlice &slice : desc.pie_slices) {
                    if (!slice.name.empty()) {
                        entries.push_back({slice.name, slice.color});
                    }
                }
            } else {
                for (const SeriesRef &series : desc.series) {
                    if (!series.name.empty()) {
                        entries.push_back({series.name, series.color});
                    }
                }
            }
            if (entries.empty()) {
                return;
            }

            auto legend_scope = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                .padding = Padding::all(6),
                .child_gap = 4,
                .direction = LayoutDirection::TopToBottom,
                .background_color = Color{0.0, 0.0, 0.0, 0.35},
                .corner_radius = CornerRadius::all(4.0f),
                .floating = FloatingConfig{
                    .attach_to = FloatingAttachTo::Parent,
                    .element_attach_point = FloatingAttachPoint::RightTop,
                    .parent_attach_point = FloatingAttachPoint::RightTop,
                    .offset = {-6.0f, 6.0f},
                    .capture_pointer = false,
                    .clip_to = FloatingClipTo::AttachedParent,
                },
            });
            const TextStyle style{.color = Color{1.0, 1.0, 1.0, 0.9}, .font_id = desc.font_id, .font_size = desc.label_font_size};
            for (usize i = 0; i < entries.size(); ++i) {
                auto row = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                    .child_gap = 6,
                    .direction = LayoutDirection::LeftToRight,
                    .id = UString{id_prefix + "-legend-row-" + std::to_string(i)},
                });
                (void)ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fixed(10.0f), SizingAxis::fixed(10.0f)},
                    .background_color = entries[i].color,
                    .corner_radius = CornerRadius::all(2.0f),
                });
                ctx.text(ustr{entries[i].name}, style);
            }
        }

        /// Draws every Cartesian chart type (Line/Area/Bar/Scatter) into `widget_size`-sized plot-
        /// local space, offset into the widget's own local space, and issued via stroke_paths()/
        /// fill_quads().
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param decl `decl` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param widget_size Requested or available size for the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_cartesian_chart(Context &ctx, const ElementDecl &decl, const GraphDesc &desc, glm::vec2 widget_size) {
            const glm::vec2 plot_origin{desc.axis_margin_left, desc.axis_margin_top};
            const glm::vec2 plot_size{
                std::max(widget_size.x - desc.axis_margin_left - desc.axis_margin_right, 0.0f),
                std::max(widget_size.y - desc.axis_margin_top - desc.axis_margin_bottom, 0.0f),
            };
            if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) {
                return;
            }

            const bool is_bar = desc.type == GraphType::Bar;

            // Bar charts always use a categorical X axis — derive one (synthetic "0","1",... labels)
            // from the longest bound series if the caller didn't already configure
            // desc.x_axis.is_categorical, so a Bar chart never silently draws nothing just because
            // that flag was left unset.
            AxisConfig bar_x_axis;
            usize category_count = 0;
            if (is_bar) {
                bar_x_axis = desc.x_axis;
                bar_x_axis.is_categorical = true;
                category_count = desc.x_axis.categories.size();
                if (category_count == 0) {
                    for (const SeriesRef &series : desc.series) {
                        category_count = std::max(category_count, series.y.size());
                    }
                    bar_x_axis.categories.clear();
                    bar_x_axis.categories.reserve(category_count);
                    for (usize i = 0; i < category_count; ++i) {
                        bar_x_axis.categories.push_back(std::to_string(i));
                    }
                }
            }
            const AxisConfig &x_axis = is_bar ? bar_x_axis : desc.x_axis;

            const vector<f64> x_values = is_bar ? vector<f64>{} : collect_axis_values(desc, true);
            const vector<f64> y_values = is_bar ? collect_bar_y_values(desc, category_count) : collect_axis_values(desc, false);
            const AxisRange x_range = resolve_axis_range(x_axis, x_values);
            const AxisRange y_range = resolve_axis_range(desc.y_axis, y_values);
            const vector<TickSpec> x_ticks = generate_ticks(x_axis, x_range.min, x_range.max);
            const vector<TickSpec> y_ticks = generate_ticks(desc.y_axis, y_range.min, y_range.max);

            vector<StrokePath> paths;
            vector<FillQuad> fills;
            paths.reserve(x_ticks.size() + y_ticks.size() + desc.series.size() + 2);

            append_gridlines(desc.y_axis, y_range, y_ticks, plot_size, true, paths);
            append_gridlines(x_axis, x_range, x_ticks, plot_size, false, paths);

            // Axis lines cross through data-space zero on the *other* axis when that zero is within
            // the visible range (the conventional "origin" placement), clamped to the plot edge
            // otherwise. A categorical axis (Bar's X axis) has no data-space zero at all, so its axis
            // line always just pins to the left edge.
            const f64 x_zero_t = is_bar ? 0.0 : std::clamp(desc.x_axis.scale.to_normalized(0.0, x_range.min, x_range.max), 0.0, 1.0);
            const f64 y_zero_t = std::clamp(desc.y_axis.scale.to_normalized(0.0, y_range.min, y_range.max), 0.0, 1.0);
            const f32 x_zero_px = plot_size.x * static_cast<f32>(x_zero_t);
            const f32 y_zero_px = plot_size.y * (1.0f - static_cast<f32>(y_zero_t));

            paths.push_back(StrokePath{
                .points = {{x_zero_px, 0.0f}, {x_zero_px, plot_size.y}},
                .style = [&] { StrokeStyle s = desc.y_axis.axis_stroke; s.color = desc.y_axis.axis_color; return s; }(),
            });
            paths.push_back(StrokePath{
                .points = {{0.0f, y_zero_px}, {plot_size.x, y_zero_px}},
                .style = [&] { StrokeStyle s = x_axis.axis_stroke; s.color = x_axis.axis_color; return s; }(),
            });

            switch (desc.type) {
                case GraphType::Line:
                case GraphType::Area: {
                    for (const SeriesRef &series : desc.series) {
                        if (series.x.size() != series.y.size() || series.x.size() < 2) {
                            continue;
                        }
                        if (desc.type == GraphType::Area) {
                            append_area_fill(series, desc, x_range, y_range, plot_size, y_zero_px, fills);
                        }
                        vector<glm::vec2> points;
                        points.reserve(series.x.size());
                        for (usize i = 0; i < series.x.size(); ++i) {
                            points.push_back(to_pixel(desc, x_range, y_range, plot_size, series.x[i], series.y[i]));
                        }
                        paths.push_back(StrokePath{
                            .points = std::move(points),
                            .style = StrokeStyle{.color = series.color, .width = series.line_width,
                                                 .feather_px = series.feather_px, .glow_intensity = series.glow_intensity},
                        });
                    }
                    break;
                }
                case GraphType::Scatter: {
                    for (const SeriesRef &series : desc.series) {
                        if (series.x.size() != series.y.size()) {
                            continue;
                        }
                        const f32 radius = series.marker_radius > 0.0f ? series.marker_radius : 3.0f;
                        const StrokeStyle marker_style{.color = series.color, .width = radius * 2.0f, .feather_px = series.feather_px};
                        for (usize i = 0; i < series.x.size(); ++i) {
                            const glm::vec2 p = to_pixel(desc, x_range, y_range, plot_size, series.x[i], series.y[i]);
                            // A zero-length (degenerate) segment renders as a filled circle — see
                            // ui_stroke.slang's own doc comment.
                            paths.push_back(StrokePath{.points = {p, p}, .style = marker_style});
                        }
                    }
                    break;
                }
                case GraphType::Bar: {
                    const f32 slot_width = category_count > 0 ? plot_size.x / static_cast<f32>(category_count) : 0.0f;
                    const f32 group_gap = slot_width * desc.bar_group_gap_fraction;
                    const f32 usable_width = std::max(slot_width - group_gap, 1.0f);
                    const usize series_count = desc.series.size();

                    if (desc.bar_stack_mode == BarStackMode::Grouped) {
                        const f32 series_gap = series_count > 1 ? usable_width * desc.bar_series_gap_fraction : 0.0f;
                        const f32 bar_width = std::max(
                            (usable_width - series_gap * static_cast<f32>(series_count > 0 ? series_count - 1 : 0)) /
                                static_cast<f32>(std::max<usize>(series_count, 1)),
                            1.0f);
                        for (usize c = 0; c < category_count; ++c) {
                            const f32 slot_left = slot_width * static_cast<f32>(c) + group_gap * 0.5f;
                            for (usize si = 0; si < series_count; ++si) {
                                const SeriesRef &series = desc.series[si];
                                if (c >= series.y.size()) {
                                    continue;
                                }
                                const glm::vec2 value_px = to_pixel(desc, x_range, y_range, plot_size, 0.0, series.y[c]);
                                const f32 bar_x = slot_left + static_cast<f32>(si) * (bar_width + series_gap);
                                const f32 top = std::min(value_px.y, y_zero_px);
                                const f32 height = std::abs(y_zero_px - value_px.y);
                                fills.push_back(FillQuad{.position = {bar_x, top}, .size = {bar_width, height}, .color = series.color});
                            }
                        }
                    } else {
                        for (usize c = 0; c < category_count; ++c) {
                            const f32 slot_left = slot_width * static_cast<f32>(c) + group_gap * 0.5f;
                            f64 pos_cursor = 0.0;
                            f64 neg_cursor = 0.0;
                            for (const SeriesRef &series : desc.series) {
                                if (c >= series.y.size()) {
                                    continue;
                                }
                                const f64 value = series.y[c];
                                f64 base;
                                f64 top_value;
                                if (value >= 0.0) {
                                    base = pos_cursor;
                                    top_value = pos_cursor + value;
                                    pos_cursor = top_value;
                                } else {
                                    base = neg_cursor;
                                    top_value = neg_cursor + value;
                                    neg_cursor = top_value;
                                }
                                const f32 base_px = to_pixel(desc, x_range, y_range, plot_size, 0.0, base).y;
                                const f32 top_px = to_pixel(desc, x_range, y_range, plot_size, 0.0, top_value).y;
                                const f32 top = std::min(base_px, top_px);
                                const f32 height = std::abs(base_px - top_px);
                                fills.push_back(FillQuad{.position = {slot_left, top}, .size = {usable_width, height}, .color = series.color});
                            }
                        }
                    }
                    break;
                }
                case GraphType::Pie:
                    break; // handled by draw_pie_chart(), not reached here
            }

            // Every path/fill above was built in plot-local space ([0, plot_size]); offset by
            // plot_origin so they land correctly within the widget's own local space, which is what
            // stroke_paths()/fill_quads() resolve against (see their own doc comments).
            for (StrokePath &path : paths) {
                for (glm::vec2 &point : path.points) {
                    point += plot_origin;
                }
            }
            for (FillQuad &fill : fills) {
                fill.position += plot_origin;
            }

            if (!fills.empty()) {
                ctx.fill_quads(ElementDecl{
                                  .sizing = {SizingAxis::fixed(widget_size.x), SizingAxis::fixed(widget_size.y)},
                                  .id = UString{decl.id.cpp_string() + "-fill"},
                              },
                              fills);
            }
            ctx.stroke_paths(ElementDecl{
                                 .sizing = {SizingAxis::fixed(widget_size.x), SizingAxis::fixed(widget_size.y)},
                                 .id = UString{decl.id.cpp_string() + "-plot"},
                             },
                             paths);

            draw_tick_labels(ctx, desc, desc.y_axis, y_range, y_ticks, plot_origin, plot_size, true);
            draw_tick_labels(ctx, desc, x_axis, x_range, x_ticks, plot_origin, plot_size, false);

            const TextStyle title_style{.color = desc.y_axis.axis_color, .font_id = desc.font_id, .font_size = desc.title_font_size};
            if (!x_axis.title.empty()) {
                (void)ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::CenterTop,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {plot_origin.x + plot_size.x * 0.5f,
                                  plot_origin.y + plot_size.y + desc.label_font_size + 8.0f},
                        .capture_pointer = false,
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                });
                ctx.text(ustr{std::string_view{x_axis.title}}, title_style);
            }
            // A conventional rotated (vertical) Y-axis title isn't feasible here — Clay has no text
            // rotation — so this sits horizontally above the Y axis instead of alongside it.
            if (!desc.y_axis.title.empty()) {
                (void)ctx.element(ElementDecl{
                    .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                    .floating = FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::LeftBottom,
                        .parent_attach_point = FloatingAttachPoint::LeftTop,
                        .offset = {2.0f, plot_origin.y - 2.0f},
                        .capture_pointer = false,
                        .clip_to = FloatingClipTo::AttachedParent,
                    },
                });
                ctx.text(ustr{std::string_view{desc.y_axis.title}}, title_style);
            }

            draw_legend(ctx, desc, decl.id.cpp_string());
        }

        /// Draws a Pie/donut chart into `widget_size`-sized local space via fill_sectors().
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param decl `decl` value used by the operation.
        /// @param desc Description of the resource or operation to perform.
        /// @param widget_size Requested or available size for the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_pie_chart(Context &ctx, const ElementDecl &decl, const GraphDesc &desc, glm::vec2 widget_size) {
            f64 total = 0.0;
            for (const PieSlice &slice : desc.pie_slices) {
                total += std::max(slice.value, 0.0);
            }
            if (total <= 0.0) {
                return;
            }

            const glm::vec2 center = widget_size * 0.5f;
            const f32 outer_radius = std::max(std::min(widget_size.x, widget_size.y) * 0.5f - 4.0f, 1.0f);
            const f32 inner_radius = outer_radius * std::clamp(desc.pie_style.hole_ratio, 0.0f, 0.95f);
            const f32 gap_radians = desc.pie_style.gap_degrees * (std::numbers::pi_v<f32> / 180.0f);
            const f32 start_radians = desc.pie_style.start_angle_degrees * (std::numbers::pi_v<f32> / 180.0f);

            vector<Sector> sectors;
            sectors.reserve(desc.pie_slices.size());
            f32 angle_cursor = start_radians;
            for (const PieSlice &slice : desc.pie_slices) {
                const f64 value = std::max(slice.value, 0.0);
                if (value <= 0.0) {
                    continue;
                }
                const f32 span = static_cast<f32>(value / total) * 2.0f * std::numbers::pi_v<f32>;
                const f32 slice_start = angle_cursor + gap_radians * 0.5f;
                const f32 slice_end = angle_cursor + span - gap_radians * 0.5f;
                angle_cursor += span;
                if (slice_end <= slice_start) {
                    continue; // the gap ate the whole slice (many tiny slices + a large gap)
                }
                sectors.push_back(Sector{
                    .center = center,
                    .inner_radius = inner_radius,
                    .outer_radius = outer_radius,
                    .start_angle = slice_start,
                    .end_angle = slice_end,
                    .style = SectorStyle{.color = slice.color, .feather_px = desc.pie_style.feather_px},
                });
            }

            ctx.fill_sectors(ElementDecl{
                                 .sizing = {SizingAxis::fixed(widget_size.x), SizingAxis::fixed(widget_size.y)},
                                 .id = UString{decl.id.cpp_string() + "-slices"},
                             },
                             sectors);

            draw_legend(ctx, desc, decl.id.cpp_string());
        }

    } // namespace

    /// Draws a graph/plot widget for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param desc Description of the resource or operation to perform.
    /// @param state `state` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    GraphResult graph(Context &ctx, const ElementDecl &decl, const GraphDesc &desc, GraphState &state) {
        (void)state;

        ElementDecl styled = decl;
        styled.background_color = desc.background_color;
        styled.corner_radius = desc.corner_radius;
        // Clip the plot content to this widget's own box regardless of scale/domain correctness
        // elsewhere — a badly out-of-range point (e.g. from a pathological axis config) should get
        // cut off at the chart's own edge, not bleed into whatever UI happens to sit past it.
        styled.clip = ClipConfig{.horizontal = true, .vertical = true};
        auto scope = ctx.element(styled);

        const optional<ElementBounds> bounds = ctx.element_bounds(decl.id);
        if (!bounds || bounds->size.x <= 0.0f || bounds->size.y <= 0.0f) {
            return GraphResult{.drawn = false};
        }
        const glm::vec2 widget_size = bounds->size;

        if (desc.type == GraphType::Pie) {
            draw_pie_chart(ctx, decl, desc, widget_size);
        } else {
            draw_cartesian_chart(ctx, decl, desc, widget_size);
        }

        return GraphResult{.drawn = true};
    }

} // namespace SFT::UI
