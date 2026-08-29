#include <UiWorkbenchGameLogic/WorkbenchUi.hpp>

#if defined(STURDY_UI_WORKBENCH_HAS_GLFW)
#include <WindowManager/Providers/GLFW/GLFW.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace SFT::UiWorkbench {

    namespace {

        constexpr UI::Color canvas{0.035, 0.041, 0.063, 1.0};
        constexpr UI::Color panel{0.066, 0.075, 0.108, 0.98};
        constexpr UI::Color panel_raised{0.090, 0.101, 0.143, 1.0};
        constexpr UI::Color outline{0.155, 0.179, 0.245, 1.0};
        constexpr UI::Color text_primary{0.930, 0.947, 0.995, 1.0};
        constexpr UI::Color text_secondary{0.590, 0.640, 0.755, 1.0};
        constexpr UI::Color accent{0.390, 0.570, 1.0, 1.0};
        constexpr UI::Color accent_hot{0.630, 0.420, 1.0, 1.0};
        constexpr UI::Color success{0.270, 0.820, 0.620, 1.0};
        constexpr UI::Color warning{1.0, 0.670, 0.260, 1.0};
        constexpr UI::Color danger{0.960, 0.350, 0.380, 1.0};

        // Console log-line virtualization: an approximate single-line row height (12px font, a
        // bit of leading) used only to estimate which lines are actually scrolled into view, so
        // the console can render a bounded number of `draw_text` calls per frame regardless of
        // how many thousand lines are captured. A wrapped (multi-row) line makes this an
        // underestimate for that one row, not a correctness problem — see
        // `WorkbenchUi::build_console_panel`'s doc comment for the full reasoning. Generous
        // padding (`console_visible_padding_lines`) absorbs the resulting drift.
        constexpr f32 console_line_height_px = 16.0f;
        constexpr usize console_visible_padding_lines = 24;

        /// Performs the background with opacity operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        /// @param opacity `opacity` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] UI::Color background_with_opacity(UI::Color color, f64 opacity) noexcept {
            color.a *= std::clamp(opacity, 0.0, 1.0);
            return color;
        }


        /// Converts the value to platform cursor icon representation.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @return Returns the value converted to platform cursor icon representation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowManager::CursorIcon to_platform_cursor_icon(UI::CursorIcon icon) {
            switch (icon) {
                case UI::CursorIcon::Auto:
                case UI::CursorIcon::Default:
                    return WindowManager::CursorIcon::Default;
                case UI::CursorIcon::Pointer:
                    return WindowManager::CursorIcon::Pointer;
                case UI::CursorIcon::Text:
                    return WindowManager::CursorIcon::Text;
                case UI::CursorIcon::Grab:
                    return WindowManager::CursorIcon::Grab;
                case UI::CursorIcon::Grabbing:
                    return WindowManager::CursorIcon::Grabbing;
                case UI::CursorIcon::ResizeHorizontal:
                    return WindowManager::CursorIcon::ResizeHorizontal;
                case UI::CursorIcon::ResizeVertical:
                    return WindowManager::CursorIcon::ResizeVertical;
                case UI::CursorIcon::ResizeNwse:
                    return WindowManager::CursorIcon::ResizeNwse;
                case UI::CursorIcon::ResizeNesw:
                    return WindowManager::CursorIcon::ResizeNesw;
                case UI::CursorIcon::NotAllowed:
                    return WindowManager::CursorIcon::NotAllowed;
            }
            return WindowManager::CursorIcon::Default;
        }


        constexpr glm::vec2 kWorkspaceOrigin{18.0f, 58.0f};

        /// Performs the text style operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param font `font` value used by the operation.
        /// @param color `color` value used by the operation.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::TextStyle text_style(UI::FontId font, UI::Color color = text_primary, u16 size = 14) {
            return UI::TextStyle{
                .color = color,
                .font_id = font,
                .font_size = size,
                .wrap_mode = UI::TextWrapMode::Words,
            };
        }

        /// Performs the number text operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param prefix `prefix` value used by the operation.
        /// @param value Value consumed by the operation.
        /// @param suffix `suffix` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string number_text(const char *prefix, f64 value, const char *suffix = "") {
            std::array<char, 96> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s%.2f%s", prefix, value, suffix);
            return std::string{buffer.data()};
        }

        /// Performs the fps text operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param fps `fps` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string fps_text(f32 fps) {
            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%.0f FPS", static_cast<f64>(std::max(fps, 0.0f)));
            return std::string{buffer.data()};
        }


        /// Performs the fps color operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param fps `fps` value used by the operation.
        /// @param good `good` value used by the operation.
        /// @param mid `mid` value used by the operation.
        /// @param bad `bad` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::Color fps_color(f32 fps, UI::Color good, UI::Color mid, UI::Color bad) {
            if (fps >= 55.0f) {
                return good;
            }
            return fps >= 30.0f ? mid : bad;
        }

        /// Performs the rgba text operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string rgba_text(const UI::Color &color) {
            std::array<char, 128> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "sRGB  %.3f   %.3f   %.3f   alpha %.3f", color.r, color.g, color.b, color.a);
            return std::string{buffer.data()};
        }

        /// Draws text using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param content `content` value used by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_text(UI::Context &ctx, std::string_view content, const UI::TextStyle &style) {
            const ustr borrowed{content};
            ctx.text(borrowed, style);
        }

        /// Returns the current or globally available action button style value.
        ///
        /// @return Returns the current action button style value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::ButtonStyle action_button_style() {
            return UI::ButtonStyle{
                .idle = UI::Color{0.125, 0.145, 0.205, 1.0},
                .hovered = UI::Color{0.190, 0.235, 0.350, 1.0},
                .pressed = UI::Color{0.095, 0.115, 0.175, 1.0},
                .disabled = UI::Color{0.090, 0.095, 0.120, 0.6},
                .corner_radius = UI::CornerRadius::all(9.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
                .transition_seconds = 0.14f,
            };
        }

        /// Returns the current or globally available toggle style value.
        ///
        /// @return Returns the current toggle style value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::ToggleStyle toggle_style() {
            return UI::ToggleStyle{
                .idle = UI::Color{0.120, 0.135, 0.185, 1.0},
                .hovered = UI::Color{0.185, 0.215, 0.300, 1.0},
                .checked = accent,
                .disabled = UI::Color{0.100, 0.105, 0.130, 0.55},
                .mark_color = UI::Color{0.985, 0.990, 1.0, 1.0},
                .transition_seconds = 0.16f,
            };
        }

        /// Performs the dock style operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param font `font` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::Docking::DockWorkspaceStyle dock_style(UI::FontId font) {
            UI::Docking::DockWorkspaceStyle style{};
            style.tab_strip_height = 34.0f;
            style.divider_thickness = 7.0f;
            style.min_leaf_size = 190.0f;
            style.drag_start_threshold = 4.0f;
            style.tab_active_style = UI::ButtonStyle{
                .idle = UI::Color{0.105, 0.125, 0.185, 1.0},
                .hovered = UI::Color{0.145, 0.175, 0.255, 1.0},
                .pressed = UI::Color{0.085, 0.105, 0.165, 1.0},
                .corner_radius = UI::CornerRadius::all(7.0f),
                .transition_seconds = 0.12f,
            };
            style.tab_inactive_style = UI::ButtonStyle{
                .idle = UI::Color{0.055, 0.063, 0.091, 1.0},
                .hovered = UI::Color{0.100, 0.115, 0.165, 1.0},
                .pressed = UI::Color{0.075, 0.085, 0.125, 1.0},
                .corner_radius = UI::CornerRadius::all(7.0f),
                .transition_seconds = 0.12f,
            };
            style.close_button_style = UI::ButtonStyle{
                .idle = UI::Color{0.0, 0.0, 0.0, 0.0},
                .hovered = UI::Color{0.75, 0.20, 0.30, 1.0},
                .pressed = UI::Color{0.50, 0.10, 0.18, 1.0},
                .corner_radius = UI::CornerRadius::all(5.0f),
                .transition_seconds = 0.10f,
            };
            style.divider_style = UI::ButtonStyle{
                .idle = UI::Color{0.040, 0.047, 0.070, 1.0},
                .hovered = accent,
                .pressed = accent_hot,
                .transition_seconds = 0.12f,
            };
            style.content_background = canvas;
            style.drop_guide_fill = UI::Color{accent.r, accent.g, accent.b, 0.28};
            style.drop_guide_border = UI::Color{0.62, 0.74, 1.0, 0.96};
            style.tab_text_color = text_primary;
            style.tab_font_id = font;
            style.tab_font_size = 14;
            return style;
        }

        /// Performs the panel heading operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param font `font` value used by the operation.
        /// @param eyebrow `eyebrow` value used by the operation.
        /// @param title `title` value used by the operation.
        /// @param description Description of the resource or operation to perform.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void panel_heading(UI::Context &ctx, UI::FontId font, std::string_view eyebrow, std::string_view title, std::string_view description) {
            draw_text(ctx, eyebrow, text_style(font, accent, 12));
            draw_text(ctx, title, text_style(font, text_primary, 23));
            draw_text(ctx, description, text_style(font, text_secondary, 13));
        }

        /// Performs the section label operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param font `font` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void section_label(UI::Context &ctx, UI::FontId font, std::string_view label) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .child_gap = 10,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
            });
            draw_text(ctx, label, text_style(font, text_secondary, 12));
            auto line = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(1.0f)},
                .background_color = outline,
            });
            (void)line;
        }

        /// Performs the status pill operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param font `font` value used by the operation.
        /// @param text Text consumed by the operation.
        /// @param color `color` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void status_pill(UI::Context &ctx, UI::FontId font, std::string_view text, UI::Color color) {
            auto pill = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(9, 4),
                .background_color = UI::Color{color.r, color.g, color.b, 0.13},
                .corner_radius = UI::CornerRadius::all(12.0f),
                .border = UI::BorderStyle{.color = UI::Color{color.r, color.g, color.b, 0.45},
                                          .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, text, text_style(font, color, 11));
        }

        /// Performs the dropdown option operation for `UiWorkbench` using the supplied arguments.
        ///
        /// @param font `font` value used by the operation.
        /// @param label `label` value used by the operation.
        /// @param swatch `swatch` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::DropdownOption dropdown_option(UI::FontId font, const char *label, UI::Color swatch) {
            return UI::DropdownOption{.build = [font, label, swatch](UI::Context &ctx) {
                auto row = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fit()},
                    .child_gap = 9,
                    .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                });
                {


                    auto dot = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(10.0f), UI::SizingAxis::fixed(10.0f)},
                        .background_color = swatch,
                        .corner_radius = UI::CornerRadius::all(5.0f),
                    });
                    (void)dot;
                }
                draw_text(ctx, label, text_style(font, text_primary, 13));
            }};
        }

    } // namespace

    struct WorkbenchUi::Surface {
        Surface(Core::RenderSurfaceHandle surface_handle, UI::Context &&ui_context, UI::UiRenderer &&sdr_ui_renderer, UI::UiRenderer &&hdr_ui_renderer, UString workspace_id, UI::Docking::DockWorkspaceStyle workspace_style, bool is_primary)
            : handle(surface_handle),
              context(std::move(ui_context)),
              sdr_renderer(std::move(sdr_ui_renderer)),
              hdr_renderer(std::move(hdr_ui_renderer)),
              workspace(std::move(workspace_id), workspace_style),
              primary(is_primary) {}

        Core::RenderSurfaceHandle handle{};
        UI::Context context{};
        UI::UiRenderer sdr_renderer{};
        UI::UiRenderer hdr_renderer{};
        UI::PointerState pointer{};
        UI::Docking::DockWorkspace workspace;
        bool primary = false;
        std::vector<UI::SliderKey> slider_keys;
        std::vector<UI::ColorPickerKey> color_keys;


        Engine::UiTextInputState text_input;


        UI::ScrollAreaState controls_scroll{};
        UI::ScrollAreaState color_scroll{};
        UI::ScrollAreaState settings_scroll{};
        UI::ScrollAreaState text_scroll{};
        UI::ScrollAreaState docking_scroll{};
        UI::ScrollAreaState metrics_scroll{};
        UI::ScrollAreaState console_scroll{};
        UI::ScrollAreaState strokes_scroll{};
        f32 custom_stroke_time = 0.0f;
        UI::ScrollAreaState graphs_scroll{};
        UI::FrameGraphState frame_graph_state{};
        f32 frame_graph_phase = 0.0f;


        f32 fps_smoothed = -1.0f;


        Renderer::FrameTimingSnapshot last_timing_snapshot{};
    };

    /// Performs the workbench UI operation for `UiWorkbench` using the supplied arguments.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WorkbenchUi::WorkbenchUi() {
        render_graph_ = Engine::RenderGraph::overlay_only();


        render_graph_.debug_overlay().draw_text = false;
    }

    /// Destroys the `UiWorkbench` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    WorkbenchUi::~WorkbenchUi() = default;

    /// Initializes the `UiWorkbench` for use.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Engine::GameLogicResult WorkbenchUi::initialize(Engine::Engine &engine) {
        const Foundation::Stopwatch stopwatch;
        const std::optional<std::string> font_bytes =
            Foundation::read_file_to_string("Fonts/MapleMono-NF-Regular.ttf");
        if (!font_bytes) {
            return std::unexpected(Engine::GameLogicError{
                .message = UString{"UiWorkbench could not read Fonts/MapleMono-NF-Regular.ttf"},
            });
        }
        const std::span<const char> chars{font_bytes->data(), font_bytes->size()};
        auto loaded = Text::Font::load(std::as_bytes(chars));
        if (!loaded) {
            return std::unexpected(Engine::GameLogicError{.message = loaded.error().message});
        }
        font_ = std::move(*loaded);


        const std::optional<std::string> cjk_font_bytes =
            Foundation::read_file_to_string("Fonts/NotoSansMonoCJK-JP-Subset.ttf");
        if (!cjk_font_bytes) {
            return std::unexpected(Engine::GameLogicError{
                .message = UString{"UiWorkbench could not read Fonts/NotoSansMonoCJK-JP-Subset.ttf"},
            });
        }
        const std::span<const char> cjk_chars{cjk_font_bytes->data(), cjk_font_bytes->size()};
        auto cjk_loaded = Text::Font::load(std::as_bytes(cjk_chars));
        if (!cjk_loaded) {
            return std::unexpected(Engine::GameLogicError{.message = cjk_loaded.error().message});
        }
        cjk_font_ = std::move(*cjk_loaded);

        hdr_enabled_ = static_cast<bool>(engine.config().features.presentation.hdr_enabled);
        swapchain_transparent_ =
            static_cast<bool>(engine.config().features.presentation.transparent_composition);
        Foundation::log_info(
            "UiWorkbench: loaded font 'Fonts/MapleMono-NF-Regular.ttf' ({} bytes) + CJK fallback "
            "'Fonts/NotoSansMonoCJK-JP-Subset.ttf' ({} bytes) in {}",
            font_bytes->size(),
            cjk_font_bytes->size(),
            stopwatch.elapsed_human());
        markdown_input_state_.set_text(UString{"# Text Lab\n"
                                               "Edit this buffer and watch the preview follow.\n"
                                               "\n"
                                               "## Supported markdown\n"
                                               "- Headings with # and ##\n"
                                               "- **Bold** inline spans\n"
                                               "- `code` inline spans\n"
                                               "\n"
                                               "Plain paragraphs render as body text."});


        static constexpr std::array<WindowManager::WindowEffectKind, 5> candidate_blur_kinds{
            WindowManager::WindowEffectKind::Blur,
            WindowManager::WindowEffectKind::Acrylic,
            WindowManager::WindowEffectKind::Mica,
            WindowManager::WindowEffectKind::MicaAlt,
            WindowManager::WindowEffectKind::Tabbed,
        };
        for (WindowManager::WindowEffectKind kind : candidate_blur_kinds) {
            if (WindowManager::operating_system_may_support_window_effect(kind)) {
                supported_blur_kinds_.push_back(kind);
            }
        }

        route_input(engine);

        // Demonstrates Foundation::add_log_sink (and, through it, the FFI's sturdy_log_add_sink):
        // captures every engine log message into console_log_lines_ for the Console panel to
        // display, the same mechanism a foreign-language host uses to build its own log viewer.
        console_log_sink_id_ = Foundation::add_log_sink(
            [this](Foundation::LogLevel level, std::string_view message) {
                const std::lock_guard<std::mutex> lock(console_log_mutex_);
                if (console_log_lines_.size() >= console_log_capacity_) {
                    console_log_lines_.pop_front();
                    ++console_log_dropped_;
                }
                console_log_lines_.push_back(ConsoleLine{level, std::string{message}});
            });

        return {};
    }

    /// Finds surface in the available state.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WorkbenchUi::Surface *WorkbenchUi::find_surface(
        WindowManager::WindowId window) noexcept {
        const auto found = surfaces_.find(window);
        return found != surfaces_.end() ? found->second.get() : nullptr;
    }

    /// Finds or creates the surface required by the operation.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param handle Handle identifying the target object or resource.
    /// @param primary `primary` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WorkbenchUi::Surface *WorkbenchUi::ensure_surface(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle handle,
        bool primary) {
        if (Surface *existing = find_surface(handle.window_id)) {
            return existing;
        }
        RHI::RhiDevice *device = engine.rhi_device();
        if (device == nullptr) {
            return nullptr;
        }
        auto context = UI::Context::create(UI::Context::Config{.max_element_count = 16384});
        if (!context) {
            Foundation::log_error("UiWorkbench: failed to create a UI context for a window.");
            return nullptr;
        }
        auto sdr_renderer = UI::UiRenderer::create(*device, RHI::Format::BGRA8UnormSrgb);
        if (!sdr_renderer) {
            Foundation::log_error("UiWorkbench: failed to create the SDR UI renderer for a window.");
            context->destroy();
            return nullptr;
        }


        auto hdr_renderer = UI::UiRenderer::create(*device, RHI::Format::RGBA16Float);
        if (!hdr_renderer) {
            Foundation::log_error("UiWorkbench: failed to create the HDR UI renderer for a window.");
            sdr_renderer->destroy(*device);
            context->destroy();
            return nullptr;
        }
        const usize numeric_window = static_cast<usize>(handle.window_id);
        auto surface = std::make_unique<Surface>(
            handle,
            std::move(*context),
            std::move(*sdr_renderer),
            std::move(*hdr_renderer),
            UString{"ui-workbench-window-" + std::to_string(numeric_window)},
            dock_style(font_id_),
            primary);
        const std::array<const Text::Font *, 1> font_fallbacks{&cjk_font_};
        surface->context.register_font(font_id_, font_,                    nullptr, font_fallbacks);

        Surface *result = surface.get();
        surfaces_.emplace(handle.window_id, std::move(surface));
        dock_coordinator_.register_workspace(handle.window_id, result->workspace, primary, !primary);

        if (primary) {
            // Seed every workbench panel into the same leaf so the initial state is one tab strip.
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"settings"}, .title = UString{"Settings"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"controls"}, .title = UString{"Slider Lab"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"color"}, .title = UString{"Color Studio"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"docking"}, .title = UString{"Docking Guide"}, .closable = false});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"text"}, .title = UString{"Text Lab"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"metrics"}, .title = UString{"Performance"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"console"}, .title = UString{"Console"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"strokes"}, .title = UString{"Strokes"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"graphs"}, .title = UString{"Graphs"}, .closable = true});
        }
        return result;
    }

    /// Destroys the surface renderers identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WorkbenchUi::destroy_surface_renderers(RHI::RhiDevice &device) noexcept {
        for (auto &[window, surface] : surfaces_) {
            (void)window;
            surface->sdr_renderer.destroy(device);
            surface->hdr_renderer.destroy(device);
        }
    }

    /// Recreates surface renderers using the supplied arguments and current state.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult WorkbenchUi::recreate_surface_renderers(RHI::RhiDevice &device) {
        for (auto &[window, surface] : surfaces_) {
            (void)window;
            auto sdr_renderer = UI::UiRenderer::create(device, RHI::Format::BGRA8UnormSrgb);
            if (!sdr_renderer) {
                return std::unexpected(sdr_renderer.error());
            }
            auto hdr_renderer = UI::UiRenderer::create(device, RHI::Format::RGBA16Float);
            if (!hdr_renderer) {
                sdr_renderer->destroy(device);
                return std::unexpected(hdr_renderer.error());
            }
            surface->sdr_renderer = std::move(*sdr_renderer);
            surface->hdr_renderer = std::move(*hdr_renderer);
            if (!surface->sdr_renderer.ready() || !surface->hdr_renderer.ready()) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UiRenderer creation did not produce a ready renderer.");
            }
        }
        return {};
    }

    /// Performs the route input operation for `UiWorkbench` using the supplied arguments.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::route_input(Engine::Engine &engine) {
        engine.update_schedule().add_system(
            [this](Ecs::EventReader<Engine::MouseMoveEvent> moves,
                   Ecs::EventReader<Engine::MouseButtonEvent> buttons,
                   Ecs::EventReader<Engine::MouseWheelEvent> wheels,
                   Ecs::EventReader<Engine::KeyboardEvent> keys,
                   Ecs::EventReader<Engine::TextInputEvent> text_events,
                   Ecs::EventReader<Engine::TextEditingEvent> text_editing_events,
                   Ecs::EventReader<Engine::WindowStateEvent> window_events) noexcept {
                for (const Engine::TextInputEvent &event : text_events.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->text_input.apply(event);
                    }
                }
                for (const Engine::TextEditingEvent &event : text_editing_events.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->text_input.apply(event);
                    }
                }
                for (const Engine::MouseMoveEvent &event : moves.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->pointer.position = {event.mouse.x, event.mouse.y};
                    }
                }
                for (const Engine::MouseButtonEvent &event : buttons.read()) {
                    if (event.mouse.button_code != WindowManager::MouseButton::Left) {
                        continue;
                    }
                    if (Surface *surface = find_surface(event.window)) {
                        surface->pointer.position = {event.mouse.x, event.mouse.y};
                        if (event.action == Engine::ButtonAction::Pressed) {
                            surface->pointer.pressed = true;
                            surface->pointer.press_position = surface->pointer.position;
                            surface->pointer.down = true;
                        } else {
                            surface->pointer.released = true;
                            surface->pointer.down = false;
                        }
                    }
                }
                for (const Engine::MouseWheelEvent &event : wheels.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->pointer.position = {event.wheel.mouse_x, event.wheel.mouse_y};


                        surface->pointer.scroll_delta += glm::vec2{-event.wheel.x, event.wheel.y};
                    }
                }
                for (const Engine::WindowStateEvent &event : window_events.read()) {
                    if (event.kind == WindowManager::WindowEventKind::FocusLost ||
                        event.kind == WindowManager::WindowEventKind::MouseLeft) {
                        if (Surface *surface = find_surface(event.window)) {
                            surface->pointer.cancelled = surface->pointer.down;
                            surface->pointer.down = false;
                        }
                    }
                }
                for (const Engine::KeyboardEvent &event : keys.read()) {
                    Surface *surface = find_surface(event.window);
                    if (surface == nullptr) {
                        continue;
                    }


                    surface->text_input.apply_key(event);


                    if (event.key_code == Engine::KeyboardKey::LeftShift ||
                        event.key_code == Engine::KeyboardKey::RightShift ||
                        event.key_code == Engine::KeyboardKey::LeftControl ||
                        event.key_code == Engine::KeyboardKey::RightControl) {
                        continue;
                    }
                    if (!event.pressed()) {
                        continue;
                    }
                    switch (event.key_code) {
                        case Engine::KeyboardKey::Left:
                            surface->slider_keys.push_back(UI::SliderKey::Decrement);
                            surface->color_keys.push_back(UI::ColorPickerKey::Left);
                            break;
                        case Engine::KeyboardKey::Right:
                            surface->slider_keys.push_back(UI::SliderKey::Increment);
                            surface->color_keys.push_back(UI::ColorPickerKey::Right);
                            break;
                        case Engine::KeyboardKey::Down:
                            surface->slider_keys.push_back(UI::SliderKey::Decrement);
                            surface->color_keys.push_back(UI::ColorPickerKey::Down);
                            break;
                        case Engine::KeyboardKey::Up:
                            surface->slider_keys.push_back(UI::SliderKey::Increment);
                            surface->color_keys.push_back(UI::ColorPickerKey::Up);
                            break;
                        case Engine::KeyboardKey::PageDown:
                            surface->slider_keys.push_back(UI::SliderKey::PageDecrement);
                            surface->color_keys.push_back(UI::ColorPickerKey::PageDecrement);
                            break;
                        case Engine::KeyboardKey::PageUp:
                            surface->slider_keys.push_back(UI::SliderKey::PageIncrement);
                            surface->color_keys.push_back(UI::ColorPickerKey::PageIncrement);
                            break;
                        case Engine::KeyboardKey::Home:
                            surface->slider_keys.push_back(UI::SliderKey::Minimum);
                            surface->color_keys.push_back(UI::ColorPickerKey::Minimum);
                            break;
                        case Engine::KeyboardKey::End:
                            surface->slider_keys.push_back(UI::SliderKey::Maximum);
                            surface->color_keys.push_back(UI::ColorPickerKey::Maximum);
                            break;
                        default:
                            break;
                    }
                }
            });
    }

    /// Performs the process window completions operation for `UiWorkbench` using the supplied arguments.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::process_window_completions(Engine::Engine &engine) {
        for (const Engine::WindowRequestCompletion &completion :
             engine.window_requests().take_completions()) {
            const bool handled = dock_coordinator_.resolve_completion(
                completion,
                [this, &engine](Core::RenderSurfaceHandle handle,
                                const UI::Docking::DockPanelDesc &) -> UI::Docking::DockWorkspace * {
                    Surface *surface = ensure_surface(engine, handle, false);
                    if (surface == nullptr) {
                        (void)engine.window_requests().close(handle.window_id);
                        return nullptr;
                    }
                    return &surface->workspace;
                });
            if (handled && completion.kind == Engine::WindowRequestKind::Spawn) {
                status_message_ = completion.accepted
                                      ? "Tear-off complete — the panel now owns an independent UI surface."
                                      : "The OS window request failed; the panel stayed safely docked.";
            }
            if (completion.kind == Engine::WindowRequestKind::RecreatePrimary) {
                status_message_ = completion.accepted
                                      ? "Primary window recreated."
                                      : "Primary window recreation failed: " +
                                            completion.message.to_std_string_unchecked();
                if (completion.accepted) {
                    primary_window_ = completion.window;
                }
            }
            if (completion.kind == Engine::WindowRequestKind::Close && completion.accepted &&
                (!primary_window_ || completion.window != *primary_window_)) {
                if (auto found = surfaces_.find(completion.window); found != surfaces_.end()) {
                    if (RHI::RhiDevice *device = engine.rhi_device()) {
                        found->second->sdr_renderer.destroy(*device);
                        found->second->hdr_renderer.destroy(*device);
                    }
                    found->second->context.destroy();
                    surfaces_.erase(found);
                }
            }
        }
    }

    /// Renders the requested content using the current rendering state.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param handle Handle identifying the target object or resource.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<Engine::RenderFrameParameters> WorkbenchUi::render(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle handle,
        const Core::FrameInput &frame) {
        process_window_completions(engine);
        if (!primary_window_) {
            primary_window_ = handle.window_id;
        }
        Surface *surface = ensure_surface(engine, handle, handle.window_id == *primary_window_);
        if (surface == nullptr || frame.framebuffer_width == 0 || frame.framebuffer_height == 0) {
            return std::nullopt;
        }


        if (!surface->sdr_renderer.ready() || !surface->hdr_renderer.ready()) {
            if (ui_renderer_rebuild_failed_) {
                return std::nullopt;
            }
            RHI::RhiDevice *device = engine.rhi_device();
            if (device == nullptr) {
                return std::nullopt;
            }
            if (auto ui_rebuilt = recreate_surface_renderers(*device); !ui_rebuilt) {
                ui_renderer_rebuild_failed_ = true;
                Foundation::log_error("UiWorkbench: failed to recreate UI renderers after graphics reconstruction: {}",
                                      ui_rebuilt.error().message);
                return std::nullopt;
            }
            ui_renderer_rebuild_failed_ = false;
        }

        if (Renderer::Renderer *renderer = engine.renderer(); renderer != nullptr) {
            surface->last_timing_snapshot = renderer->last_frame_timings(handle);
        }

        const glm::vec2 viewport{static_cast<f32>(frame.framebuffer_width),
                                 static_cast<f32>(frame.framebuffer_height)};
        UI::Docking::DockWorkspaceEvents dock_events =
            build_frame(engine, *surface, viewport, static_cast<f32>(frame.delta_seconds));
        handle_dock_events(engine, *surface, std::move(dock_events));


        if (frames_to_skip_after_graphics_reconstruction_ != 0) {
            --frames_to_skip_after_graphics_reconstruction_;
            (void)surface->context.finish_frame();
            return std::nullopt;
        }


        engine.window_requests().set_cursor_icon(surface->handle.window_id,
                                                 to_platform_cursor_icon(surface->context.desired_cursor()));
        auto snapshot = std::make_shared<UI::FrameSnapshot>(surface->context.finish_frame());
        Renderer::UiOverlayHooks overlay = build_overlay_hooks(engine, *surface, snapshot);
        return Engine::RenderFrameParameters{
            .camera = {},
            .lighting = {},
            .render_graph = render_graph_,
            .ui_overlay = std::move(overlay),
            .debug_label = UString{"UiWorkbench"},
        };
    }

    /// Builds frame.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param viewport `viewport` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UI::Docking::DockWorkspaceEvents WorkbenchUi::build_frame(
        Engine::Engine &engine,
        Surface &surface,
        glm::vec2 viewport,
        f32 delta_seconds) {
        UI::PointerState framebuffer_pointer = surface.pointer;
        if (const Engine::WindowSnapshot *window = engine.window_state().find(surface.handle.window_id)) {


            const glm::vec2 framebuffer_size{window->framebuffer_size};
            if (framebuffer_size.x > 0.0f && framebuffer_size.y > 0.0f) {
                const glm::vec2 snapshot_to_framebuffer = viewport / framebuffer_size;
                framebuffer_pointer.position *= snapshot_to_framebuffer;
                if (framebuffer_pointer.press_position) {
                    *framebuffer_pointer.press_position *= snapshot_to_framebuffer;
                }
            }
        }


        if (const std::optional<UI::ElementBounds> bounds =
                surface.context.element_bounds(UString{"workbench-text-markdown"});
            bounds && framebuffer_pointer.position.x >= bounds->position.x &&
            framebuffer_pointer.position.y >= bounds->position.y &&
            framebuffer_pointer.position.x < bounds->position.x + bounds->size.x &&
            framebuffer_pointer.position.y < bounds->position.y + bounds->size.y) {
            const UString text_area_id{"workbench-text-markdown"};
            const UI::Context::ScrollMetrics metrics = surface.context.scroll_metrics(text_area_id);
            if (metrics.found) {
                glm::vec2 offset = metrics.offset;
                const glm::vec2 max_scroll = glm::max(metrics.content_size - metrics.container_size, glm::vec2{0.0f});
                const auto route_axis = [](f32 delta, f32 maximum, f32 &axis_offset) {
                    if (delta == 0.0f || maximum <= 0.0f) {
                        return false;
                    }
                    const f32 routed = std::clamp(axis_offset + delta * 30.0f, -maximum, 0.0f);
                    if (std::abs(routed - axis_offset) <= 0.001f) {
                        return false;
                    }
                    axis_offset = routed;
                    return true;
                };
                if (metrics.horizontal && route_axis(framebuffer_pointer.scroll_delta.x, max_scroll.x, offset.x)) {
                    framebuffer_pointer.scroll_delta.x = 0.0f;
                }
                if (metrics.vertical && route_axis(framebuffer_pointer.scroll_delta.y, max_scroll.y, offset.y)) {
                    framebuffer_pointer.scroll_delta.y = 0.0f;
                }
                surface.context.set_scroll_offset(text_area_id, offset);
            }
        }
        surface.context.begin_layout(viewport, framebuffer_pointer, delta_seconds);
        surface.pointer.pressed = false;
        surface.pointer.press_position.reset();
        surface.pointer.released = false;
        surface.pointer.cancelled = false;
        surface.pointer.scroll_delta = glm::vec2{0.0f};

        if (delta_seconds > 0.0f) {
            const f32 instantaneous_fps = 1.0f / delta_seconds;


            const f32 blend = std::min(1.0f, 6.0f * delta_seconds);
            surface.fps_smoothed =
                surface.fps_smoothed < 0.0f ? instantaneous_fps : surface.fps_smoothed + (instantaneous_fps - surface.fps_smoothed) * blend;
        }

        {


            auto background = surface.context.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(viewport.x), UI::SizingAxis::fixed(viewport.y)},
                .padding = UI::Padding::symmetric(18, 12),
                .child_gap = 10,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Top},
                .background_color = background_with_opacity(canvas, effective_background_opacity()),
            });
            {
                auto brand = surface.context.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(34.0f), UI::SizingAxis::fixed(34.0f)},
                    .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                    .background_color = accent,
                    .corner_radius = UI::CornerRadius::all(10.0f),
                });
                draw_text(surface.context, "S", text_style(font_id_, UI::Color{1.0, 1.0, 1.0, 1.0}, 18));
            }
            {
                auto title = surface.context.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                    .direction = UI::LayoutDirection::TopToBottom,
                });
                draw_text(surface.context, "Sturdy UI Workbench", text_style(font_id_, text_primary, 16));
                draw_text(surface.context, "composable controls · perceptual color · live docking", text_style(font_id_, text_secondary, 10));
            }
            {


                auto spacer = surface.context.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(1.0f)}});
                (void)spacer;
            }
            if (surface.fps_smoothed >= 0.0f) {
                status_pill(surface.context, font_id_, fps_text(surface.fps_smoothed), fps_color(surface.fps_smoothed, success, warning, danger));
            }
            status_pill(surface.context, font_id_, surface.primary ? "PRIMARY SURFACE" : "DETACHED SURFACE", surface.primary ? success : accent_hot);
        }

        const glm::vec2 workspace_size{std::max(viewport.x - 36.0f, 1.0f),
                                       std::max(viewport.y - 72.0f, 1.0f)};
        surface.context.set_scroll_settings(UI::ScrollSettings{
            .click_and_drag_scroll = scroll_click_drag_,
            .smooth_scrolling = scroll_smooth_,
            .smoothing_rate = static_cast<f32>(scroll_smoothing_rate_),
        });
        surface.workspace.set_content_background(
            background_with_opacity(canvas, effective_background_opacity()));
        surface.workspace.begin_frame(
            surface.context,
            UI::Docking::DockRect{.origin = kWorkspaceOrigin, .size = workspace_size},
            delta_seconds);


        if (auto decl = surface.workspace.panel_content_region(UString{"controls"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.controls_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_controls_panel(surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"color"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.color_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_color_panel(surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"settings"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.settings_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_settings_panel(engine, surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"text"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.text_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_text_panel(engine, surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"docking"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.docking_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_docking_panel(surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"metrics"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.metrics_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_metrics_panel(surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"console"})) {
            usize console_line_count = 0;
            {
                const std::lock_guard<std::mutex> lock(console_log_mutex_);
                console_line_count = console_log_lines_.size();
            }
            // Estimate which lines are actually scrolled into view from *last* frame's metrics
            // (one-frame-stale, same contract the autoscroll logic below already relies on) so
            // build_console_panel only builds text elements for those, not every captured line —
            // see its own doc comment for why. Padded generously on both sides so a fast scroll
            // or an underestimated wrapped-line height doesn't pop lines in visibly.
            usize first_visible_line = 0;
            usize visible_line_count = console_line_count;
            {
                const UI::Context::ScrollMetrics metrics = surface.context.scroll_metrics(decl->id);
                if (metrics.found && metrics.vertical) {
                    const f32 first_visible_f = (-metrics.offset.y) / console_line_height_px;
                    const usize first_estimate = first_visible_f > 0.0f ? static_cast<usize>(first_visible_f) : 0;
                    first_visible_line =
                        first_estimate > console_visible_padding_lines ? first_estimate - console_visible_padding_lines : 0;
                    const usize visible_rows =
                        static_cast<usize>(std::ceil(metrics.container_size.y / console_line_height_px)) +
                        2 * console_visible_padding_lines;
                    visible_line_count = visible_rows;
                }
            }
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.console_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_console_panel(surface, ctx, delta_seconds, first_visible_line, visible_line_count);
            });
            // Forcing the scroll position every single frame (the bug this replaced) fights any
            // scroll the user is doing on every frame that follows, which is indistinguishable
            // from "scrolling doesn't work" — only re-pin to the bottom on the frame new content
            // actually arrives (a Clear also changes the count, resetting the tracked baseline),
            // the same "tail -f" behavior any log console uses. All other frames leave the scroll
            // position alone.
            if (console_autoscroll_ && console_line_count != console_lines_at_last_autoscroll_) {
                console_lines_at_last_autoscroll_ = console_line_count;
                // One-frame-stale metrics, the same contract `Context::scroll_metrics`'s own users
                // rely on elsewhere in this file — enough to land on the bottom as of this new
                // line without needing this frame's just-built layout.
                //
                // Clay's scroll offset is 0 at the top and *negative* going down (an element's
                // screen position is `base + scrollOffset`, so a negative offset shifts content up
                // on screen, revealing later/lower content) — clamped to
                // [container_size - content_size, 0] by `Context::set_scroll_offset` itself.
                const UI::Context::ScrollMetrics metrics = surface.context.scroll_metrics(decl->id);
                if (metrics.found && metrics.vertical) {
                    const f32 bottom_offset_y = std::min(0.0f, metrics.container_size.y - metrics.content_size.y);
                    (void)surface.context.set_scroll_offset(decl->id, glm::vec2{metrics.offset.x, bottom_offset_y});
                }
            }
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"strokes"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.strokes_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_strokes_panel(surface, ctx, delta_seconds);
            });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"graphs"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.graphs_scroll, delta_seconds, [&](UI::Context &ctx) {
                build_graphs_panel(surface, ctx, delta_seconds);
            });
        }

        UI::Docking::DockWorkspaceEvents events = surface.workspace.end_frame(surface.context);
        surface.slider_keys.clear();
        surface.color_keys.clear();
        surface.text_input.clear_transitions();
        return events;
    }

    /// Builds controls panel.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_controls_panel(Surface &surface, UI::Context &ctx, f32                  ) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 17,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-controls-body"},
        });
        panel_heading(ctx, font_id_, "RANGE INPUT", "Slider Lab", "Track-click, drag, focus, tick, reverse and keyboard semantics in engine-native UI.");

        {
            auto badges = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .child_gap = 7,
            });
            status_pill(ctx, font_id_, "STEP 0.5", accent);
            status_pill(ctx, font_id_, "HOME / END", success);
            status_pill(ctx, font_id_, "PAGE ±10", warning);
        }

        section_label(ctx, font_id_, "EXPOSURE — CUSTOM COMPOSITION");
        draw_text(ctx, number_text("Current value  ", exposure_, "%"), text_style(font_id_, text_primary, 14));

        const std::array ticks{
            UI::SliderTick{.value = 0.0},
            UI::SliderTick{.value = 25.0},
            UI::SliderTick{.value = 50.0},
            UI::SliderTick{.value = 75.0},
            UI::SliderTick{.value = 100.0},
        };
        UI::SliderConfig config{
            .min = 0.0,
            .max = 100.0,
            .step = 0.5,
            .keyboard_step = 0.5,
            .page_step = 10.0,
            .orientation = UI::SliderOrientation::Horizontal,
            .reversed = false,
            .ticks = ticks,
        };
        UI::SliderStyle style{
            .track = UI::Color{0.115, 0.130, 0.180, 1.0},
            .track_disabled = UI::Color{0.090, 0.095, 0.115, 0.55},
            .fill = selected_color_,
            .fill_disabled = UI::Color{0.190, 0.210, 0.260, 0.45},
            .thumb = text_primary,
            .thumb_hovered = UI::Color{1.0, 1.0, 1.0, 1.0},
            .thumb_dragging = accent_hot,
            .thumb_disabled = UI::Color{0.50, 0.52, 0.58, 0.55},
            .tick = UI::Color{0.35, 0.39, 0.50, 1.0},
            .track_thickness = 8.0f,
            .thumb_size = 22.0f,
            .tick_thickness = 1.0f,
            .tick_length = 10.0f,


            .focused_border = UI::BorderStyle{},
        };
        UI::SliderComposition composition{};
        composition.track.enabled = slider_enabled_;
        composition.marker.visible = show_slider_markers_;
        composition.marker_label.visible = show_slider_markers_;
        composition.marker_label.build = [](UI::Context &part_ctx,
                                            const UI::SliderPartContext &part) {
            if (part.marker_value) {
                draw_text(part_ctx, number_text("", *part.marker_value), text_style(font_id_, text_secondary, 9));
            }
        };
        composition.thumb.alter_decl = [this](UI::ElementDecl &decl,
                                              const UI::SliderPartContext &part) {
            if (!use_custom_thumb_) {
                return;
            }
            decl.sizing = {UI::SizingAxis::fixed(26.0f), UI::SizingAxis::fixed(26.0f)};
            decl.corner_radius = UI::CornerRadius::all(8.0f);


            decl.child_alignment = {.x = UI::AlignX::Center, .y = UI::AlignY::Center};
            decl.background_color = part.visual.active ? accent_hot : text_primary;

        };
        composition.thumb.build = [this](UI::Context &part_ctx,
                                         const UI::SliderPartContext &) {
            if (!use_custom_thumb_) {
                return;
            }
            auto core = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(8.0f), UI::SizingAxis::fixed(8.0f)},
                .background_color = accent,
                .corner_radius = UI::CornerRadius::all(4.0f),
            });
            (void)core;
        };
        composition.tooltip.visible = true;
        composition.tooltip.build = [this](UI::Context &part_ctx,
                                           const UI::SliderPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(9, 5),
                .background_color = background_with_opacity(panel_raised, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(7.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(part_ctx, number_text("", part.value, "%"), text_style(font_id_, text_primary, 12));
        };

        const UI::SliderResult exposure_result = UI::slider(
            ctx,
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(340.0f), UI::SizingAxis::fixed(42.0f)},
                .id = UString{"workbench-exposure"},
            },
            config,
            style,
            exposure_slider_state_,
            exposure_,
            UI::SliderInput{.keys = surface.slider_keys},
            slider_enabled_,
            composition);
        exposure_ = exposure_result.value;

        section_label(ctx, font_id_, "CONTINUOUS + REVERSED VERTICAL");
        {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .child_gap = 18,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
            });
            const UI::SliderConfig vertical_config{
                .min = -180.0,
                .max = 180.0,
                .step = std::nullopt,
                .keyboard_step = 1.0,
                .page_step = 30.0,
                .orientation = UI::SliderOrientation::Vertical,
                .reversed = true,
            };
            UI::SliderStyle vertical_style = style;
            vertical_style.fill = accent_hot;
            vertical_style.thumb_size = 20.0f;
            const UI::SliderResult rotation_result = UI::slider(
                ctx,
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(46.0f), UI::SizingAxis::fixed(170.0f)},
                    .id = UString{"workbench-rotation"},
                },
                vertical_config,
                vertical_style,
                rotation_slider_state_,
                rotation_,
                UI::SliderInput{.keys = surface.slider_keys},
                slider_enabled_);
            rotation_ = rotation_result.value;
            {
                auto details = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(250.0f), UI::SizingAxis::fit()},
                    .padding = UI::Padding::all(14),
                    .child_gap = 7,
                    .direction = UI::LayoutDirection::TopToBottom,
                    .background_color = background_with_opacity(panel, effective_background_opacity()),
                    .corner_radius = UI::CornerRadius::all(12.0f),
                    .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
                });
                draw_text(ctx, number_text("Rotation  ", rotation_, "°"), text_style(font_id_, text_primary, 15));
                draw_text(ctx, "step=any · min at top · full captured drag", text_style(font_id_, text_secondary, 11));
                draw_text(ctx, "Click either track, then use arrows, Page Up/Down, Home or End.", text_style(font_id_, text_secondary, 11));
            }
        }
    }

    /// Builds color panel.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 14,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-color-body"},
        });
        panel_heading(ctx, font_id_, "FOUNDATION COLOR", "Color Studio", "Pick visually, preserve alpha, then inspect the selected strongly typed color space.");

        UI::ColorPickerConfig config{
            .show_color_space_dropdown = true,
            .show_alpha = show_alpha_,
            .show_preview = show_preview_,
            .keyboard_step = 0.01,
            .page_step = 0.1,
        };
        UI::ColorPickerStyle style{};
        style.plane_size = {250.0f, 170.0f};
        style.bar_height = 18.0f;
        style.preview_height = 34.0f;
        style.color_space_dropdown_height = 34.0f;
        style.gap = 10;
        style.plane_cursor_size = 16.0f;
        style.bar_cursor_width = 5.0f;
        style.cursor_color = text_primary;
        style.cursor_shadow = UI::Color{0.0, 0.0, 0.0, 0.85};
        style.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
        style.focused_border = UI::BorderStyle{.color = accent, .width = UI::BorderWidth::all(2)};
        style.color_space_text = text_style(font_id_, text_primary, 13);
        style.color_space_text.wrap_mode = UI::TextWrapMode::None;
        style.color_space_dropdown.trigger = action_button_style();


        style.color_space_dropdown.list_background = style.color_space_dropdown.trigger.idle;
        style.color_space_dropdown.option_hovered = UI::Color{0.16, 0.19, 0.28, 1.0};
        style.color_space_dropdown.corner_radius = UI::CornerRadius::all(10.0f);
        style.color_space_dropdown.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
        style.color_space_dropdown.arrow_color = accent;
        style.color_space_dropdown.arrow_font_id = font_id_;
        style.color_space_dropdown.arrow_font_size = 15;

        UI::ColorPickerComposition composition{};
        composition.dropdown.visual.focused.border =
            UI::BorderStyle{.color = accent, .width = UI::BorderWidth::all(1)};
        composition.saturation_value.alter_decl = [](UI::ElementDecl &decl,
                                                     const UI::ColorPickerPartContext &) {
            decl.corner_radius = UI::CornerRadius::all(12.0f);
        };
        composition.preview.alter_decl = [](UI::ElementDecl &decl,
                                            const UI::ColorPickerPartContext &) {
            decl.corner_radius = UI::CornerRadius::all(10.0f);
        };
        composition.saturation_value_marker.alter_decl = [](UI::ElementDecl &decl,
                                                            const UI::ColorPickerPartContext &part) {
            decl.sizing = {UI::SizingAxis::fixed(part.visual.active ? 19.0f : 16.0f),
                           UI::SizingAxis::fixed(part.visual.active ? 19.0f : 16.0f)};
        };
        composition.tooltip.visible = true;
        composition.tooltip.build = [this](UI::Context &part_ctx,
                                           const UI::ColorPickerPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(9, 5),
                .background_color = background_with_opacity(panel_raised, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(7.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(part_ctx, rgba_text(part.color), text_style(font_id_, text_primary, 10));
        };

        const UI::ColorPickerResult result = UI::color_picker(
            ctx,
            UString{"workbench-color-picker"},
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(14),
                .child_gap = 10,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(14.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            },
            config,
            style,
            color_picker_state_,
            selected_color_,
            UI::ColorPickerInput{.keys = surface.color_keys, .delta_seconds = delta_seconds},
            picker_enabled_,
            composition);
        selected_color_ = result.color;
        selected_color_value_ = result.value;
        selected_color_space_ = result.color_space;

        {
            auto value_card = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(13),
                .child_gap = 6,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = background_with_opacity(panel_raised, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, "Typed Foundation value", text_style(font_id_, accent, 11));


            const std::span<const UI::ColorPickerComponent> components =
                UI::color_picker_components(selected_color_space_);
            const std::array<f64, 4> values = UI::color_picker_component_values(selected_color_value_);
            std::array<char, 160> typed_line{};
            std::snprintf(typed_line.data(), typed_line.size(), "%s   %s %.3f   %s %.3f   %s %.3f   alpha %.3f", UI::color_picker_space_name(selected_color_space_), components[0].label, values[0], components[1].label, values[1], components[2].label, values[2], values[3]);
            draw_text(ctx, typed_line.data(), text_style(font_id_, text_primary, 12));
        }
    }

    /// Builds settings panel.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_settings_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 15,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-settings-body"},
        });
        panel_heading(ctx, font_id_, "SETTINGS", "Settings", "Graphics, window, scrolling, and widget-composition controls for the workbench.");

        const auto apply_presentation_config = [&](const Engine::EngineConfig &config) {
            Surface *primary_surface = primary_window_ ? find_surface(*primary_window_) : nullptr;
            const Core::RenderSurfaceHandle config_surface =
                primary_surface != nullptr ? primary_surface->handle : surface.handle;
            auto applied = engine.apply_runtime_settings(config_surface, config);
            if (!applied) {
                return applied;
            }
            for (const auto &[window, other_surface] : surfaces_) {
                if (window == config_surface.window_id) {
                    continue;
                }
                if (const Core::RendererResult mirrored = engine.set_presentation_settings(
                        other_surface->handle,
                        config.features.presentation);
                    !mirrored) {
                    Foundation::log_warn(
                        "UiWorkbench: failed to mirror presentation settings to window {}: {}",
                        static_cast<usize>(window),
                        mirrored.error().message);
                }
            }
            return applied;
        };

        const std::span<const RHI::PhysicalGpu> adapters = engine.gpu_inventory().gpus;
        if (!adapters.empty()) {
            selected_graphics_adapter_index_ = std::min(selected_graphics_adapter_index_, adapters.size() - 1);
            UI::DropdownStyle graphics_dropdown_style{};
            graphics_dropdown_style.trigger = action_button_style();
            graphics_dropdown_style.list_background = panel_raised;
            graphics_dropdown_style.option_hovered = UI::Color{accent.r, accent.g, accent.b, 0.22};
            graphics_dropdown_style.arrow_font_id = font_id_;

            section_label(ctx, font_id_, "Graphics adapter");
            std::vector<UI::DropdownOption> adapter_options;
            for (const RHI::PhysicalGpu &gpu : adapters) {
                const std::string label = gpu.name + " — " + gpu.vendor;
                adapter_options.push_back({.build = [label](UI::Context &option_ctx) {
                    draw_text(option_ctx, label, text_style(font_id_, text_primary, 12));
                }});
            }
            const UI::DropdownResult adapter_result = UI::dropdown(
                ctx,
                UString{"workbench-graphics-adapter-dropdown"},
                UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(330.0f), UI::SizingAxis::fixed(38.0f)},
                                .padding = UI::Padding::symmetric(12, 8),
                                .id = UString{"workbench-graphics-adapter-dropdown"}},
                graphics_dropdown_style,
                graphics_adapter_dropdown_state_,
                delta_seconds,
                selected_graphics_adapter_index_,
                adapter_options,
                true,
                UI::DropdownComposition{});
            selected_graphics_adapter_index_ = adapter_result.selected_index;
            if (adapter_result.changed)
                selected_graphics_api_index_ = 0;

            const RHI::PhysicalGpu &adapter = adapters[selected_graphics_adapter_index_];
            selected_graphics_api_index_ = std::min(selected_graphics_api_index_, adapter.api_support.size() - 1);
            section_label(ctx, font_id_, "Graphics API");
            std::vector<UI::DropdownOption> api_options;
            for (const RHI::GpuApiSupport &api : adapter.api_support) {
                const std::string label = RHI::backend_type_name(api.adapter.backend);
                api_options.push_back({.build = [label](UI::Context &option_ctx) {
                    draw_text(option_ctx, label, text_style(font_id_, text_primary, 12));
                }});
            }
            const UI::DropdownResult api_result = UI::dropdown(
                ctx,
                UString{"workbench-graphics-api-dropdown"},
                UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(330.0f), UI::SizingAxis::fixed(38.0f)},
                                .padding = UI::Padding::symmetric(12, 8),
                                .id = UString{"workbench-graphics-api-dropdown"}},
                graphics_dropdown_style,
                graphics_api_dropdown_state_,
                delta_seconds,
                selected_graphics_api_index_,
                api_options,
                true,
                UI::DropdownComposition{});
            selected_graphics_api_index_ = api_result.selected_index;

            const UI::ButtonResult reconstruct = UI::button(ctx,
                                                            UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fixed(36.0f)},
                                                                            .padding = UI::Padding::symmetric(12, 8),
                                                                            .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                                                                            .id = UString{"workbench-reconstruct-graphics"}},
                                                            action_button_style(),
                                                            reconstruct_graphics_button_state_,
                                                            delta_seconds);
            draw_text(ctx, "Reconstruct graphics", text_style(font_id_, text_primary, 12));
            if (reconstruct.clicked) {
                ui_renderer_rebuild_failed_ = false;
                Engine::EngineConfig requested = engine.config();
                requested.graphics_backend = adapter.api_support[selected_graphics_api_index_].adapter.backend;
                requested.graphics_physical_device_id = adapter.physical_device_id;


                engine.wait_idle();
                if (RHI::RhiDevice *old_device = engine.rhi_device()) {
                    destroy_surface_renderers(*old_device);
                }


                Foundation::log_info("UiWorkbench: reconstructing graphics on {} using {}...",
                                     adapter.name, RHI::backend_type_name(requested.graphics_backend));
                if (auto rebuilt = apply_presentation_config(requested); rebuilt) {
                    RHI::RhiDevice *new_device = engine.rhi_device();
                    if (new_device == nullptr) {
                        status_message_ = "Graphics reconstruction failed: replacement RHI device is unavailable.";
                        Foundation::log_error("UiWorkbench: {}", status_message_);
                    } else if (auto ui_rebuilt = recreate_surface_renderers(*new_device); !ui_rebuilt) {
                        ui_renderer_rebuild_failed_ = true;
                        status_message_ = "Graphics reconstruction succeeded, but UI resources could not be rebuilt: " +
                                          ui_rebuilt.error().message;
                        Foundation::log_error("UiWorkbench: {}", status_message_);
                    } else {
                        ui_renderer_rebuild_failed_ = false;
                        frames_to_skip_after_graphics_reconstruction_ = 2;
                        status_message_ = "Reconstructed " + adapter.name + " using " +
                                          RHI::backend_type_name(requested.graphics_backend) + ".";
                        Foundation::log_info("UiWorkbench: {}", status_message_);
                    }
                } else {


                    const std::string reconstruction_error = rebuilt.error().message;
                    if (RHI::RhiDevice *current_device = engine.rhi_device()) {
                        if (auto ui_restored = recreate_surface_renderers(*current_device); !ui_restored) {
                            status_message_ = "Graphics reconstruction failed: " + reconstruction_error +
                                              "; UI resources could not be restored: " + ui_restored.error().message;
                        } else {
                            status_message_ = "Graphics reconstruction failed: " + reconstruction_error;
                        }
                    } else {
                        status_message_ = "Graphics reconstruction failed: " + reconstruction_error;
                    }
                    Foundation::log_error("UiWorkbench: {}", status_message_);
                }
            }
        } else {
            draw_text(ctx, "No graphics devices were discovered.", text_style(font_id_, warning, 12));
        }

        {
            UI::DropdownStyle window_type_dropdown_style{};
            window_type_dropdown_style.trigger = action_button_style();
            window_type_dropdown_style.list_background = panel_raised;
            window_type_dropdown_style.option_hovered = UI::Color{accent.r, accent.g, accent.b, 0.22};
            window_type_dropdown_style.arrow_font_id = font_id_;

            section_label(ctx, font_id_, "Window Type");
#if defined(STURDY_UI_WORKBENCH_HAS_GLFW)
            static constexpr std::array<const char *, 2> window_type_labels = {"SDL3", "GLFW"};
#else
            static constexpr std::array<const char *, 1> window_type_labels = {"SDL3"};
#endif
            std::vector<UI::DropdownOption> window_type_options;
            for (const char *label : window_type_labels) {
                window_type_options.push_back({.build = [label](UI::Context &option_ctx) {
                    draw_text(option_ctx, label, text_style(font_id_, text_primary, 12));
                }});
            }
            const UI::DropdownResult window_type_result = UI::dropdown(
                ctx,
                UString{"workbench-window-type-dropdown"},
                UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(330.0f), UI::SizingAxis::fixed(38.0f)},
                                .padding = UI::Padding::symmetric(12, 8),
                                .id = UString{"workbench-window-type-dropdown"}},
                window_type_dropdown_style,
                window_type_dropdown_state_,
                delta_seconds,
                selected_window_type_index_,
                window_type_options,
                true,
                UI::DropdownComposition{});
            selected_window_type_index_ = window_type_result.selected_index;

            const UI::ButtonResult recreate_window = UI::button(
                ctx,
                UI::ElementDecl{.sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fixed(36.0f)},
                                .padding = UI::Padding::symmetric(12, 8),
                                .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                                .id = UString{"workbench-recreate-window"}},
                action_button_style(),
                recreate_window_button_state_,
                delta_seconds);
            draw_text(ctx, "Recreate window", text_style(font_id_, text_primary, 12));
            if (recreate_window.clicked) {
                const Engine::WindowSnapshot *primary_snapshot = engine.window_state().primary();
                if (primary_snapshot == nullptr) {
                    status_message_ = "Cannot recreate the primary window: no live window snapshot yet.";
                } else {
                    const WindowManager::WindowConfig config{
                        .title = "Sturdy UI Workbench",
                        .extent = primary_snapshot->size,
                        .position = primary_snapshot->position,
                        .use_default_position = false,
                        .visible = true,
                        .resizable = true,
                        .decorated = true,
                        .high_dpi = true,
                        // Matches the workbench's own current toggle rather than being hardcoded true —
                        // a window recreated while "Transparent swapchain" is off should stay opaque,
                        // not silently switch on. Presentation settings (HDR/transparent-composition)
                        // and any active blur effect survive recreation on their own now
                        // (Application::recreate_primary_window() carries a window's current
                        // Core::PresentationSettings and WindowManager::WindowEffects forward
                        // onto its replacement), so nothing else needs reapplying here.
                        .transparent = swapchain_transparent_,
                        .mode = WindowManager::WindowMode::Windowed,
                        .graphics_api = WindowManager::WindowGraphicsApi::Vulkan,
                    };
                    WindowManager::WindowFactory factory = nullptr;
#if defined(STURDY_UI_WORKBENCH_HAS_GLFW)
                    if (selected_window_type_index_ == 1) {
                        factory = &WindowManager::GLFW::create_window;
                    }
#endif
                    (void)engine.window_requests().recreate_primary_window(config, factory);
                    status_message_ = "Requesting primary window recreation...";
                }
            }
        }

        const auto toggle_row = [&](usize index, const char *label, const char *description, bool &value, const std::function<void()> &on_change = [] {}) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 12,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            {
                auto copy = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                    .child_gap = 3,
                    .direction = UI::LayoutDirection::TopToBottom,
                });
                draw_text(ctx, label, text_style(font_id_, text_primary, 13));
                draw_text(ctx, description, text_style(font_id_, text_secondary, 10));
            }
            const UI::ToggleResult result = UI::switch_toggle(
                ctx,
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                    .id = UString{"workbench-toggle-" + std::to_string(index)},
                },
                toggle_style(), toggle_states_[index], delta_seconds, value);
            if (result.clicked) {
                value = !value;
                on_change();
            } };

        toggle_row(0, "Slider enabled", "Whole-control interaction gate", slider_enabled_);
        toggle_row(1, "Marker parts", "Hide ticks and generated labels", show_slider_markers_);
        toggle_row(2, "Custom thumb", "Replace geometry while retaining drag behavior", use_custom_thumb_);
        toggle_row(3, "Color picker enabled", "Disabled visuals keep layout stable", picker_enabled_);
        toggle_row(4, "Alpha part", "Remove the alpha strip independently", show_alpha_);
        toggle_row(5, "Preview part", "Remove preview without changing color state", show_preview_);

        section_label(ctx, font_id_, "PRESENTATION");
        toggle_row(10, "HDR", "Use compositor-managed linear scRGB presentation", hdr_enabled_, [&] {
            Engine::EngineConfig config = engine.config();
            config.features.presentation.hdr_enabled = hdr_enabled_;
            if (const auto applied = apply_presentation_config(config)) {
                status_message_ = hdr_enabled_
                                      ? "HDR enabled with scRGB presentation; the swapchain will be recreated."
                                      : "HDR disabled; SDR presentation will resume.";
            } else {
                hdr_enabled_ = !hdr_enabled_;
                status_message_ = "HDR change rejected: " + applied.error().message;
            }
        });
        if (hdr_enabled_) {
            section_label(ctx, font_id_, "HDR METADATA");
            draw_text(
                ctx,
                "Reference white and peak affect scRGB/PQ rendering. MaxCLL and MaxFALL are staged "
                "for HDR10 because linear scRGB carries no static HDR10 metadata.",
                text_style(font_id_, text_secondary, 10));

            const UI::SliderStyle metadata_style{
                .track = UI::Color{0.115, 0.130, 0.180, 1.0},
                .fill = accent,
                .thumb = text_primary,
                .thumb_hovered = UI::Color{1.0, 1.0, 1.0, 1.0},
                .thumb_dragging = accent_hot,
                .track_thickness = 6.0f,
                .thumb_size = 16.0f,
                .focused_border = UI::BorderStyle{},
            };
            const auto metadata_slider = [&](const char *id, const char *label, const char *description, f64 &value, const UI::SliderConfig &config, UI::SliderState &state, f64 display_scale = 1.0, const char *suffix = " nits") {
                auto row = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                    .padding = UI::Padding::all(12),
                    .child_gap = 7,
                    .direction = UI::LayoutDirection::TopToBottom,
                    .background_color = background_with_opacity(panel, effective_background_opacity()),
                    .corner_radius = UI::CornerRadius::all(11.0f),
                    .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
                });
                draw_text(ctx, number_text((std::string{label} + "  ").c_str(), value * display_scale, suffix), text_style(font_id_, text_primary, 13));
                draw_text(ctx, description, text_style(font_id_, text_secondary, 10));
                const UI::SliderResult result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(28.0f)},
                        .id = UString{id},
                    },
                    config,
                    metadata_style,
                    state,
                    value,
                    UI::SliderInput{},
                    true);
                value = result.value;
                return result;
            };

            const UI::SliderResult reference_result = metadata_slider(
                "workbench-hdr-reference-white",
                "Reference white scale",
                "Scales the compositor-provided SDR white while preserving monitor-specific calibration.",
                hdr_reference_white_scale_,
                UI::SliderConfig{.min = 0.25, .max = 2.0, .step = 0.01},
                hdr_reference_white_scale_state_,
                100.0,
                "%");
            const f64 minimum_peak_nits =
                std::max(80.0, static_cast<f64>(render_graph_.tone_mapping().hdr_paper_white_nits));
            hdr_peak_luminance_nits_ = std::max(hdr_peak_luminance_nits_, minimum_peak_nits);
            const UI::SliderResult peak_result = metadata_slider(
                "workbench-hdr-peak",
                "Peak luminance",
                "Highlight ceiling used by HDR-capable tone-mapping operators.",
                hdr_peak_luminance_nits_,
                UI::SliderConfig{
                    .min = minimum_peak_nits,
                    .max = std::max(4000.0, minimum_peak_nits),
                    .step = 10.0,
                },
                hdr_peak_luminance_state_);
            render_graph_.tone_mapping().hdr_peak_nits =
                static_cast<f32>(hdr_peak_luminance_nits_);

            const UI::SliderResult max_cll_result = metadata_slider(
                "workbench-hdr-max-cll",
                "MaxCLL",
                "Maximum content light level advertised to an HDR10 display.",
                hdr_max_content_light_level_nits_,
                UI::SliderConfig{.min = 80.0, .max = 10000.0, .step = 10.0},
                hdr_max_content_light_level_state_);
            hdr_max_frame_average_light_level_nits_ = std::min(
                hdr_max_frame_average_light_level_nits_,
                hdr_max_content_light_level_nits_);
            const UI::SliderResult max_fall_result = metadata_slider(
                "workbench-hdr-max-fall",
                "MaxFALL",
                "Maximum frame-average light level advertised to an HDR10 display.",
                hdr_max_frame_average_light_level_nits_,
                UI::SliderConfig{
                    .min = 0.0,
                    .max = std::max(0.0, hdr_max_content_light_level_nits_),
                    .step = 10.0,
                },
                hdr_max_frame_average_light_level_state_);

            if (reference_result.committed || peak_result.committed) {
                status_message_ = "HDR rendering metadata updated.";
            }
            if (max_cll_result.committed || max_fall_result.committed) {
                if (engine.config().features.presentation.hdr_color_space !=
                    Core::HdrColorSpaceMode::Hdr10St2084) {
                    status_message_ = "MaxCLL/MaxFALL staged; they apply when HDR10/PQ presentation is active.";
                } else {
                    const RHI::HdrContentLightLevelUpdate update{
                        .max_content_light_level_nits =
                            static_cast<f32>(hdr_max_content_light_level_nits_),
                        .max_frame_average_light_level_nits =
                            static_cast<f32>(hdr_max_frame_average_light_level_nits_),
                    };
                    bool all_updated = true;
                    for (const auto &[window, other_surface] : surfaces_) {
                        (void)window;
                        if (const RHI::RhiResult updated =
                                engine.update_hdr_content_light_level(other_surface->handle, update);
                            !updated) {
                            all_updated = false;
                            Foundation::log_warn(
                                "UiWorkbench: failed to update HDR10 metadata for window {}: {}",
                                static_cast<usize>(other_surface->handle.window_id),
                                updated.error().message);
                        }
                    }
                    status_message_ = all_updated
                                          ? "HDR10 MaxCLL/MaxFALL metadata updated on every surface."
                                          : "Some surfaces rejected the HDR10 metadata update; see the log.";
                }
            }
        }
        toggle_row(8, "Transparent swapchain", "Use premultiplied compositor alpha for this surface", swapchain_transparent_, [&] {
            Engine::EngineConfig config = engine.config();
            config.features.presentation.transparent_composition = swapchain_transparent_;
            if (const auto applied = apply_presentation_config(config)) {


                status_message_ = swapchain_transparent_
                                      ? "Transparent swapchain enabled; adjust background opacity below."
                                      : "Opaque swapchain composition restored.";
            } else {
                swapchain_transparent_ = !swapchain_transparent_;
                status_message_ = "Transparency change rejected: " + applied.error().message;
            }
        });


        if (WindowManager::operating_system_may_support_window_effect(
                WindowManager::WindowEffectKind::Transparent)) {
            for (const auto &[window, other_surface] : surfaces_) {
                const bool wants_legacy_effect =
                    swapchain_transparent_ &&
                    !engine.presentation_resolution(other_surface->handle).via_composition_present;
                const bool currently_applied = legacy_window_transparency_applied_.contains(window);
                if (wants_legacy_effect == currently_applied) {
                    continue;
                }
                engine.window_requests().set_transparent(window, wants_legacy_effect);
                if (wants_legacy_effect) {
                    legacy_window_transparency_applied_.insert(window);
                } else {
                    legacy_window_transparency_applied_.erase(window);
                }
            }
        }
        if (swapchain_transparent_) {
            auto opacity_row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 10,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, number_text("UI background opacity  ", ui_background_opacity_ * 100.0, "%"), text_style(font_id_, text_primary, 13));
            const UI::SliderConfig opacity_config{.min = 0.0, .max = 1.0, .step = 0.01};
            const UI::SliderStyle opacity_style{
                .track = UI::Color{0.115, 0.130, 0.180, 1.0},
                .fill = accent,
                .thumb = text_primary,
                .thumb_hovered = UI::Color{1.0, 1.0, 1.0, 1.0},
                .thumb_dragging = accent_hot,
                .track_thickness = 6.0f,
                .thumb_size = 16.0f,
                .focused_border = UI::BorderStyle{},
            };
            const UI::SliderResult opacity_result = UI::slider(
                ctx,
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(28.0f)},
                    .id = UString{"workbench-ui-background-opacity"},
                },
                opacity_config,
                opacity_style,
                ui_background_opacity_slider_state_,
                ui_background_opacity_,
                UI::SliderInput{},
                true);
            ui_background_opacity_ = opacity_result.value;
        }

        section_label(ctx, font_id_, "OS WINDOW COMPOSITION");
        {


            const std::array fullscreen_options{
                dropdown_option(font_id_, "Off", text_secondary),
                dropdown_option(font_id_, "Borderless", accent),
                dropdown_option(font_id_, "Exclusive", accent_hot),
            };
            UI::DropdownStyle fullscreen_style{};
            fullscreen_style.trigger = action_button_style();
            fullscreen_style.list_background = fullscreen_style.trigger.idle;
            fullscreen_style.option_hovered = UI::Color{0.17, 0.20, 0.29, 1.0};
            fullscreen_style.corner_radius = UI::CornerRadius::all(11.0f);
            fullscreen_style.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
            fullscreen_style.option_padding = 10;
            fullscreen_style.arrow_color = accent;
            fullscreen_style.arrow_font_id = font_id_;

            draw_text(ctx, "Fullscreen", text_style(font_id_, text_primary, 13));
            const UI::DropdownResult fullscreen_result = UI::dropdown(
                ctx,
                UString{"workbench-fullscreen-dropdown"},
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fixed(38.0f)},
                    .padding = UI::Padding::symmetric(12, 8),
                    .child_gap = 8,
                    .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                    .id = UString{"workbench-fullscreen-dropdown"},
                },
                fullscreen_style,
                fullscreen_mode_dropdown_state_,
                delta_seconds,
                selected_fullscreen_mode_index_,
                fullscreen_options,
                true);
            if (fullscreen_result.changed) {
                selected_fullscreen_mode_index_ = fullscreen_result.selected_index;
                engine.window_requests().set_fullscreen(
                    surface.handle.window_id,
                    static_cast<WindowManager::WindowMode>(selected_fullscreen_mode_index_));
            }
        }
        toggle_row(7, "Window decorated", "Disable to remove the OS title bar/border", window_decorated_, [&] {
            engine.window_requests().set_decorated(surface.handle.window_id, window_decorated_);
        });

        section_label(ctx, font_id_, "WINDOW BLUR");
        if (supported_blur_kinds_.empty()) {
            draw_text(ctx, "No blur-capable window effect is supported on this OS build.", text_style(font_id_, text_secondary, 11));
        } else {
            std::vector<UI::DropdownOption> blur_options;
            blur_options.reserve(supported_blur_kinds_.size());
            for (WindowManager::WindowEffectKind kind : supported_blur_kinds_) {
                blur_options.push_back(dropdown_option(
                    font_id_,
                    WindowManager::window_effect_kind_name(kind).data(),
                    accent));
            }

            UI::DropdownStyle blur_style{};
            blur_style.trigger = action_button_style();


            blur_style.list_background = blur_style.trigger.idle;
            blur_style.option_hovered = UI::Color{0.17, 0.20, 0.29, 1.0};
            blur_style.corner_radius = UI::CornerRadius::all(11.0f);
            blur_style.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
            blur_style.option_padding = 10;
            blur_style.arrow_color = accent;
            blur_style.arrow_font_id = font_id_;


            const usize previous_blur_index = selected_blur_kind_index_;
            const UI::DropdownResult blur_dropdown_result = UI::dropdown(
                ctx,
                UString{"workbench-blur-dropdown"},
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fixed(38.0f)},
                    .padding = UI::Padding::symmetric(12, 8),
                    .child_gap = 8,
                    .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                    .id = UString{"workbench-blur-dropdown"},
                },
                blur_style,
                blur_dropdown_state_,
                delta_seconds,
                selected_blur_kind_index_,
                blur_options,
                true);
            selected_blur_kind_index_ = blur_dropdown_result.selected_index;
            if (window_blur_enabled_ && selected_blur_kind_index_ != previous_blur_index) {
                engine.window_requests().set_blur(surface.handle.window_id,
                                                  supported_blur_kinds_[selected_blur_kind_index_],
                                                  true);
            }

            toggle_row(9, "Blur enabled", "Applies the selected blur type above", window_blur_enabled_, [&] {
                engine.window_requests().set_blur(surface.handle.window_id,
                                                  supported_blur_kinds_[selected_blur_kind_index_],
                                                  window_blur_enabled_);
            });
        }

        section_label(ctx, font_id_, "COMPOSED DROPDOWN");
        const std::array options{
            dropdown_option(font_id_, "Aurora Glass", accent),
            dropdown_option(font_id_, "Warm Graphite", warning),
            dropdown_option(font_id_, "Neon Void (locked)", accent_hot),
            dropdown_option(font_id_, "Mint Terminal", success),
        };
        UI::DropdownStyle style{};
        style.trigger = action_button_style();

        style.list_background = style.trigger.idle;
        style.option_hovered = UI::Color{0.17, 0.20, 0.29, 1.0};
        style.corner_radius = UI::CornerRadius::all(11.0f);
        style.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
        style.option_padding = 10;
        style.arrow_color = accent;
        style.arrow_font_id = font_id_;

        UI::DropdownComposition composition{};
        composition.option_enabled = [](usize index) { return index != 2; };
        composition.header.visible = true;
        composition.header.alter_decl = [](UI::ElementDecl &decl, const UI::DropdownPartContext &) {
            decl.padding = UI::Padding::symmetric(10, 8);
            decl.background_color = UI::Color{0.075, 0.088, 0.130, 1.0};
        };
        composition.header.build = [](UI::Context &part_ctx,
                                      const UI::DropdownPartContext &) {
            draw_text(part_ctx, "WORKBENCH PRESETS", text_style(font_id_, accent, 10));
        };
        composition.footer.visible = true;
        composition.footer.alter_decl = [](UI::ElementDecl &decl, const UI::DropdownPartContext &) {
            decl.padding = UI::Padding::symmetric(10, 7);
        };
        composition.footer.build = [](UI::Context &part_ctx,
                                      const UI::DropdownPartContext &) {
            draw_text(part_ctx, "Disabled rows remain visible and stylable.", text_style(font_id_, text_secondary, 9));
        };
        composition.option.visual.selected.background_color =
            UI::Color{accent.r, accent.g, accent.b, 0.18};
        composition.option.visual.disabled.background_color =
            UI::Color{0.060, 0.065, 0.080, 0.65};
        composition.tooltip.visible = true;
        composition.tooltip.build = [this](UI::Context &part_ctx,
                                           const UI::DropdownPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(8, 5),
                .background_color = background_with_opacity(panel_raised, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(7.0f),
            });
            draw_text(part_ctx,
                      part.option_index == 2 ? "This preset is intentionally disabled."
                                             : "Select this visual preset.",
                      text_style(font_id_, text_primary, 10));
        };

        const UI::DropdownResult dropdown_result = UI::dropdown(
            ctx,
            UString{"workbench-preset-dropdown"},
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(260.0f), UI::SizingAxis::fixed(38.0f)},
                .padding = UI::Padding::symmetric(12, 8),
                .child_gap = 8,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .id = UString{"workbench-preset-dropdown"},
            },
            style,
            preset_dropdown_state_,
            delta_seconds,
            selected_preset_,
            options,
            true,
            composition);
        selected_preset_ = dropdown_result.selected_index;

        section_label(ctx, font_id_, "SCROLLING");

        const auto scroll_toggle_row = [&](usize index, const char *label, const char *description, bool &value) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 12,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            {
                auto copy = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                    .child_gap = 3,
                    .direction = UI::LayoutDirection::TopToBottom,
                });
                draw_text(ctx, label, text_style(font_id_, text_primary, 13));
                draw_text(ctx, description, text_style(font_id_, text_secondary, 10));
            }
            const UI::ToggleResult result = UI::switch_toggle(
                ctx,
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                    .id = UString{"workbench-scroll-toggle-" + std::to_string(index)},
                },
                toggle_style(),
                toggle_states_[index],
                delta_seconds,
                value);
            if (result.clicked) {
                value = !value;
            }
        };

        scroll_toggle_row(6, "Click-and-drag scroll", "Off by default — dragging this body's content no longer competes with widget drags.", scroll_click_drag_);
        scroll_toggle_row(7, "Smooth wheel scrolling", "Eases wheel deltas across frames instead of snapping the offset instantly.", scroll_smooth_);

        {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 10,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, "Smoothing rate", text_style(font_id_, text_primary, 13));
            const UI::SliderConfig rate_config{.min = 2.0, .max = 60.0, .step = 1.0};
            UI::SliderStyle rate_style{
                .track = UI::Color{0.115, 0.130, 0.180, 1.0},
                .fill = accent,
                .thumb = text_primary,
                .thumb_hovered = UI::Color{1.0, 1.0, 1.0, 1.0},
                .thumb_dragging = accent_hot,
                .track_thickness = 6.0f,
                .thumb_size = 16.0f,
                .focused_border = UI::BorderStyle{},
            };
            const UI::SliderResult rate_result = UI::slider(
                ctx,
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(28.0f)},
                    .id = UString{"workbench-scroll-smoothing-rate"},
                },
                rate_config,
                rate_style,
                scroll_smoothing_rate_state_,
                scroll_smoothing_rate_,
                UI::SliderInput{},
                scroll_smooth_);
            scroll_smoothing_rate_ = rate_result.value;
        }

        const UI::ButtonResult reset = UI::button(
            ctx,
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(180.0f), UI::SizingAxis::fixed(36.0f)},
                .padding = UI::Padding::symmetric(12, 8),
                .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                .id = UString{"workbench-reset"},
            },
            action_button_style(),
            reset_button_state_,
            delta_seconds);
        draw_text(ctx, "Reset widget composition", text_style(font_id_, text_primary, 12));
        if (reset.clicked) {
            slider_enabled_ = true;
            show_slider_markers_ = true;
            use_custom_thumb_ = true;
            picker_enabled_ = true;
            show_alpha_ = true;
            show_preview_ = true;
        }
    }

    /// Builds text panel.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_text_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-text-body"},
        });
        panel_heading(ctx, font_id_, "TEXT INPUT", "Text Lab", "One-line, masked, and multiline markdown editing on the shared TextEdit engine.");

        WindowManager::Window *clipboard_window = engine.primary_window();
        const UI::TextEditInput edit_input = surface.text_input.frame_input(
            [clipboard_window]() { return clipboard_window != nullptr ? UString{clipboard_window->clipboard_text()} : UString{}; },
            [clipboard_window](const UString &text) {
                if (clipboard_window != nullptr) {
                    [[maybe_unused]] const auto result =
                        clipboard_window->set_clipboard_text(text.cpp_string_view());
                } });

        UI::TextEditStyle edit_style{};
        edit_style.idle = background_with_opacity(panel, effective_background_opacity());
        edit_style.hovered = background_with_opacity(panel_raised, effective_background_opacity());
        edit_style.focused = UI::Color{0.050, 0.058, 0.088, 1.0};
        edit_style.border_idle = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
        edit_style.border_focused = UI::BorderStyle{.color = accent, .width = UI::BorderWidth::all(1)};
        edit_style.text_color = text_primary;
        edit_style.placeholder_color = text_secondary;
        edit_style.corner_radius = UI::CornerRadius::all(9.0f);
        edit_style.font_id = font_id_;
        edit_style.font_size = 13;

        section_label(ctx, font_id_, "SINGLE LINE");
        const UI::TextInputResult single_line_result =
            UI::text_input(ctx,
                           UI::ElementDecl{
                               .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(36.0f)},
                               .padding = UI::Padding::symmetric(11, 8),
                               .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                               .id = UString{"workbench-text-single"},
                           },
                           edit_style,
                           line_input_state_,
                           edit_input,
                           delta_seconds,
                           UString{"Type something..."});

        section_label(ctx, font_id_, "PASSWORD");
        UI::TextEditStyle password_style = edit_style;
        password_style.mask_characters = true;
        const UI::TextInputResult password_result =
            UI::text_input(ctx,
                           UI::ElementDecl{
                               .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(36.0f)},
                               .padding = UI::Padding::symmetric(11, 8),
                               .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                               .id = UString{"workbench-text-password"},
                           },
                           password_style,
                           password_input_state_,
                           edit_input,
                           delta_seconds,
                           UString{"Password"});

        section_label(ctx, font_id_, "MARKDOWN + LIVE PREVIEW");
        UI::TextEditStyle markdown_style = edit_style;


        markdown_style.highlighter = [](const UString &text) {
            std::vector<UI::RichTextSpan> spans;
            const usize n = text.size();
            const auto scalar_at = [&](usize i) { return text.substr(i, 1).cpp_string(); };
            usize line_start = 0;
            usize code_start = 0;
            usize bold_start = 0;
            bool in_code = false;
            bool in_bold = false;
            for (usize i = 0; i <= n; ++i) {
                const std::string s = i < n ? scalar_at(i) : std::string{"\n"};
                if (s == "\n") {
                    if (line_start < i && scalar_at(line_start) == "#") {
                        spans.push_back({.scalar_start = line_start, .scalar_length = i - line_start, .color = accent});
                    } else if (i >= line_start + 2 && scalar_at(line_start) == "-" && scalar_at(line_start + 1) == " ") {
                        spans.push_back({.scalar_start = line_start, .scalar_length = 1, .color = warning});
                    }
                    line_start = i + 1;
                    in_code = false;
                    in_bold = false;
                    continue;
                }
                if (s == "`") {
                    if (!in_code) {
                        in_code = true;
                        code_start = i;
                    } else {
                        spans.push_back({.scalar_start = code_start, .scalar_length = i - code_start + 1, .color = success});
                        in_code = false;
                    }
                    continue;
                }
                if (s == "*" && i + 1 < n && scalar_at(i + 1) == "*") {
                    if (!in_bold) {
                        in_bold = true;
                        bold_start = i;
                    } else {
                        spans.push_back({.scalar_start = bold_start, .scalar_length = i + 2 - bold_start, .color = accent_hot});
                        in_bold = false;
                    }
                    ++i;
                }
            }
            return spans;
        };
        const UI::TextAreaResult markdown_result =
            UI::text_area(ctx,
                          UI::ElementDecl{
                              .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(150.0f)},
                              .padding = UI::Padding::all(10),
                              .id = UString{"workbench-text-markdown"},
                          },
                          markdown_style,
                          markdown_input_state_,
                          edit_input,
                          delta_seconds,
                          scrollbar_style_,
                          markdown_input_scroll_state_,
                          UString{"# Write some markdown..."});


        const auto resolve_focus = [&](const std::optional<UI::ElementBounds> &caret_bounds, const UString &widget_id,
                                       bool ime_enabled) -> std::optional<Engine::TextInputFocusInfo> {
            if (!caret_bounds) {
                return std::nullopt;
            }
            const std::optional<UI::ElementBounds> field_bounds = ctx.element_bounds(widget_id);
            return Engine::TextInputFocusInfo{
                .field_bounds = field_bounds ? *field_bounds : *caret_bounds,
                .caret_bounds = *caret_bounds,
                .ime_enabled = ime_enabled,
            };
        };
        std::optional<Engine::TextInputFocusInfo> focus =
            resolve_focus(single_line_result.caret_bounds, UString{"workbench-text-single"}, edit_style.features.ime_enabled);
        if (!focus) {
            focus = resolve_focus(password_result.caret_bounds, UString{"workbench-text-password"}, password_style.features.ime_enabled);
        }
        if (!focus) {
            focus = resolve_focus(markdown_result.caret_bounds, UString{"workbench-text-markdown"}, markdown_style.features.ime_enabled);
        }
        Engine::forward_text_input_state(engine.window_requests(), surface.handle.window_id, focus);

        {
            auto preview_card = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(13),
                .child_gap = 5,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = background_with_opacity(panel_raised, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });


            const auto render_inline = [&](const std::string &line, u16 font_size) {
                auto row = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                });
                (void)row;
                std::string segment;
                UI::Color segment_color = text_primary;
                bool bold = false;
                bool code = false;
                const auto flush = [&]() {
                    if (!segment.empty()) {
                        draw_text(ctx, segment, text_style(font_id_, segment_color, font_size));
                        segment.clear();
                    }
                };
                for (usize i = 0; i < line.size(); ++i) {
                    if (!code && i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
                        flush();
                        bold = !bold;
                        segment_color = bold ? accent_hot : text_primary;
                        ++i;
                        continue;
                    }
                    if (line[i] == '`') {
                        flush();
                        code = !code;
                        segment_color = code ? success : (bold ? accent_hot : text_primary);
                        continue;
                    }
                    segment += line[i];
                }
                flush();
            };

            const std::string markdown = markdown_input_state_.text().cpp_string();
            usize start = 0;
            while (start <= markdown.size()) {
                const usize end = std::min(markdown.find('\n', start), markdown.size());
                const std::string line = markdown.substr(start, end - start);
                if (line.rfind("## ", 0) == 0) {
                    draw_text(ctx, line.substr(3), text_style(font_id_, text_primary, 16));
                } else if (line.rfind("# ", 0) == 0) {
                    draw_text(ctx, line.substr(2), text_style(font_id_, text_primary, 20));
                } else if (line.rfind("- ", 0) == 0) {
                    auto bullet_row = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                        .child_gap = 6,
                    });
                    draw_text(ctx, "•", text_style(font_id_, warning, 12));
                    render_inline(line.substr(2), 12);
                } else if (line.empty()) {
                    auto spacer = ctx.element(UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(1.0f), UI::SizingAxis::fixed(4.0f)}});
                    (void)spacer;
                } else {
                    render_inline(line, 12);
                }
                if (end == markdown.size()) {
                    break;
                }
                start = end + 1;
            }
        }
    }

    /// Builds docking panel.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-docking-body"},
        });
        panel_heading(ctx, font_id_, "WORKSPACE", "Docking Guide", "The layout tree is live: tabs reorder, split targets preview, and dividers resize.");

        const auto instruction = [&](const char *number, const char *title, const char *copy, UI::Color color) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(11),
                .child_gap = 11,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = background_with_opacity(panel, effective_background_opacity()),
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            {
                auto badge = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(29.0f), UI::SizingAxis::fixed(29.0f)},
                    .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                    .background_color = UI::Color{color.r, color.g, color.b, 0.18},
                    .corner_radius = UI::CornerRadius::all(9.0f),
                    .border = UI::BorderStyle{.color = color, .width = UI::BorderWidth::all(1)},
                });
                draw_text(ctx, number, text_style(font_id_, color, 12));
            }
            {
                auto copy_block = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                    .child_gap = 3,
                    .direction = UI::LayoutDirection::TopToBottom,
                });
                draw_text(ctx, title, text_style(font_id_, text_primary, 12));
                draw_text(ctx, copy, text_style(font_id_, text_secondary, 10));
            }
        };

        instruction("01", "Reorder or merge tabs", "Drag a tab across its strip or release it over another panel's center guide.", accent);
        instruction("02", "Create a split", "Release over a left, right, top or bottom guide; drag the divider afterward.", accent_hot);
        instruction("03", "Tear off to an OS window", "Drag beyond the workspace edge. The deferred coordinator transfers only after spawn succeeds.", success);

        {
            auto status = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 5,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = UI::Color{accent.r, accent.g, accent.b, 0.10},
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = UI::Color{accent.r, accent.g, accent.b, 0.38},
                                          .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, surface.primary ? "PRIMARY COORDINATOR" : "SECONDARY COORDINATOR", text_style(font_id_, accent, 10));
            draw_text(ctx, status_message_, text_style(font_id_, text_primary, 11));
        }


    }

    /// Builds metrics panel.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_metrics_panel(Surface &surface, UI::Context &ctx, f32                  ) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-metrics-body"},
        });
        panel_heading(ctx, font_id_, "DIAGNOSTICS", "Performance", "CPU and GPU render-graph pass timing, read back from Renderer::last_frame_timings() "
                                                                   "— one frame stale, the same contract the engine's own debug overlay uses.");

        const Renderer::FrameTimingSnapshot &timings = surface.last_timing_snapshot;
        if (!timings.has_data) {
            draw_text(ctx, "Waiting for the first timing readback…", text_style(font_id_, text_secondary, 13));
            return;
        }

        const auto timing_section = [&](const char *label,
                                        const std::vector<std::pair<std::string, f64>> &entries) {
            section_label(ctx, font_id_, label);
            if (entries.empty()) {
                draw_text(ctx, "No passes recorded this frame.", text_style(font_id_, text_secondary, 12));
                return;
            }
            f64 total_ms = 0.0;
            for (const auto &[category, ms] : entries) {
                total_ms += ms;
            }
            draw_text(ctx, number_text("Total  ", total_ms, " ms"), text_style(font_id_, text_primary, 14));
            for (const auto &[category, ms] : entries) {
                draw_text(ctx, number_text((category + "  ").c_str(), ms, " ms"), text_style(font_id_, text_secondary, 12));
            }
        };

        timing_section("GPU PASS TIMING", timings.gpu_pass_timings_ms);
        timing_section("CPU FRAME STAGES", timings.cpu_stage_timings_ms);
        timing_section("CPU PASS RECORDING", timings.cpu_pass_timings_ms);
    }

    /// Builds strokes panel — a smoke test for UI::Context::stroke_polyline() (spiral, sharp zigzag,
    /// and a pixel-snapped dashed hairline).
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_strokes_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 16,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-strokes-body"},
        });
        panel_heading(ctx, font_id_, "PRIMITIVES", "Strokes",
                      "Anti-aliased polyline smoke test for UI::Context::stroke_polyline() -- a spiral, a sharp "
                      "zigzag, and a pixel-snapped dashed hairline -- plus UI::Context::stroke_custom() for a "
                      "caller-supplied fragment shader.");

        constexpr f32 row_width = 640.0f;
        constexpr f32 row_height = 160.0f;

        const auto stroke_row = [&](const char *id_suffix, const UI::StrokeStyle &style,
                                    const std::vector<glm::vec2> &points) {
            const std::string base = std::string("workbench-strokes-") + id_suffix;
            auto box = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(row_width), UI::SizingAxis::fixed(row_height)},
                .background_color = UI::Color{0.08, 0.09, 0.11, 1.0},
                .corner_radius = UI::CornerRadius::all(8.0f),
                .id = UString{base},
            });
            ctx.stroke_polyline(UI::ElementDecl{
                                    .sizing = {UI::SizingAxis::fixed(row_width), UI::SizingAxis::fixed(row_height)},
                                    .id = UString{base + "-line"},
                                },
                                points, style);
        };

        {
            std::vector<glm::vec2> spiral_points;
            spiral_points.reserve(121);
            const glm::vec2 center{140.0f, 80.0f};
            for (int i = 0; i <= 120; ++i) {
                const f32 t = static_cast<f32>(i) / 120.0f;
                const f32 angle = t * 6.0f * 3.14159265f;
                const f32 radius = 6.0f + t * 68.0f;
                spiral_points.push_back(center + glm::vec2{std::cos(angle), std::sin(angle)} * radius);
            }
            stroke_row("spiral", UI::StrokeStyle{.color = UI::Color{0.35, 0.75, 1.0, 1.0}, .width = 3.0f, .feather_px = 1.5f},
                      spiral_points);
        }
        {
            const std::vector<glm::vec2> zigzag_points{
                {320.0f, 20.0f}, {400.0f, 80.0f}, {320.0f, 140.0f},
            };
            stroke_row("zigzag", UI::StrokeStyle{.color = UI::Color{1.0, 0.55, 0.25, 1.0}, .width = 4.0f}, zigzag_points);
        }
        {
            const std::vector<glm::vec2> dashed_points{{460.0f, 80.0f}, {620.0f, 80.0f}};
            stroke_row("dashed",
                      UI::StrokeStyle{.color = UI::Color{0.6, 1.0, 0.55, 1.0}, .width = 1.0f, .dash_length = 8.0f,
                                     .dash_gap = 6.0f, .snap_to_pixel_grid = true},
                      dashed_points);
        }
        {
            // Custom-shader stroke: an animated rainbow gradient, driven entirely by a caller-supplied
            // fragment shader (Shaders/ui_stroke_custom_demo.slang) instead of ui_stroke.slang.
            surface.custom_stroke_time += delta_seconds;
            struct DemoTrailingParams {
                f32 time;
                f32 pad0;
                f32 pad1;
                f32 pad2;
            };
            const DemoTrailingParams trailing{.time = surface.custom_stroke_time};
            std::vector<std::byte> push_constants(sizeof(trailing));
            std::memcpy(push_constants.data(), &trailing, sizeof(trailing));

            const UI::CustomShaderRef shader{
                .shader_path = "Shaders/ui_stroke_custom_demo.slang",
                .module_name = "ui_stroke_custom_demo",
                .fragment_entry_point = "fragmentMain",
                .push_constants = std::move(push_constants),
            };

            const std::string base = "workbench-strokes-custom";
            auto box = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(row_width), UI::SizingAxis::fixed(row_height)},
                .background_color = UI::Color{0.08, 0.09, 0.11, 1.0},
                .corner_radius = UI::CornerRadius::all(8.0f),
                .id = UString{base},
            });
            std::vector<glm::vec2> wave_points;
            wave_points.reserve(97);
            for (int i = 0; i <= 96; ++i) {
                const f32 t = static_cast<f32>(i) / 96.0f;
                const f32 x = 20.0f + t * 600.0f;
                const f32 y = 80.0f + 45.0f * std::sin(t * 4.0f * 3.14159265f + surface.custom_stroke_time);
                wave_points.push_back({x, y});
            }
            ctx.stroke_custom(UI::ElementDecl{
                                  .sizing = {UI::SizingAxis::fixed(row_width), UI::SizingAxis::fixed(row_height)},
                                  .id = UString{base + "-line"},
                              },
                              wave_points, 4.0f, 1.5f, shader);
        }
    }

    /// Builds graphs panel — a smoke test for UI::graph()/UI::GraphType::Line, including a log-Y-axis
    /// chart and a chart with both axes non-linear (symlog X, log Y) to demonstrate AxisConfig::scale
    /// isn't limited to linear.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_graphs_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        (void)surface;
        (void)delta_seconds;
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 16,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-graphs-body"},
        });
        panel_heading(ctx, font_id_, "PRIMITIVES", "Graphs",
                      "UI::graph() smoke test -- linear axes, a log Y axis, and a chart with both axes "
                      "non-linear (symlog X, log Y).");

        constexpr f32 chart_width = 640.0f;
        constexpr f32 chart_height = 220.0f;

        // Linear: sine wave, both axes linear/autoscaled.
        {
            std::vector<f64> xs;
            std::vector<f64> ys;
            xs.reserve(120);
            ys.reserve(120);
            for (int i = 0; i < 120; ++i) {
                const f64 x = static_cast<f64>(i) / 119.0 * 4.0 * 3.14159265358979;
                xs.push_back(x);
                ys.push_back(std::sin(x));
            }
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.x_axis.title = "x";
            desc.y_axis.title = "sin(x)";
            desc.series.push_back(UI::SeriesRef{
                .name = "sin(x)", .x = xs, .y = ys, .color = UI::Color{0.4, 0.75, 1.0, 1.0}, .line_width = 2.0f,
                .feather_px = 1.0f,
            });
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-linear"},
                            },
                            desc, state);
        }

        // Log Y: exponential growth -- a straight line on a log axis is the classic visual proof that
        // the scale is actually logarithmic, not just relabeled.
        {
            std::vector<f64> xs;
            std::vector<f64> ys;
            xs.reserve(60);
            ys.reserve(60);
            for (int i = 0; i < 60; ++i) {
                const f64 x = static_cast<f64>(i) / 59.0 * 10.0;
                xs.push_back(x);
                ys.push_back(std::exp(x * 0.5));
            }
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.x_axis.title = "x";
            desc.y_axis.title = "exp(x/2)";
            desc.y_axis.scale.kind = UI::ScaleKind::Log;
            desc.series.push_back(UI::SeriesRef{
                .name = "exp(x/2)", .x = xs, .y = ys, .color = UI::Color{1.0, 0.6, 0.3, 1.0}, .line_width = 2.0f,
                .feather_px = 1.0f,
            });
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-logy"},
                            },
                            desc, state);
        }

        // Both axes non-linear: x is symlog (the data crosses zero, which a plain log axis can't
        // represent), y is log (x^2 + 1 is always positive).
        {
            std::vector<f64> xs;
            std::vector<f64> ys;
            xs.reserve(101);
            ys.reserve(101);
            for (int i = 0; i <= 100; ++i) {
                const f64 x = -50.0 + static_cast<f64>(i);
                xs.push_back(x);
                ys.push_back(x * x + 1.0);
            }
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.x_axis.title = "x";
            desc.y_axis.title = "x^2 + 1";
            desc.x_axis.scale.kind = UI::ScaleKind::Symlog;
            desc.x_axis.scale.symlog_linear_threshold = 5.0;
            desc.y_axis.scale.kind = UI::ScaleKind::Log;
            desc.series.push_back(UI::SeriesRef{
                .name = "x^2 + 1", .x = xs, .y = ys, .color = UI::Color{0.65, 1.0, 0.55, 1.0}, .line_width = 2.0f,
                .feather_px = 1.0f,
            });
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-both"},
                            },
                            desc, state);
        }

        // Area: damped oscillation, filled to the zero baseline.
        {
            std::vector<f64> xs;
            std::vector<f64> ys;
            xs.reserve(150);
            ys.reserve(150);
            for (int i = 0; i < 150; ++i) {
                const f64 x = static_cast<f64>(i) / 149.0 * 6.0 * 3.14159265358979;
                xs.push_back(x);
                ys.push_back(std::sin(x) * std::exp(-x * 0.15));
            }
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.type = UI::GraphType::Area;
            desc.x_axis.title = "x";
            desc.y_axis.title = "damped sin(x)";
            desc.series.push_back(UI::SeriesRef{
                .name = "damped", .x = xs, .y = ys, .color = UI::Color{0.55, 0.85, 1.0, 1.0}, .line_width = 2.0f,
                .feather_px = 1.0f, .area_fill_opacity = 0.4f,
            });
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-area"},
                            },
                            desc, state);
        }

        // Bar (grouped): two series across five categories.
        {
            const std::vector<f64> a{4.0, 7.0, 3.0, 8.0, 5.0};
            const std::vector<f64> b{6.0, 2.0, 5.0, 4.0, 7.0};
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.type = UI::GraphType::Bar;
            desc.bar_stack_mode = UI::BarStackMode::Grouped;
            desc.x_axis.is_categorical = true;
            desc.x_axis.categories = {"Mon", "Tue", "Wed", "Thu", "Fri"};
            desc.y_axis.title = "value";
            desc.series.push_back(UI::SeriesRef{.name = "A", .y = a, .color = UI::Color{0.4, 0.75, 1.0, 1.0}});
            desc.series.push_back(UI::SeriesRef{.name = "B", .y = b, .color = UI::Color{1.0, 0.6, 0.35, 1.0}});
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-bar-grouped"},
                            },
                            desc, state);
        }

        // Bar (stacked): same data, stacked instead of grouped.
        {
            const std::vector<f64> a{4.0, 7.0, 3.0, 8.0, 5.0};
            const std::vector<f64> b{6.0, 2.0, 5.0, 4.0, 7.0};
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.type = UI::GraphType::Bar;
            desc.bar_stack_mode = UI::BarStackMode::Stacked;
            desc.x_axis.is_categorical = true;
            desc.x_axis.categories = {"Mon", "Tue", "Wed", "Thu", "Fri"};
            desc.y_axis.title = "value";
            desc.series.push_back(UI::SeriesRef{.name = "A", .y = a, .color = UI::Color{0.4, 0.75, 1.0, 1.0}});
            desc.series.push_back(UI::SeriesRef{.name = "B", .y = b, .color = UI::Color{1.0, 0.6, 0.35, 1.0}});
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-bar-stacked"},
                            },
                            desc, state);
        }

        // Scatter: a noisy point cloud around a rising trend.
        {
            std::vector<f64> xs;
            std::vector<f64> ys;
            xs.reserve(80);
            ys.reserve(80);
            u32 rng_state = 1234567u;
            const auto next_unit = [&]() -> f64 {
                rng_state = rng_state * 1664525u + 1013904223u;
                return static_cast<f64>(rng_state) / static_cast<f64>(std::numeric_limits<u32>::max());
            };
            for (int i = 0; i < 80; ++i) {
                const f64 x = static_cast<f64>(i) / 79.0 * 10.0;
                xs.push_back(x);
                ys.push_back(x * 0.6 + (next_unit() - 0.5) * 4.0);
            }
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.type = UI::GraphType::Scatter;
            desc.x_axis.title = "x";
            desc.y_axis.title = "y";
            desc.series.push_back(UI::SeriesRef{
                .name = "samples", .x = xs, .y = ys, .color = UI::Color{0.85, 0.55, 1.0, 0.85}, .marker_radius = 3.5f,
            });
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-scatter"},
                            },
                            desc, state);
        }

        // Pie / donut: five wedges with a hole cut in the middle.
        {
            UI::GraphDesc desc{};
            desc.font_id = font_id_;
            desc.type = UI::GraphType::Pie;
            desc.pie_style.hole_ratio = 0.55f;
            desc.pie_style.gap_degrees = 2.0f;
            desc.pie_slices = {
                UI::PieSlice{.name = "Rendering", .value = 38.0, .color = UI::Color{0.4, 0.75, 1.0, 1.0}},
                UI::PieSlice{.name = "Physics", .value = 22.0, .color = UI::Color{1.0, 0.6, 0.35, 1.0}},
                UI::PieSlice{.name = "Audio", .value = 12.0, .color = UI::Color{0.65, 1.0, 0.55, 1.0}},
                UI::PieSlice{.name = "Scripting", .value = 18.0, .color = UI::Color{0.85, 0.55, 1.0, 1.0}},
                UI::PieSlice{.name = "Other", .value = 10.0, .color = UI::Color{0.6, 0.62, 0.68, 1.0}},
            };
            UI::GraphState state{};
            (void)UI::graph(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(chart_height + 40.0f), UI::SizingAxis::fixed(chart_height)},
                                .id = UString{"workbench-graphs-pie"},
                            },
                            desc, state);
        }

        // Frame graph: synthetic per-frame CPU/GPU-style stage timings, pushed once per frame to
        // build a rolling stacked-bar window -- the shape UI::frame_graph() is purpose-built for.
        {
            surface.frame_graph_phase += delta_seconds;
            const f32 t = surface.frame_graph_phase;
            const std::array<f64, 3> sample{
                4.0 + 2.0 * std::sin(static_cast<f64>(t) * 1.3),
                2.5 + 1.0 * std::sin(static_cast<f64>(t) * 2.1 + 1.0),
                1.0 + 0.5 * std::sin(static_cast<f64>(t) * 0.7 + 2.0),
            };
            UI::FrameGraphDesc fg_desc{};
            fg_desc.font_id = font_id_;
            fg_desc.segments = {
                UI::FrameGraphSegmentDef{.name = "Scene", .color = UI::Color{0.4, 0.75, 1.0, 1.0}},
                UI::FrameGraphSegmentDef{.name = "Shadows", .color = UI::Color{1.0, 0.6, 0.35, 1.0}},
                UI::FrameGraphSegmentDef{.name = "Post", .color = UI::Color{0.65, 1.0, 0.55, 1.0}},
            };
            fg_desc.window_size = 90;
            fg_desc.y_axis.title = "ms";
            UI::frame_graph_push(surface.frame_graph_state, fg_desc, sample);

            section_label(ctx, font_id_, "FRAME GRAPH");
            (void)UI::frame_graph(ctx,
                                  UI::ElementDecl{
                                      .sizing = {UI::SizingAxis::fixed(chart_width), UI::SizingAxis::fixed(chart_height)},
                                      .id = UString{"workbench-graphs-framegraph"},
                                  },
                                  fg_desc, surface.frame_graph_state);
        }
    }

    /// Builds console panel.
    ///
    /// Virtualized: only the lines estimated to be scrolled into view (`first_visible_line` ..
    /// `+visible_line_count`, computed by the caller in `build_frame`) get a real `draw_text`
    /// element each frame; everything outside that range collapses into one fixed-height spacer
    /// each side. Without this, every captured line got its own `draw_text` every single frame
    /// regardless of scroll position or even whether it was clipped — with the 1000-line capture
    /// cap this panel keeps, that is up to 1000 text-layout-and-draw calls a frame, indefinitely,
    /// for as long as the Console tab stayed selected. That is a real, unbounded, purely CPU-side
    /// cost (this function only runs while Console is the active tab — `panel_content_region`
    /// returns nothing for a background tab — so it does not run in general, but it always runs
    /// in full while you are actually looking at it), and exactly what a profiler or a repeated
    /// "Long frame detected" warning showing up *in the console's own captured log* would surface.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param ctx `ctx` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param first_visible_line Index into the captured-line buffer of the first line to build a
    ///        real text element for.
    /// @param visible_line_count How many lines starting at `first_visible_line` to build real
    ///        text elements for.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::build_console_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds,
                                          usize first_visible_line, usize visible_line_count) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-console-body"},
        });
        panel_heading(ctx, font_id_, "DIAGNOSTICS", "Console",
                      "Every engine log message, captured through Foundation::add_log_sink — the same hook "
                      "sturdy_log_add_sink exposes over the FFI so a foreign-language host can build its own "
                      "console instead of only ever seeing this process's stdout.");

        {
            auto controls = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .child_gap = 14,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .id = UString{"workbench-console-controls"},
            });
            (void)controls;

            {
                const UI::ToggleResult autoscroll = UI::checkbox(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(20.0f), UI::SizingAxis::fixed(20.0f)},
                        .id = UString{"workbench-console-autoscroll"},
                    },
                    toggle_style(), console_autoscroll_toggle_state_, delta_seconds, console_autoscroll_, font_id_);
                if (autoscroll.clicked) {
                    console_autoscroll_ = !console_autoscroll_;
                }
            }
            {
                // `UI::button`'s `ButtonResult::scope` is an open element scope — everything drawn
                // while this local is alive nests *inside* the button (that's how a button gets a
                // label), so each button+label pair needs its own block. Leaving one open across
                // the next `UI::button(...)` call nests that entire next button inside this one
                // instead of placing it as a sibling.
                auto label = ctx.element(UI::ElementDecl{.sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()}});
                (void)label;
                draw_text(ctx, "Autoscroll", text_style(font_id_, text_secondary, 12));
            }
            {
                const UI::ButtonResult clear = UI::button(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(90.0f), UI::SizingAxis::fixed(28.0f)},
                        .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                        .id = UString{"workbench-console-clear"},
                    },
                    action_button_style(), console_clear_button_state_, delta_seconds);
                draw_text(ctx, "Clear", text_style(font_id_, text_primary, 12));
                if (clear.clicked) {
                    const std::lock_guard<std::mutex> lock(console_log_mutex_);
                    console_log_lines_.clear();
                    console_log_dropped_ = 0;
                }
            }
            {
                const UI::ButtonResult emit_test = UI::button(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(150.0f), UI::SizingAxis::fixed(28.0f)},
                        .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                        .id = UString{"workbench-console-emit-test"},
                    },
                    action_button_style(), console_test_log_button_state_, delta_seconds);
                draw_text(ctx, "Emit test message", text_style(font_id_, text_primary, 12));
                if (emit_test.clicked) {
                    Foundation::log_info("UiWorkbench: console test message emitted at your request.");
                }
            }
        }

        // Copied out under the lock so the (comparatively slow) per-line layout/draw work below
        // never runs while holding it — the sink can otherwise be invoked from any engine thread
        // at any time, including mid-frame here.
        std::deque<ConsoleLine> lines_copy;
        u64 dropped = 0;
        {
            const std::lock_guard<std::mutex> lock(console_log_mutex_);
            lines_copy = console_log_lines_;
            dropped = console_log_dropped_;
        }

        section_label(ctx, font_id_,
                     (std::to_string(lines_copy.size()) + " line(s) captured" +
                      (dropped > 0 ? (", " + std::to_string(dropped) + " older line(s) dropped") : std::string{}))
                         .c_str());

        if (lines_copy.empty()) {
            draw_text(ctx, "No log messages captured yet.", text_style(font_id_, text_secondary, 13));
            return;
        }

        auto log_list = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .child_gap = 2,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-console-lines"},
        });
        (void)log_list;

        const usize total = lines_copy.size();
        const usize begin_index = std::min(first_visible_line, total);
        const usize end_index = std::min(begin_index + visible_line_count, total);

        if (begin_index > 0) {
            auto spacer = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(static_cast<f32>(begin_index) * console_line_height_px)},
            });
            (void)spacer;
        }
        for (usize i = begin_index; i < end_index; ++i) {
            const ConsoleLine &line = lines_copy[i];
            const UI::Color color = line.level == Foundation::LogLevel::Error ||
                                            line.level == Foundation::LogLevel::Critical
                                        ? danger
                                    : line.level == Foundation::LogLevel::Warn ? warning
                                    : line.level == Foundation::LogLevel::Debug ||
                                            line.level == Foundation::LogLevel::Trace
                                        ? text_secondary
                                        : text_primary;
            draw_text(ctx, line.text, text_style(font_id_, color, 12));
        }
        if (end_index < total) {
            auto spacer = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(),
                          UI::SizingAxis::fixed(static_cast<f32>(total - end_index) * console_line_height_px)},
            });
            (void)spacer;
        }
    }

    /// Performs the handle dock events operation for `UiWorkbench` using the supplied arguments.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param events Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void WorkbenchUi::handle_dock_events(Engine::Engine &engine, Surface &surface, UI::Docking::DockWorkspaceEvents events) {
        for (const UI::Docking::DockTearOffRequest &request : events.tear_off_requests) {


            if (const Engine::WindowSnapshot *origin_window = engine.window_state().find(surface.handle.window_id)) {


                const glm::vec2 physical_local = kWorkspaceOrigin + request.workspace_local_drop_position;
                const glm::vec2 framebuffer_size{origin_window->framebuffer_size};
                const glm::vec2 logical_size{origin_window->size};
                const glm::vec2 physical_to_logical = framebuffer_size.x > 0.0f && framebuffer_size.y > 0.0f
                                                          ? logical_size / framebuffer_size
                                                          : glm::vec2{1.0f};
                const glm::vec2 global_drop = glm::vec2{origin_window->position} + physical_local * physical_to_logical;
                std::optional<WindowManager::WindowId> redock_target;
                for (const Engine::WindowSnapshot &candidate : engine.window_state().windows()) {
                    if (candidate.id == surface.handle.window_id) {
                        continue;
                    }
                    const glm::vec2 candidate_position{candidate.position};
                    const glm::vec2 candidate_size{candidate.size};
                    if (global_drop.x >= candidate_position.x && global_drop.x <= candidate_position.x + candidate_size.x &&
                        global_drop.y >= candidate_position.y && global_drop.y <= candidate_position.y + candidate_size.y) {
                        redock_target = candidate.id;
                        break;
                    }
                }
                if (redock_target &&
                    dock_coordinator_.transfer_panel(surface.handle.window_id, *redock_target, request.panel)) {
                    status_message_ = "Panel redocked into another window.";
                    continue;
                }
            }

            const WindowManager::WindowConfig config{
                .title = "Sturdy UI — Detached Panel",
                .extent = {720, 620},
                .position = {0, 0},
                .use_default_position = true,
                .visible = true,
                .resizable = true,
                .decorated = true,
                .high_dpi = true,
                .transparent = true,
                .mode = WindowManager::WindowMode::Windowed,
                .graphics_api = WindowManager::WindowGraphicsApi::Vulkan,
            };
            const Engine::WindowRequestId request_id = dock_coordinator_.request_tear_off(
                surface.handle.window_id,
                request,
                config,
                engine.window_requests());
            status_message_ = request_id
                                  ? "Creating a secondary UI surface; the source panel remains until acceptance."
                                  : "Tear-off was rejected before window creation; no panel data changed.";
        }
        dock_coordinator_.request_empty_window_closes(engine.window_requests());
    }

    /// Builds overlay hooks.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param snapshot `snapshot` value used by the operation.
    ///
    /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Renderer::UiOverlayHooks WorkbenchUi::build_overlay_hooks(
        Engine::Engine &engine,
        Surface &surface,
        std::shared_ptr<UI::FrameSnapshot> snapshot) {
        Renderer::UiOverlayHooks hooks{};


        UI::UiRenderer *renderer = (hdr_enabled_ || swapchain_transparent_)
                                       ? &surface.hdr_renderer
                                       : &surface.sdr_renderer;
        hooks.hdr_reference_white_scale = static_cast<f32>(hdr_reference_white_scale_);
        Renderer::Renderer *texture_resolver = engine.renderer();
        const u64 renderer_generation = renderer->generation();
        hooks.prepare = [renderer, renderer_generation, snapshot, texture_resolver](
                            RHI::RhiDevice &device,
                            RHI::CommandEncoder &encoder,
                            glm::vec2 viewport_size,
                            Core::RenderSurfaceHandle render_surface,
                            u32 frame_resource_index,
                            std::vector<RHI::BufferHandle> &transient_buffers,
                            Renderer::TextAtlasRetiredResources &retired_resources)
            -> Core::RendererResult {
            if (renderer->generation() != renderer_generation) {
                return {};
            }
            const Core::Extent2D extent = snapshot->viewport_extent();
            if (viewport_size != glm::vec2{extent}) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UiWorkbench snapshot extent does not match its render surface.");
            }
            return renderer->prepare(device, encoder, *snapshot, texture_resolver, render_surface, frame_resource_index, transient_buffers, retired_resources);
        };
        hooks.draw = [renderer, renderer_generation](RHI::RenderPassEncoder &pass, glm::vec2 viewport_size, Core::RenderSurfaceHandle render_surface, u32 frame_resource_index) -> Core::RendererResult {
            return renderer->generation() == renderer_generation
                       ? renderer->draw(pass, viewport_size, render_surface, frame_resource_index)
                       : Core::RendererResult{};
        };
        return hooks;
    }

    /// Shuts down the `UiWorkbench` and releases associated runtime state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WorkbenchUi::shutdown(Engine::Engine &engine) noexcept {
        // Unregistered before anything it captures is torn down — Foundation::remove_log_sink
        // blocks until any in-flight callback invocation on another thread has returned, so no
        // call into console_log_lines_/console_log_mutex_ can start after this point.
        Foundation::remove_log_sink(console_log_sink_id_);

        if (RHI::RhiDevice *device = engine.rhi_device()) {
            for (auto &[window, surface] : surfaces_) {
                (void)window;
                surface->sdr_renderer.destroy(*device);
                surface->hdr_renderer.destroy(*device);
                surface->context.destroy();
            }
        } else {
            for (auto &[window, surface] : surfaces_) {
                (void)window;
                surface->context.destroy();
            }
        }
        surfaces_.clear();
    }

} // namespace SFT::UiWorkbench

namespace SFT::UiWorkbench {

    /// Returns the current or globally available effective background opacity value.
    ///
    /// @return Returns the current effective background opacity value.
    /// @note This function does not throw exceptions.
    f64 WorkbenchUi::effective_background_opacity() const noexcept {
        return swapchain_transparent_ ? ui_background_opacity_ : 1.0;
    }

} // namespace SFT::UiWorkbench

