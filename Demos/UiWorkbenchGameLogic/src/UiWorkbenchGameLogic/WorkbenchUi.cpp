#include <UiWorkbenchGameLogic/WorkbenchUi.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
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

        // UI stays Platform-agnostic (see UI.hpp's own doc comment), so nothing below Engine ever
        // translates UI::CursorIcon to Platform::Windowing::CursorIcon on its own — this is exactly
        // that Engine-level glue, applied once per surface per frame further down in render().
        // CursorIcon::Auto never actually reaches here (UI::Context::desired_cursor() always
        // resolves to a concrete value, defaulting to Default — see its own doc comment,
        // Context.hpp) but is still handled explicitly rather than left to fall through a switch's
        // default, so a future CursorIcon addition on either side fails to compile here instead of
        // silently mapping to the wrong glyph.
        [[nodiscard]] Platform::Windowing::CursorIcon to_platform_cursor_icon(UI::CursorIcon icon) {
            switch (icon) {
                case UI::CursorIcon::Auto:
                case UI::CursorIcon::Default: return Platform::Windowing::CursorIcon::Default;
                case UI::CursorIcon::Pointer: return Platform::Windowing::CursorIcon::Pointer;
                case UI::CursorIcon::Text: return Platform::Windowing::CursorIcon::Text;
                case UI::CursorIcon::Grab: return Platform::Windowing::CursorIcon::Grab;
                case UI::CursorIcon::Grabbing: return Platform::Windowing::CursorIcon::Grabbing;
                case UI::CursorIcon::ResizeHorizontal: return Platform::Windowing::CursorIcon::ResizeHorizontal;
                case UI::CursorIcon::ResizeVertical: return Platform::Windowing::CursorIcon::ResizeVertical;
                case UI::CursorIcon::ResizeNwse: return Platform::Windowing::CursorIcon::ResizeNwse;
                case UI::CursorIcon::ResizeNesw: return Platform::Windowing::CursorIcon::ResizeNesw;
                case UI::CursorIcon::NotAllowed: return Platform::Windowing::CursorIcon::NotAllowed;
            }
            return Platform::Windowing::CursorIcon::Default;
        }

        // Top-left of the dock workspace within its own window's client area (below the brand/title
        // bar built by hand at the top of build_frame(), inset from the side margins) — a fixed
        // offset, not viewport-dependent (only the workspace's *size* scales with the window).
        // handle_dock_events() needs this same constant to convert a DockTearOffRequest's
        // workspace-local drop position into window-local, then global, screen coordinates when
        // deciding whether a drag ended over another managed window instead of empty desktop.
        constexpr glm::vec2 kWorkspaceOrigin{18.0f, 58.0f};

        [[nodiscard]] UI::TextStyle text_style(UI::FontId font, UI::Color color = text_primary,
                                                u16 size = 14) {
            return UI::TextStyle{
                .color = color,
                .font_id = font,
                .font_size = size,
                .wrap_mode = UI::TextWrapMode::Words,
            };
        }

        [[nodiscard]] std::string number_text(const char *prefix, f64 value, const char *suffix = "") {
            std::array<char, 96> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s%.2f%s", prefix, value, suffix);
            return std::string{buffer.data()};
        }

        [[nodiscard]] std::string fps_text(f32 fps) {
            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%.0f FPS", static_cast<f64>(std::max(fps, 0.0f)));
            return std::string{buffer.data()};
        }

        // Thresholds match a typical 60Hz-display expectation: comfortably above it is "good," a
        // choppy-but-usable mid range, and anything below 30 calls out real frame drops.
        [[nodiscard]] UI::Color fps_color(f32 fps, UI::Color good, UI::Color mid, UI::Color bad) {
            if (fps >= 55.0f) {
                return good;
            }
            return fps >= 30.0f ? mid : bad;
        }

        [[nodiscard]] std::string rgba_text(const UI::Color &color) {
            std::array<char, 128> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "sRGB  %.3f   %.3f   %.3f   alpha %.3f",
                          color.r, color.g, color.b, color.a);
            return std::string{buffer.data()};
        }

        void draw_text(UI::Context &ctx, std::string_view content, const UI::TextStyle &style) {
            const ustr borrowed{content};
            ctx.text(borrowed, style);
        }


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

        void panel_heading(UI::Context &ctx, UI::FontId font, std::string_view eyebrow,
                           std::string_view title, std::string_view description) {
            draw_text(ctx, eyebrow, text_style(font, accent, 12));
            draw_text(ctx, title, text_style(font, text_primary, 23));
            draw_text(ctx, description, text_style(font, text_secondary, 13));
        }

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

        [[nodiscard]] UI::DropdownOption dropdown_option(UI::FontId font, const char *label,
                                                          UI::Color swatch) {
            return UI::DropdownOption{.build = [font, label, swatch](UI::Context &ctx) {
                auto row = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fit()},
                    .child_gap = 9,
                    .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                });
                {
                    // Braced so this scope closes here — without it, ElementScope's RAII close
                    // doesn't run until the end of the lambda, and draw_text() below ends up as a
                    // *child* of this 10x10 dot instead of its sibling, collapsing the label's
                    // wrapped lines on top of each other inside the dot's own tiny box.
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
        Surface(Core::RenderSurfaceHandle surface_handle, UI::Context &&ui_context,
                UI::UiRenderer &&ui_renderer, UString workspace_id,
                UI::Docking::DockWorkspaceStyle workspace_style, bool is_primary)
            : handle(surface_handle),
              context(std::move(ui_context)),
              renderer(std::move(ui_renderer)),
              workspace(std::move(workspace_id), workspace_style),
              primary(is_primary) {}

        Core::RenderSurfaceHandle handle{};
        UI::Context context{};
        UI::UiRenderer renderer{};
        UI::PointerState pointer{};
        UI::Docking::DockWorkspace workspace;
        bool primary = false;
        std::vector<UI::SliderKey> slider_keys;
        std::vector<UI::ColorPickerKey> color_keys;
        // Per-frame text-editing input for the Text Lab widgets, accumulated by route_input()'s
        // system and drained after each build_frame() the same way slider_keys/color_keys are.
        // shift/ctrl are held-state (tracked across frames from press/release), not per-frame edges.
        std::vector<UI::EditKey> edit_keys;
        std::string typed_text;
        bool shift_down = false;
        bool ctrl_down = false;
        // One fade/drag state per panel content region — panel_content_region()'s own id is stable
        // per panel regardless of which tab/split it's currently docked into, so these persist a
        // panel's scrollbar feel (mid-fade, mid-drag) across reordering/resizing, only resetting if
        // the panel tears off into a different Surface entirely.
        UI::ScrollAreaState controls_scroll{};
        UI::ScrollAreaState color_scroll{};
        UI::ScrollAreaState composition_scroll{};
        UI::ScrollAreaState text_scroll{};
        UI::ScrollAreaState docking_scroll{};
        UI::ScrollAreaState metrics_scroll{};

        // Exponentially-smoothed instantaneous FPS (1/delta_seconds), updated once per build_frame()
        // — negative means "no sample yet," so the header shows the very first frame's raw value
        // instead of blending from zero.
        f32 fps_smoothed = -1.0f;

        // This surface's last completed frame's CPU/GPU pass timing breakdown, refreshed once per
        // render() call from Renderer::last_frame_timings() before build_frame() runs — same
        // once-per-frame refresh pattern as fps_smoothed above, just sourced from the engine instead
        // of computed locally. See build_metrics_panel() (the "Performance" tab) for its consumer.
        Renderer::FrameTimingSnapshot last_timing_snapshot{};
    };

    WorkbenchUi::WorkbenchUi() {
        render_graph_ = Engine::RenderGraph::overlay_only();
        render_graph_.set_execution_mode(Engine::RenderGraphExecutionMode::WaitForCompletion);
        // overlay_only() leaves debug_overlay.enabled true (it's the CPU/GPU timing collection this
        // workbench wants), but the burned-in on-screen text block it used to also draw is redundant
        // with the "Performance" dock panel below (build_metrics_panel), which reads the same numbers
        // via Renderer::last_frame_timings() — so only the text draw is turned off here.
        render_graph_.debug_overlay().draw_text = false;
    }

    WorkbenchUi::~WorkbenchUi() = default;

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
        Foundation::log_info(
            "UiWorkbench: loaded font 'Fonts/MapleMono-NF-Regular.ttf' ({} bytes) in {}",
            font_bytes->size(),
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

        // Blur-type dropdown (Composition panel): only offer kinds this OS build actually reports as
        // supported (see WindowEffectKind::Transparent's doc comment for why this query exists) —
        // detected once, at startup, rather than hardcoding a per-platform list here.
        static constexpr std::array<Platform::Windowing::WindowEffectKind, 5> candidate_blur_kinds{
            Platform::Windowing::WindowEffectKind::Blur,
            Platform::Windowing::WindowEffectKind::Acrylic,
            Platform::Windowing::WindowEffectKind::Mica,
            Platform::Windowing::WindowEffectKind::MicaAlt,
            Platform::Windowing::WindowEffectKind::Tabbed,
        };
        for (Platform::Windowing::WindowEffectKind kind : candidate_blur_kinds) {
            if (Platform::Windowing::operating_system_may_support_window_effect(kind)) {
                supported_blur_kinds_.push_back(kind);
            }
        }

        route_input(engine);
        return {};
    }

    WorkbenchUi::Surface *WorkbenchUi::find_surface(
        Platform::Windowing::WindowId window) noexcept {
        const auto found = surfaces_.find(window);
        return found != surfaces_.end() ? found->second.get() : nullptr;
    }

    WorkbenchUi::Surface *WorkbenchUi::ensure_surface(
        Engine::Engine &engine, Core::RenderSurfaceHandle handle, bool primary) {
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
        auto renderer = UI::UiRenderer::create(*device, RHI::Format::BGRA8UnormSrgb);
        if (!renderer) {
            Foundation::log_error("UiWorkbench: failed to create a UI renderer for a window.");
            context->destroy();
            return nullptr;
        }
        const usize numeric_window = static_cast<usize>(handle.window_id);
        auto surface = std::make_unique<Surface>(
            handle, std::move(*context), std::move(*renderer),
            UString{"ui-workbench-window-" + std::to_string(numeric_window)},
            dock_style(font_id_), primary);
        surface->context.register_font(font_id_, font_);

        Surface *result = surface.get();
        surfaces_.emplace(handle.window_id, std::move(surface));
        dock_coordinator_.register_workspace(handle.window_id, result->workspace, primary, !primary);

        if (primary) {
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"composition"}, .title = UString{"Composition"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"controls"}, .title = UString{"Slider Lab"}, .closable = true});
            const UI::Docking::DockNodeId controls_leaf = *result->workspace.focused_leaf();
            (void)result->workspace.add_panel(
                UI::Docking::DockPanelDesc{
                    .id = UString{"color"}, .title = UString{"Color Studio"}, .closable = true},
                UI::Docking::DockPlacement{
                    .target_node = controls_leaf, .zone = UI::Docking::DockDropZone::Right});
            const UI::Docking::DockNodeId color_leaf = *result->workspace.focused_leaf();
            (void)result->workspace.add_panel(
                UI::Docking::DockPanelDesc{
                    .id = UString{"docking"}, .title = UString{"Docking Guide"}, .closable = false},
                UI::Docking::DockPlacement{
                    .target_node = color_leaf, .zone = UI::Docking::DockDropZone::Bottom});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"text"}, .title = UString{"Text Lab"}, .closable = true});
            (void)result->workspace.add_panel(UI::Docking::DockPanelDesc{
                .id = UString{"metrics"}, .title = UString{"Performance"}, .closable = true});
        }
        return result;
    }

    void WorkbenchUi::route_input(Engine::Engine &engine) {
        engine.update_schedule().add_system(
            [this](Ecs::EventReader<Engine::MouseMoveEvent> moves,
                   Ecs::EventReader<Engine::MouseButtonEvent> buttons,
                   Ecs::EventReader<Engine::MouseWheelEvent> wheels,
                   Ecs::EventReader<Engine::KeyboardEvent> keys,
                   Ecs::EventReader<Engine::TextInputEvent> text_events,
                   Ecs::EventReader<Engine::WindowStateEvent> window_events) noexcept {
                for (const Engine::TextInputEvent &event : text_events.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->typed_text += event.text.utf8;
                    }
                }
                for (const Engine::MouseMoveEvent &event : moves.read()) {
                    if (Surface *surface = find_surface(event.window)) {
                        surface->pointer.position = {event.mouse.x, event.mouse.y};
                    }
                }
                for (const Engine::MouseButtonEvent &event : buttons.read()) {
                    if (event.mouse.button_code != Platform::Windowing::MouseButton::Left) {
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
                        surface->pointer.scroll_delta += glm::vec2{event.wheel.x, event.wheel.y};
                    }
                }
                for (const Engine::WindowStateEvent &event : window_events.read()) {
                    if (event.kind == Platform::Windowing::WindowEventKind::FocusLost ||
                        event.kind == Platform::Windowing::WindowEventKind::MouseLeft) {
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
                    // Held-state modifiers need both press AND release, unlike everything below —
                    // handled before the pressed()-only gate.
                    if (event.key_code == Engine::KeyboardKey::LeftShift ||
                        event.key_code == Engine::KeyboardKey::RightShift) {
                        surface->shift_down = event.pressed();
                        continue;
                    }
                    if (event.key_code == Engine::KeyboardKey::LeftControl ||
                        event.key_code == Engine::KeyboardKey::RightControl) {
                        surface->ctrl_down = event.pressed();
                        continue;
                    }
                    if (!event.pressed()) {
                        continue;
                    }
                    // Text-editing intents for the Text Lab widgets (TextEditInput's contract:
                    // Ctrl+A/C/X/V arrive pre-resolved as SelectAll/Copy/Cut/Paste, not as letters
                    // plus a modifier the widget would have to interpret).
                    if (surface->ctrl_down) {
                        switch (event.key_code) {
                            case Engine::KeyboardKey::A: surface->edit_keys.push_back(UI::EditKey::SelectAll); break;
                            case Engine::KeyboardKey::C: surface->edit_keys.push_back(UI::EditKey::Copy); break;
                            case Engine::KeyboardKey::X: surface->edit_keys.push_back(UI::EditKey::Cut); break;
                            case Engine::KeyboardKey::V: surface->edit_keys.push_back(UI::EditKey::Paste); break;
                            default: break;
                        }
                    }
                    switch (event.key_code) {
                        case Engine::KeyboardKey::Left: surface->edit_keys.push_back(UI::EditKey::Left); break;
                        case Engine::KeyboardKey::Right: surface->edit_keys.push_back(UI::EditKey::Right); break;
                        case Engine::KeyboardKey::Up: surface->edit_keys.push_back(UI::EditKey::Up); break;
                        case Engine::KeyboardKey::Down: surface->edit_keys.push_back(UI::EditKey::Down); break;
                        case Engine::KeyboardKey::Home: surface->edit_keys.push_back(UI::EditKey::Home); break;
                        case Engine::KeyboardKey::End: surface->edit_keys.push_back(UI::EditKey::End); break;
                        case Engine::KeyboardKey::Backspace: surface->edit_keys.push_back(UI::EditKey::Backspace); break;
                        case Engine::KeyboardKey::Delete: surface->edit_keys.push_back(UI::EditKey::Delete); break;
                        case Engine::KeyboardKey::Enter: surface->edit_keys.push_back(UI::EditKey::Enter); break;
                        case Engine::KeyboardKey::Escape: surface->edit_keys.push_back(UI::EditKey::Escape); break;
                        default: break;
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
            if (completion.kind == Engine::WindowRequestKind::Close && completion.accepted &&
                (!primary_window_ || completion.window != *primary_window_)) {
                if (auto found = surfaces_.find(completion.window); found != surfaces_.end()) {
                    if (RHI::RhiDevice *device = engine.rhi_device()) {
                        found->second->renderer.destroy(*device);
                    }
                    found->second->context.destroy();
                    surfaces_.erase(found);
                }
            }
        }
    }

    std::optional<Engine::RenderFrameParameters> WorkbenchUi::render(
        Engine::Engine &engine, Core::RenderSurfaceHandle handle, const Core::FrameInput &frame) {
        process_window_completions(engine);
        if (!primary_window_) {
            primary_window_ = handle.window_id;
        }
        Surface *surface = ensure_surface(engine, handle, handle.window_id == *primary_window_);
        if (surface == nullptr || frame.framebuffer_width == 0 || frame.framebuffer_height == 0) {
            return std::nullopt;
        }

        if (Renderer::Renderer *renderer = engine.renderer(); renderer != nullptr) {
            surface->last_timing_snapshot = renderer->last_frame_timings(handle);
        }

        const glm::vec2 viewport{static_cast<f32>(frame.framebuffer_width),
                                 static_cast<f32>(frame.framebuffer_height)};
        UI::Docking::DockWorkspaceEvents dock_events =
            build_frame(engine, *surface, viewport, static_cast<f32>(frame.delta_seconds));
        handle_dock_events(engine, *surface, std::move(dock_events));
        // Fire-and-forget through the same deferred WindowRequests queue spawn/close already use —
        // GameLogic never holds a live Platform::Windowing::Window* (see WindowRequests.hpp's own
        // doc comment). One frame of latency between a hover changing and the OS cursor actually
        // updating is imperceptible, unlike the round-trip a spawn/close request needs.
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

    UI::Docking::DockWorkspaceEvents WorkbenchUi::build_frame(
        Engine::Engine &engine, Surface &surface, glm::vec2 viewport, f32 delta_seconds) {
        surface.context.begin_layout(viewport, surface.pointer, delta_seconds);
        surface.pointer.pressed = false;
        surface.pointer.press_position.reset();
        surface.pointer.released = false;
        surface.pointer.cancelled = false;
        surface.pointer.scroll_delta = glm::vec2{0.0f};

        if (delta_seconds > 0.0f) {
            const f32 instantaneous_fps = 1.0f / delta_seconds;
            // Blend toward the instantaneous reading rather than snapping straight to it — a raw
            // per-frame 1/delta reading jitters wildly frame to frame, which would make the counter
            // itself unreadable.
            const f32 blend = std::min(1.0f, 6.0f * delta_seconds);
            surface.fps_smoothed =
                surface.fps_smoothed < 0.0f ? instantaneous_fps : surface.fps_smoothed + (instantaneous_fps - surface.fps_smoothed) * blend;
        }

        {
            // Full-viewport so it fills the margins around the dock workspace below (kWorkspaceOrigin
            // insets the workspace by 18px/58px/14px on the left/top/right+bottom — DockWorkspaceStyle
            // ::content_background paints the workspace's own interior with this same `canvas` color,
            // but not those outer margins). Its own header content (brand/title/status pill) must stay
            // pinned to a thin strip at the *top* — AlignY::Top, not Center — since Center would
            // vertically center that content across the *entire* window height instead of a header-
            // sized band, drifting it down into (and behind/in front of, depending on paint order) the
            // workspace's own panels below y ~= 58, which is what made the status pill appear to
            // "clip in and out" as whatever panel content sat underneath it changed.
            auto background = surface.context.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(viewport.x), UI::SizingAxis::fixed(viewport.y)},
                .padding = UI::Padding::symmetric(18, 12),
                .child_gap = 10,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Top},
                .background_color = canvas,
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
                draw_text(surface.context, "composable controls · perceptual color · live docking",
                          text_style(font_id_, text_secondary, 10));
            }
            {
                // Braced so this closes here — otherwise status_pill() below nests inside this
                // 1px-tall spacer instead of following it as a sibling, breaking the "empty grow
                // spacer pushes the next sibling to the far edge" layout this is meant to produce
                // (same ElementScope-lifetime mistake as dropdown_option()'s "dot" above).
                auto spacer = surface.context.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(1.0f)}});
                (void)spacer;
            }
            if (surface.fps_smoothed >= 0.0f) {
                status_pill(surface.context, font_id_, fps_text(surface.fps_smoothed),
                            fps_color(surface.fps_smoothed, success, warning, danger));
            }
            status_pill(surface.context, font_id_, surface.primary ? "PRIMARY SURFACE" : "DETACHED SURFACE",
                        surface.primary ? success : accent_hot);
        }

        const glm::vec2 workspace_size{std::max(viewport.x - 36.0f, 1.0f),
                                       std::max(viewport.y - 72.0f, 1.0f)};
        surface.workspace.begin_frame(
            surface.context,
            UI::Docking::DockRect{.origin = kWorkspaceOrigin, .size = workspace_size},
            delta_seconds);

        // scroll_area() (ScrollArea.hpp) opens the panel's own clip container itself — decl already
        // carries a stable id and .clip.vertical=true from panel_content_region(), so it's passed
        // straight through rather than opened as a plain ctx.element() first.
        if (auto decl = surface.workspace.panel_content_region(UString{"controls"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.controls_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_controls_panel(surface, ctx, delta_seconds);
                                  });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"color"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.color_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_color_panel(surface, ctx, delta_seconds);
                                  });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"composition"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.composition_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_composition_panel(engine, surface, ctx, delta_seconds);
                                  });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"text"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.text_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_text_panel(surface, ctx, delta_seconds);
                                  });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"docking"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.docking_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_docking_panel(surface, ctx, delta_seconds);
                                  });
        }
        if (auto decl = surface.workspace.panel_content_region(UString{"metrics"})) {
            (void)UI::scroll_area(surface.context, decl->id, *decl, scrollbar_style_, surface.metrics_scroll,
                                  delta_seconds, [&](UI::Context &ctx) {
                                      build_metrics_panel(surface, ctx, delta_seconds);
                                  });
        }

        UI::Docking::DockWorkspaceEvents events = surface.workspace.end_frame(surface.context);
        surface.slider_keys.clear();
        surface.color_keys.clear();
        surface.edit_keys.clear();
        surface.typed_text.clear();
        return events;
    }

    void WorkbenchUi::build_controls_panel(Surface &surface, UI::Context &ctx, f32 /*delta_seconds*/) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 17,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-controls-body"},
        });
        panel_heading(ctx, font_id_, "RANGE INPUT", "Slider Lab",
                      "Track-click, drag, focus, tick, reverse and keyboard semantics in engine-native UI.");

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
        draw_text(ctx, number_text("Current value  ", exposure_, "%"),
                  text_style(font_id_, text_primary, 14));

        const std::array ticks{
            UI::SliderTick{.value = 0.0}, UI::SliderTick{.value = 25.0},
            UI::SliderTick{.value = 50.0}, UI::SliderTick{.value = 75.0},
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
            // No focus-ring rectangle around the whole track — the demo's click-and-drag bars stay
            // outline-free at every state, including focused.
            .focused_border = UI::BorderStyle{},
        };
        UI::SliderComposition composition{};
        composition.track.enabled = slider_enabled_;
        composition.marker.visible = show_slider_markers_;
        composition.marker_label.visible = show_slider_markers_;
        composition.marker_label.build = [](UI::Context &part_ctx,
                                                const UI::SliderPartContext &part) {
            if (part.marker_value) {
                draw_text(part_ctx, number_text("", *part.marker_value),
                          text_style(font_id_, text_secondary, 9));
            }
        };
        composition.thumb.alter_decl = [this](UI::ElementDecl &decl,
                                              const UI::SliderPartContext &part) {
            if (!use_custom_thumb_) {
                return;
            }
            decl.sizing = {UI::SizingAxis::fixed(26.0f), UI::SizingAxis::fixed(26.0f)};
            decl.corner_radius = UI::CornerRadius::all(8.0f);
            // composition.thumb.build below adds an 8x8 "core" dot as a plain (non-floating) child
            // of this element — without centering it here, it inherits the default Left/Top child
            // alignment and pins itself into the thumb's top-left corner instead of its middle.
            decl.child_alignment = {.x = UI::AlignX::Center, .y = UI::AlignY::Center};
            decl.background_color = part.visual.active ? accent_hot : text_primary;
            // No ring around the thumb — keep it a plain filled circle with its accent core dot.
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
        composition.tooltip.build = [](UI::Context &part_ctx,
                                       const UI::SliderPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(9, 5),
                .background_color = panel_raised,
                .corner_radius = UI::CornerRadius::all(7.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(part_ctx, number_text("", part.value, "%"),
                      text_style(font_id_, text_primary, 12));
        };

        const UI::SliderResult exposure_result = UI::slider(
            ctx,
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(340.0f), UI::SizingAxis::fixed(42.0f)},
                .id = UString{"workbench-exposure"},
            },
            config, style, exposure_slider_state_, exposure_,
            UI::SliderInput{.keys = surface.slider_keys}, slider_enabled_, composition);
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
                vertical_config, vertical_style, rotation_slider_state_, rotation_,
                UI::SliderInput{.keys = surface.slider_keys}, slider_enabled_);
            rotation_ = rotation_result.value;
            {
                auto details = ctx.element(UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(250.0f), UI::SizingAxis::fit()},
                    .padding = UI::Padding::all(14),
                    .child_gap = 7,
                    .direction = UI::LayoutDirection::TopToBottom,
                    .background_color = panel,
                    .corner_radius = UI::CornerRadius::all(12.0f),
                    .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
                });
                draw_text(ctx, number_text("Rotation  ", rotation_, "°"),
                          text_style(font_id_, text_primary, 15));
                draw_text(ctx, "step=any · min at top · full captured drag",
                          text_style(font_id_, text_secondary, 11));
                draw_text(ctx, "Click either track, then use arrows, Page Up/Down, Home or End.",
                          text_style(font_id_, text_secondary, 11));
            }
        }
    }

    void WorkbenchUi::build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 14,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-color-body"},
        });
        panel_heading(ctx, font_id_, "FOUNDATION COLOR", "Color Studio",
                      "Pick visually, preserve alpha, then inspect the selected strongly typed color space.");

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
        style.color_space_dropdown.list_background = panel_raised;
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
        composition.tooltip.build = [](UI::Context &part_ctx,
                                       const UI::ColorPickerPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(9, 5),
                .background_color = panel_raised,
                .corner_radius = UI::CornerRadius::all(7.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(part_ctx, rgba_text(part.color), text_style(font_id_, text_primary, 10));
        };

        const UI::ColorPickerResult result = UI::color_picker(
            ctx, UString{"workbench-color-picker"},
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(14),
                .child_gap = 10,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = panel,
                .corner_radius = UI::CornerRadius::all(14.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            },
            config, style, color_picker_state_, selected_color_,
            UI::ColorPickerInput{.keys = surface.color_keys, .delta_seconds = delta_seconds},
            picker_enabled_, composition);
        selected_color_ = result.color;
        selected_color_value_ = result.value;
        selected_color_space_ = result.color_space;

        {
            auto value_card = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(13),
                .child_gap = 6,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = panel_raised,
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });
            draw_text(ctx, "Typed Foundation value", text_style(font_id_, accent, 11));
            // Exactly one line, in the *selected* space's own terms — this used to print two
            // near-identical hardcoded-"sRGB" readouts (the raw color and its round-trip) no matter
            // which space the dropdown had picked.
            const std::span<const UI::ColorPickerComponent> components =
                UI::color_picker_components(selected_color_space_);
            const std::array<f64, 4> values = UI::color_picker_component_values(selected_color_value_);
            std::array<char, 160> typed_line{};
            std::snprintf(typed_line.data(), typed_line.size(), "%s   %s %.3f   %s %.3f   %s %.3f   alpha %.3f",
                          UI::color_picker_space_name(selected_color_space_),
                          components[0].label, values[0],
                          components[1].label, values[1],
                          components[2].label, values[2],
                          values[3]);
            draw_text(ctx, typed_line.data(), text_style(font_id_, text_primary, 12));
        }
    }

    void WorkbenchUi::build_composition_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 15,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-composition-body"},
        });
        panel_heading(ctx, font_id_, "PART SLOTS", "Composition",
                      "Enable, disable, hide or replace widget internals without forking interaction logic.");

        const auto toggle_row = [&](usize index, const char *label, const char *description,
                                    bool &value, const std::function<void()> &on_change = [] {}) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 12,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = panel,
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
            }
        };

        toggle_row(0, "Slider enabled", "Whole-control interaction gate", slider_enabled_);
        toggle_row(1, "Marker parts", "Hide ticks and generated labels", show_slider_markers_);
        toggle_row(2, "Custom thumb", "Replace geometry while retaining drag behavior", use_custom_thumb_);
        toggle_row(3, "Color picker enabled", "Disabled visuals keep layout stable", picker_enabled_);
        toggle_row(4, "Alpha part", "Remove the alpha strip independently", show_alpha_);
        toggle_row(5, "Preview part", "Remove preview without changing color state", show_preview_);

        section_label(ctx, font_id_, "OS WINDOW COMPOSITION");
        toggle_row(6, "Borderless fullscreen", "Toggle OS borderless fullscreen for this window",
                   window_borderless_fullscreen_, [&] {
                       engine.window_requests().set_fullscreen(
                           surface.handle.window_id,
                           window_borderless_fullscreen_ ? Platform::Windowing::WindowMode::BorderlessFullscreen
                                                          : Platform::Windowing::WindowMode::Windowed);
                   });
        toggle_row(7, "Window decorated", "Disable to remove the OS title bar/border",
                   window_decorated_, [&] {
                       engine.window_requests().set_decorated(surface.handle.window_id, window_decorated_);
                   });
        toggle_row(8, "Window transparent", "Live per-pixel transparency (dynamic on Windows; no-ops elsewhere)",
                   window_transparent_, [&] {
                       engine.window_requests().set_transparent(surface.handle.window_id, window_transparent_);
                   });

        section_label(ctx, font_id_, "WINDOW BLUR");
        if (supported_blur_kinds_.empty()) {
            draw_text(ctx, "No blur-capable window effect is supported on this OS build.",
                      text_style(font_id_, text_secondary, 11));
        } else {
            std::vector<UI::DropdownOption> blur_options;
            blur_options.reserve(supported_blur_kinds_.size());
            for (Platform::Windowing::WindowEffectKind kind : supported_blur_kinds_) {
                blur_options.push_back(dropdown_option(
                    font_id_, Platform::Windowing::window_effect_kind_name(kind).data(), accent));
            }

            UI::DropdownStyle blur_style{};
            blur_style.trigger = action_button_style();
            blur_style.list_background = panel_raised;
            blur_style.option_hovered = UI::Color{0.17, 0.20, 0.29, 1.0};
            blur_style.corner_radius = UI::CornerRadius::all(11.0f);
            blur_style.border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
            blur_style.option_padding = 10;
            blur_style.arrow_color = accent;
            blur_style.arrow_font_id = font_id_;

            // The dropdown only stages a choice locally; the "Blur enabled" switch below is what
            // actually commits it (fires set_blur()) — except while already enabled, in which case
            // changing the selection here re-applies immediately so switching type feels live rather
            // than requiring an off/on cycle to pick up the new kind.
            const usize previous_blur_index = selected_blur_kind_index_;
            const UI::DropdownResult blur_dropdown_result = UI::dropdown(
                ctx, UString{"workbench-blur-dropdown"},
                UI::ElementDecl{
                    .sizing = {UI::SizingAxis::fixed(220.0f), UI::SizingAxis::fixed(38.0f)},
                    .padding = UI::Padding::symmetric(12, 8),
                    .child_gap = 8,
                    .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                    .id = UString{"workbench-blur-dropdown"},
                },
                blur_style, blur_dropdown_state_, delta_seconds, selected_blur_kind_index_, blur_options, true);
            selected_blur_kind_index_ = blur_dropdown_result.selected_index;
            if (window_blur_enabled_ && selected_blur_kind_index_ != previous_blur_index) {
                engine.window_requests().set_blur(surface.handle.window_id,
                                                  supported_blur_kinds_[selected_blur_kind_index_], true);
            }

            toggle_row(9, "Blur enabled", "Applies the selected blur type above",
                       window_blur_enabled_, [&] {
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
        style.list_background = panel_raised;
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
            draw_text(part_ctx, "Disabled rows remain visible and stylable.",
                      text_style(font_id_, text_secondary, 9));
        };
        composition.option.visual.selected.background_color =
            UI::Color{accent.r, accent.g, accent.b, 0.18};
        composition.option.visual.disabled.background_color =
            UI::Color{0.060, 0.065, 0.080, 0.65};
        composition.tooltip.visible = true;
        composition.tooltip.build = [](UI::Context &part_ctx,
                                       const UI::DropdownPartContext &part) {
            auto tooltip = part_ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fit(), UI::SizingAxis::fit()},
                .padding = UI::Padding::symmetric(8, 5),
                .background_color = panel_raised,
                .corner_radius = UI::CornerRadius::all(7.0f),
            });
            draw_text(part_ctx,
                      part.option_index == 2 ? "This preset is intentionally disabled."
                                             : "Select this visual preset.",
                      text_style(font_id_, text_primary, 10));
        };

        const UI::DropdownResult dropdown_result = UI::dropdown(
            ctx, UString{"workbench-preset-dropdown"},
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(260.0f), UI::SizingAxis::fixed(38.0f)},
                .padding = UI::Padding::symmetric(12, 8),
                .child_gap = 8,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .id = UString{"workbench-preset-dropdown"},
            },
            style, preset_dropdown_state_, delta_seconds, selected_preset_, options, true,
            composition);
        selected_preset_ = dropdown_result.selected_index;

        const UI::ButtonResult reset = UI::button(
            ctx,
            UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(180.0f), UI::SizingAxis::fixed(36.0f)},
                .padding = UI::Padding::symmetric(12, 8),
                .child_alignment = {UI::AlignX::Center, UI::AlignY::Center},
                .id = UString{"workbench-reset"},
            },
            action_button_style(), reset_button_state_, delta_seconds);
        draw_text(ctx, "Reset composition", text_style(font_id_, text_primary, 12));
        if (reset.clicked) {
            slider_enabled_ = true;
            show_slider_markers_ = true;
            use_custom_thumb_ = true;
            picker_enabled_ = true;
            show_alpha_ = true;
            show_preview_ = true;
        }
    }

    void WorkbenchUi::build_text_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-text-body"},
        });
        panel_heading(ctx, font_id_, "TEXT INPUT", "Text Lab",
                      "One-line, masked, and multiline markdown editing on the shared TextEdit engine.");

        const UI::TextEditInput edit_input{
            .typed_text = surface.typed_text,
            .keys = {surface.edit_keys.begin(), surface.edit_keys.end()},
            .shift_held = surface.shift_down,
            .word_modifier_held = surface.ctrl_down,
        };

        UI::TextEditStyle edit_style{};
        edit_style.idle = panel;
        edit_style.hovered = panel_raised;
        edit_style.focused = UI::Color{0.050, 0.058, 0.088, 1.0};
        edit_style.border_idle = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)};
        edit_style.border_focused = UI::BorderStyle{.color = accent, .width = UI::BorderWidth::all(1)};
        edit_style.text_color = text_primary;
        edit_style.placeholder_color = text_secondary;
        edit_style.corner_radius = UI::CornerRadius::all(9.0f);
        edit_style.font_id = font_id_;
        edit_style.font_size = 13;

        section_label(ctx, font_id_, "SINGLE LINE");
        (void)UI::text_input(ctx,
                             UI::ElementDecl{
                                 .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(36.0f)},
                                 .padding = UI::Padding::symmetric(11, 8),
                                 .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                                 .id = UString{"workbench-text-single"},
                             },
                             edit_style, line_input_state_, edit_input, delta_seconds,
                             UString{"Type something..."});

        section_label(ctx, font_id_, "PASSWORD");
        UI::TextEditStyle password_style = edit_style;
        password_style.mask_characters = true;
        (void)UI::text_input(ctx,
                             UI::ElementDecl{
                                 .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(36.0f)},
                                 .padding = UI::Padding::symmetric(11, 8),
                                 .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                                 .id = UString{"workbench-text-password"},
                             },
                             password_style, password_input_state_, edit_input, delta_seconds,
                             UString{"Password"});

        section_label(ctx, font_id_, "MARKDOWN + LIVE PREVIEW");
        UI::TextEditStyle markdown_style = edit_style;
        // In-editor markdown coloring via the TextEdit engine's own Highlighter hook (cached —
        // reruns only when the buffer changes). Scalar-indexed on purpose; UString::substr is
        // scalar-based, so multi-byte characters can't skew the span ranges.
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
        (void)UI::text_area(ctx,
                            UI::ElementDecl{
                                .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fixed(150.0f)},
                                .padding = UI::Padding::all(10),
                                .id = UString{"workbench-text-markdown"},
                            },
                            markdown_style, markdown_input_state_, edit_input, delta_seconds,
                            UString{"# Write some markdown..."});

        {
            auto preview_card = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(360.0f), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(13),
                .child_gap = 5,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = panel_raised,
                .corner_radius = UI::CornerRadius::all(11.0f),
                .border = UI::BorderStyle{.color = outline, .width = UI::BorderWidth::all(1)},
            });

            // Line-based renderer: headings, bullets, and inline **bold** / `code` runs. Bold has
            // no second font face registered, so it renders as an emphasis color instead — same
            // tradeoff RichTextSpan itself documents for the editor side.
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

    void WorkbenchUi::build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-docking-body"},
        });
        panel_heading(ctx, font_id_, "WORKSPACE", "Docking Guide",
                      "The layout tree is live: tabs reorder, split targets preview, and dividers resize.");

        const auto instruction = [&](const char *number, const char *title, const char *copy,
                                     UI::Color color) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(11),
                .child_gap = 11,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = panel,
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

        instruction("01", "Reorder or merge tabs",
                    "Drag a tab across its strip or release it over another panel's center guide.", accent);
        instruction("02", "Create a split",
                    "Release over a left, right, top or bottom guide; drag the divider afterward.", accent_hot);
        instruction("03", "Tear off to an OS window",
                    "Drag beyond the workspace edge. The deferred coordinator transfers only after spawn succeeds.", success);

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
            draw_text(ctx, surface.primary ? "PRIMARY COORDINATOR" : "SECONDARY COORDINATOR",
                      text_style(font_id_, accent, 10));
            draw_text(ctx, status_message_, text_style(font_id_, text_primary, 11));
        }

        section_label(ctx, font_id_, "SCROLLING");

        const auto scroll_toggle_row = [&](usize index, const char *label, const char *description,
                                           bool &value) {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 12,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = panel,
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
                toggle_style(), toggle_states_[index], delta_seconds, value);
            if (result.clicked) {
                value = !value;
            }
        };

        scroll_toggle_row(6, "Click-and-drag scroll",
                          "Off by default — dragging this body's content no longer competes with widget drags.",
                          scroll_click_drag_);
        scroll_toggle_row(7, "Smooth wheel scrolling",
                          "Eases wheel deltas across frames instead of snapping the offset instantly.",
                          scroll_smooth_);

        {
            auto row = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(12),
                .child_gap = 10,
                .child_alignment = {UI::AlignX::Left, UI::AlignY::Center},
                .background_color = panel,
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
                rate_config, rate_style, scroll_smoothing_rate_state_, scroll_smoothing_rate_,
                UI::SliderInput{}, scroll_smooth_);
            scroll_smoothing_rate_ = rate_result.value;
        }

        ctx.set_scroll_settings(UI::ScrollSettings{
            .click_and_drag_scroll = scroll_click_drag_,
            .smooth_scrolling = scroll_smooth_,
            .smoothing_rate = static_cast<f32>(scroll_smoothing_rate_),
        });
    }

    void WorkbenchUi::build_metrics_panel(Surface &surface, UI::Context &ctx, f32 /*delta_seconds*/) {
        auto body = ctx.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fit()},
            .padding = UI::Padding::all(22),
            .child_gap = 13,
            .direction = UI::LayoutDirection::TopToBottom,
            .id = UString{"workbench-metrics-body"},
        });
        panel_heading(ctx, font_id_, "DIAGNOSTICS", "Performance",
                      "CPU and GPU render-graph pass timing, read back from Renderer::last_frame_timings() "
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
                draw_text(ctx, number_text((category + "  ").c_str(), ms, " ms"),
                          text_style(font_id_, text_secondary, 12));
            }
        };

        timing_section("GPU PASS TIMING", timings.gpu_pass_timings_ms);
        timing_section("CPU FRAME STAGES", timings.cpu_stage_timings_ms);
        timing_section("CPU PASS RECORDING", timings.cpu_pass_timings_ms);
    }

    void WorkbenchUi::handle_dock_events(Engine::Engine &engine, Surface &surface,
                                         UI::Docking::DockWorkspaceEvents events) {
        for (const UI::Docking::DockTearOffRequest &request : events.tear_off_requests) {
            // Before spawning a brand-new OS window, check whether the drag actually ended on top
            // of another *existing* managed window (dragging a tab back from a torn-off window
            // toward the primary one, or toward a third window) — if so, merge directly into it
            // instead. DockWorkspace itself only knows "this crossed my own bounds," never about
            // sibling windows (see its own top doc comment), so this cross-window geometry check has
            // to happen here, the one layer that actually has every managed window's screen rect
            // (Engine::WindowState).
            if (const Engine::WindowSnapshot *origin_window = engine.window_state().find(surface.handle.window_id)) {
                // build_frame() lays out UI::Context in *framebuffer* pixels (viewport comes from
                // Core::FrameInput::framebuffer_width/height), but WindowSnapshot::position/size are
                // *logical* screen coordinates — the same distinction Window.hpp itself draws between
                // size() and framebuffer_size(). On a HiDPI/fractional-scaling display those differ,
                // so the physical-pixel drop position has to be rescaled into logical space before
                // it can be compared against another window's logical screen rect.
                const glm::vec2 physical_local = kWorkspaceOrigin + request.workspace_local_drop_position;
                const glm::vec2 framebuffer_size{origin_window->framebuffer_size};
                const glm::vec2 logical_size{origin_window->size};
                const glm::vec2 physical_to_logical = framebuffer_size.x > 0.0f && framebuffer_size.y > 0.0f
                                                          ? logical_size / framebuffer_size
                                                          : glm::vec2{1.0f};
                const glm::vec2 global_drop = glm::vec2{origin_window->position} + physical_local * physical_to_logical;
                std::optional<Platform::Windowing::WindowId> redock_target;
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

            const Platform::Windowing::WindowConfig config{
                .title = "Sturdy UI — Detached Panel",
                .extent = {720, 620},
                .position = {0, 0},
                .use_default_position = true,
                .visible = true,
                .resizable = true,
                .decorated = true,
                .high_dpi = true,
                .mode = Platform::Windowing::WindowMode::Windowed,
                .graphics_api = Platform::Windowing::WindowGraphicsApi::Vulkan,
            };
            const Engine::WindowRequestId request_id = dock_coordinator_.request_tear_off(
                surface.handle.window_id, request, config, engine.window_requests());
            status_message_ = request_id
                                  ? "Creating a secondary UI surface; the source panel remains until acceptance."
                                  : "Tear-off was rejected before window creation; no panel data changed.";
        }
        dock_coordinator_.request_empty_window_closes(engine.window_requests());
    }

    Renderer::UiOverlayHooks WorkbenchUi::build_overlay_hooks(
        Engine::Engine &engine, Surface &surface,
        std::shared_ptr<UI::FrameSnapshot> snapshot) {
        Renderer::UiOverlayHooks hooks{};
        UI::UiRenderer *renderer = &surface.renderer;
        Renderer::Renderer *texture_resolver = engine.renderer();
        hooks.prepare = [renderer, snapshot, texture_resolver](
                            RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                            glm::vec2 viewport_size,
                            std::vector<RHI::BufferHandle> &transient_buffers,
                            Renderer::TextAtlasRetiredResources &retired_resources)
            -> Core::RendererResult {
            const Core::Extent2D extent = snapshot->viewport_extent();
            if (viewport_size.x != static_cast<f32>(extent.width) ||
                viewport_size.y != static_cast<f32>(extent.height)) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "UiWorkbench snapshot extent does not match its render surface.");
            }
            return renderer->prepare(device, encoder, *snapshot, texture_resolver,
                                     transient_buffers, retired_resources);
        };
        hooks.draw = [renderer](RHI::RenderPassEncoder &pass,
                                glm::vec2 viewport_size) -> Core::RendererResult {
            return renderer->draw(pass, viewport_size);
        };
        return hooks;
    }

    void WorkbenchUi::shutdown(Engine::Engine &engine) noexcept {
        if (RHI::RhiDevice *device = engine.rhi_device()) {
            for (auto &[window, surface] : surfaces_) {
                (void)window;
                surface->renderer.destroy(*device);
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
