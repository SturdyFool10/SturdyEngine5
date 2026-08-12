#pragma once

#include <Engine/Engine.hpp>
#include <Text/Text.hpp>
#include <UI/UI.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFT::UiWorkbench {

    class WorkbenchUi {
      public:
        WorkbenchUi();
        ~WorkbenchUi();

        WorkbenchUi(const WorkbenchUi &) = delete;
        WorkbenchUi &operator=(const WorkbenchUi &) = delete;

        [[nodiscard]] Engine::GameLogicResult initialize(Engine::Engine &engine);
        [[nodiscard]] std::optional<Engine::RenderFrameParameters> render(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle handle,
            const Core::FrameInput &frame);
        void shutdown(Engine::Engine &engine) noexcept;

      private:
        struct Surface;

        [[nodiscard]] Surface *find_surface(Platform::Windowing::WindowId window) noexcept;
        [[nodiscard]] Surface *ensure_surface(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle handle,
            bool primary);
        void process_window_completions(Engine::Engine &engine);
        void route_input(Engine::Engine &engine);
        [[nodiscard]] UI::Docking::DockWorkspaceEvents build_frame(
            Engine::Engine &engine, Surface &surface, glm::vec2 viewport, f32 delta_seconds);
        void build_controls_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_composition_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_text_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_metrics_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void handle_dock_events(Engine::Engine &engine, Surface &surface,
                                UI::Docking::DockWorkspaceEvents events);
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(
            Engine::Engine &engine,
            Surface &surface,
            std::shared_ptr<UI::FrameSnapshot> snapshot);
        [[nodiscard]] f64 effective_background_opacity() const noexcept {
            return swapchain_transparent_ ? ui_background_opacity_ : 1.0;
        }

        static constexpr UI::FontId font_id_ = 1;

        Text::Font font_{};
        Engine::RenderGraph render_graph_{};
        Engine::DockWindowCoordinator dock_coordinator_{};
        std::unordered_map<Platform::Windowing::WindowId, std::unique_ptr<Surface>> surfaces_;
        std::optional<Platform::Windowing::WindowId> primary_window_;

        UI::SliderState exposure_slider_state_{};
        UI::SliderState rotation_slider_state_{};
        f64 exposure_ = 62.0;
        f64 rotation_ = 35.0;

        UI::ColorPickerState color_picker_state_{};
        UI::Color selected_color_{0.28, 0.48, 0.98, 0.86};
        UI::ColorPickerValue selected_color_value_{Foundation::Color::Srgb{0.28, 0.48, 0.98, 0.86}};
        UI::ColorPickerColorSpace selected_color_space_ = UI::ColorPickerColorSpace::Srgb;

        UI::DropdownState preset_dropdown_state_{};
        usize selected_preset_ = 0;

        // Text Lab: plain single-line, masked (password) single-line, and a multiline Markdown
        // editor whose content renders live in a preview card below it. Buffers live here (like
        // every other widget's persistent state), seeded once in initialize().
        UI::TextEditState line_input_state_{};
        UI::TextEditState password_input_state_{};
        UI::TextEditState markdown_input_state_{};
        // The text area's horizontal/vertical scrollbar fade and drag state belongs to this
        // long-lived editor, not to the transient frame-local UI build.
        UI::ScrollAreaState markdown_input_scroll_state_{};

        bool slider_enabled_ = true;
        bool show_slider_markers_ = true;
        bool use_custom_thumb_ = true;
        bool picker_enabled_ = true;
        bool show_alpha_ = true;
        bool show_preview_ = true;
        std::array<UI::ToggleState, 12> toggle_states_{};
        UI::ButtonState reset_button_state_{};

        // Presentation/window-composition controls in the Composition panel. The native window is
        // created alpha-capable; swapchain_transparent_ controls whether its swapchain actually
        // preserves compositor-visible alpha, while ui_background_opacity_ controls the backgrounds
        // drawn into that alpha channel.
        bool hdr_enabled_ = false;
        bool swapchain_transparent_ = false;
        f64 ui_background_opacity_ = 0.78;
        UI::SliderState ui_background_opacity_slider_state_{};
        // HDR presentation calibration. Reference white is a scale over the compositor-provided value;
        // peak shapes HDR output, while MaxCLL/MaxFALL are live Vulkan HDR10 metadata values.
        f64 hdr_reference_white_scale_ = 1.0;
        f64 hdr_peak_luminance_nits_ = 1000.0;
        f64 hdr_max_content_light_level_nits_ = 1000.0;
        f64 hdr_max_frame_average_light_level_nits_ = 400.0;
        UI::SliderState hdr_reference_white_scale_state_{};
        UI::SliderState hdr_peak_luminance_state_{};
        UI::SliderState hdr_max_content_light_level_state_{};
        UI::SliderState hdr_max_frame_average_light_level_state_{};
        bool window_borderless_fullscreen_ = false;
        bool window_decorated_ = true;
        bool window_blur_enabled_ = false;
        // Which entry of supported_blur_kinds_ is picked in the blur-type dropdown; only actually
        // applied to the window once window_blur_enabled_ is on (or changed while already on).
        usize selected_blur_kind_index_ = 0;
        UI::DropdownState blur_dropdown_state_{};
        // Populated once in initialize() by filtering the candidate blur-family kinds through
        // Platform::Windowing::operating_system_may_support_window_effect() — only what the running
        // OS actually reports as supported ends up in the dropdown.
        std::vector<Platform::Windowing::WindowEffectKind> supported_blur_kinds_;

        // Applied to each Surface's UI::Context every frame (Context::set_scroll_settings()) —
        // demonstrates the scroll system's own defaults (drag-scroll off, wheel smoothing on) plus
        // lets a user experiment with them live against the docking body's real scroll container.
        bool scroll_click_drag_ = false;
        bool scroll_smooth_ = true;
        f64 scroll_smoothing_rate_ = 18.0;
        UI::SliderState scroll_smoothing_rate_state_{};

        // Egui-defaults-flavored: thin, invisible track, translucent thumb that only appears while
        // the pointer is in a scrollable panel (or actively dragging/scrolling it) and fades back
        // out shortly after — see ScrollbarStyle's own doc comment (ScrollArea.hpp) for the exact
        // timings. Shared by every panel; per-panel feel (fade/drag progress) lives on each Surface
        // instead (Surface::controls_scroll etc.).
        UI::ScrollbarStyle scrollbar_style_{};

        std::string status_message_ = "Ready — drag any tab beyond a window edge to tear it off.";
    };

} // namespace SFT::UiWorkbench
