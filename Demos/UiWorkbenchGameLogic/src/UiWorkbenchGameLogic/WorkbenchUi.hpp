#pragma once

#include <Engine/Engine.hpp>
#include <Text/Text.hpp>
#include <UI/UI.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        void destroy_surface_renderers(RHI::RhiDevice &device) noexcept;
        [[nodiscard]] Core::RendererResult recreate_surface_renderers(RHI::RhiDevice &device);
        void process_window_completions(Engine::Engine &engine);
        void route_input(Engine::Engine &engine);
        [[nodiscard]] UI::Docking::DockWorkspaceEvents build_frame(
            Engine::Engine &engine,
            Surface &surface,
            glm::vec2 viewport,
            f32 delta_seconds);
        void build_controls_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_composition_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_text_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_metrics_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void handle_dock_events(Engine::Engine &engine, Surface &surface, UI::Docking::DockWorkspaceEvents events);
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(
            Engine::Engine &engine,
            Surface &surface,
            std::shared_ptr<UI::FrameSnapshot> snapshot);
        [[nodiscard]] f64 effective_background_opacity() const noexcept;

        static constexpr UI::FontId font_id_ = 1;

        Text::Font font_{};
        /// Fallback for glyphs font_ (a Latin/symbols/Nerd-Font-icons face) doesn't cover — see
        /// register_font()'s own call site for how this is wired in, and Fonts/NOTO_CJK_LICENSE.txt
        /// for provenance (subsetted from Google's Noto Sans Mono CJK JP, OFL-1.1).
        Text::Font cjk_font_{};
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

        UI::DropdownState preset_dropdown_state_;
        usize selected_preset_ = 0;
        UI::DropdownState graphics_adapter_dropdown_state_;
        UI::DropdownState graphics_api_dropdown_state_;
        UI::ButtonState reconstruct_graphics_button_state_;
        usize selected_graphics_adapter_index_ = 0;
        usize selected_graphics_api_index_ = 0;

        /// Window type: which windowing library owns the primary OS window (index 0 = SDL3,
        /// Platform's built-in default; 1 = GlfwWindowProvider). Reflects the user's last selection
        /// here, same "not continuously re-derived from the live window" convention the two dropdowns
        /// above already follow — see build_composition_panel()'s own doc comment on this dropdown.
        UI::DropdownState window_type_dropdown_state_;
        UI::ButtonState recreate_window_button_state_;
        usize selected_window_type_index_ = 0;

        /// Text Lab: plain single-line, masked (password) single-line, and a multiline Markdown
        /// editor whose content renders live in a preview card below it. Buffers live here (like
        /// every other widget's persistent state), seeded once in initialize().
        UI::TextEditState line_input_state_{};
        UI::TextEditState password_input_state_{};
        UI::TextEditState markdown_input_state_{};
        /// The text area's horizontal/vertical scrollbar fade and drag state belongs to this
        /// long-lived editor, not to the transient frame-local UI build.
        UI::ScrollAreaState markdown_input_scroll_state_{};

        bool slider_enabled_ = true;
        bool show_slider_markers_ = true;
        bool use_custom_thumb_ = true;
        bool picker_enabled_ = true;
        bool show_alpha_ = true;
        bool show_preview_ = true;
        std::array<UI::ToggleState, 12> toggle_states_{};
        UI::ButtonState reset_button_state_{};

        /// Presentation/window-composition controls in the Composition panel. The native window is
        /// created alpha-capable; swapchain_transparent_ controls whether its swapchain actually
        /// preserves compositor-visible alpha, while ui_background_opacity_ controls the backgrounds
        /// drawn into that alpha channel.
        bool hdr_enabled_ = false;
        bool swapchain_transparent_ = false;
        /// Which windows currently have the legacy Win32 WS_EX_LAYERED/DwmExtendFrameIntoClientArea
        /// transparency effect applied (WindowEffectKind::Transparent). Tracked per window, and
        /// reconciled every frame in build_composition_panel rather than once at toggle time, because
        /// whether a given window's swapchain actually needs that legacy effect isn't known until its
        /// RHI swapchain has been (re)built — apply_presentation_config only marks it dirty for the
        /// renderer to rebuild on an upcoming frame, it doesn't rebuild synchronously. A window whose
        /// swapchain ends up using composition present (RHI::PresentationResolution::
        /// via_composition_present) must NOT also get this legacy effect: DirectComposition already
        /// owns that window's transparent compositing end to end, and layering the legacy mechanism on
        /// top leaves its now-otherwise-unused redirection surface's last frame visible as a frozen
        /// second layer underneath the live composition-present content.
        std::unordered_set<Platform::Windowing::WindowId> legacy_window_transparency_applied_{};
        f64 ui_background_opacity_ = 0.78;
        UI::SliderState ui_background_opacity_slider_state_{};
        /// HDR presentation calibration. Reference white is a scale over the compositor-provided value;
        /// peak shapes HDR output, while MaxCLL/MaxFALL are live Vulkan HDR10 metadata values.
        f64 hdr_reference_white_scale_ = 1.0;
        f64 hdr_peak_luminance_nits_ = 1000.0;
        f64 hdr_max_content_light_level_nits_ = 1000.0;
        f64 hdr_max_frame_average_light_level_nits_ = 400.0;
        UI::SliderState hdr_reference_white_scale_state_{};
        UI::SliderState hdr_peak_luminance_state_{};
        UI::SliderState hdr_max_content_light_level_state_{};
        UI::SliderState hdr_max_frame_average_light_level_state_{};
        /// Index into the fullscreen-mode dropdown's three options, in Platform::Windowing::WindowMode's
        /// own declaration order (0=Windowed/"Off", 1=BorderlessFullscreen, 2=ExclusiveFullscreen) so
        /// the index can be used as the enum's underlying value directly rather than needing a lookup
        /// table. Exclusive is a *request*, not a guarantee — RHI::PresentationResolution::
        /// full_screen_exclusive_active reports whether the backend actually got it (the window still
        /// goes fullscreen either way; only the presentation path underneath it differs).
        usize selected_fullscreen_mode_index_ = 0;
        UI::DropdownState fullscreen_mode_dropdown_state_{};
        bool window_decorated_ = true;
        bool window_blur_enabled_ = false;
        /// Which entry of supported_blur_kinds_ is picked in the blur-type dropdown; only actually
        /// applied to the window once window_blur_enabled_ is on (or changed while already on).
        usize selected_blur_kind_index_ = 0;
        UI::DropdownState blur_dropdown_state_{};
        /// Populated once in initialize() by filtering the candidate blur-family kinds through
        /// Platform::Windowing::operating_system_may_support_window_effect() — only what the running
        /// OS actually reports as supported ends up in the dropdown.
        std::vector<Platform::Windowing::WindowEffectKind> supported_blur_kinds_;

        /// Applied to each Surface's UI::Context every frame (Context::set_scroll_settings()) —
        /// demonstrates the scroll system's own defaults (drag-scroll off, wheel smoothing on) plus
        /// lets a user experiment with them live against the docking body's real scroll container.
        bool scroll_click_drag_ = false;
        bool scroll_smooth_ = true;
        f64 scroll_smoothing_rate_ = 18.0;
        UI::SliderState scroll_smoothing_rate_state_{};

        /// Egui-defaults-flavored: thin, invisible track, translucent thumb that only appears while
        /// the pointer is in a scrollable panel (or actively dragging/scrolling it) and fades back
        /// out shortly after — see ScrollbarStyle's own doc comment (ScrollArea.hpp) for the exact
        /// timings. Shared by every panel; per-panel feel (fade/drag progress) lives on each Surface
        /// instead (Surface::controls_scroll etc.).
        UI::ScrollbarStyle scrollbar_style_{};

        /// The frame that applies a backend reconstruction was assembled against the old device. Drop
        /// it after the UI transaction closes; the next frame rebuilds all renderer resources first.
        /// Two frames are dropped: the frame initiating reconstruction and one already queued by the
        /// dedicated per-window render thread against the old UI renderer generation.
        u32 frames_to_skip_after_graphics_reconstruction_ = 0;
        /// A pipeline-creation error is deterministic for the current backend. Avoid recompiling and
        /// logging it once per managed-window frame; a later reconstruction explicitly clears this.
        bool ui_renderer_rebuild_failed_ = false;
        std::string status_message_ = "Ready — drag any tab beyond a window edge to tear it off.";
    };

} // namespace SFT::UiWorkbench
