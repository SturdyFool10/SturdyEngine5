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
            Surface &surface, glm::vec2 viewport, f32 delta_seconds);
        void build_controls_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_composition_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        void handle_dock_events(Engine::Engine &engine, Surface &surface,
                                UI::Docking::DockWorkspaceEvents events);
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(
            Engine::Engine &engine,
            Surface &surface,
            std::shared_ptr<UI::FrameSnapshot> snapshot);

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

        bool slider_enabled_ = true;
        bool show_slider_markers_ = true;
        bool use_custom_thumb_ = true;
        bool picker_enabled_ = true;
        bool show_alpha_ = true;
        bool show_preview_ = true;
        std::array<UI::ToggleState, 6> toggle_states_{};
        UI::ButtonState reset_button_state_{};

        std::string status_message_ = "Ready — drag any tab beyond a window edge to tear it off.";
    };

} // namespace SFT::UiWorkbench
