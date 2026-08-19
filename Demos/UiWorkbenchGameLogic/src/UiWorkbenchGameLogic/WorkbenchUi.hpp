#pragma once

#include <Engine/Engine.hpp>
#include <Renderer/Text/Text.hpp>
#include <Renderer/UI/UI.hpp>

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
        /// Constructs a `WorkbenchUi` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        WorkbenchUi();
        /// Destroys the `WorkbenchUi` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~WorkbenchUi();

        /// Disables this construction form for `WorkbenchUi`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WorkbenchUi(const WorkbenchUi &) = delete;
        /// Assigns a new value to this `WorkbenchUi`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        WorkbenchUi &operator=(const WorkbenchUi &) = delete;

        /// Initializes the `WorkbenchUi` for use.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Engine::GameLogicResult initialize(Engine::Engine &engine);
        /// Renders the requested content using the current rendering state.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param handle Handle identifying the target object or resource.
        /// @param frame `frame` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] std::optional<Engine::RenderFrameParameters> render(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle handle,
            const Core::FrameInput &frame);
        /// Shuts down the `WorkbenchUi` and releases associated runtime state.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void shutdown(Engine::Engine &engine) noexcept;

      private:
        struct Surface;

        /// Finds surface in the available state.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Surface *find_surface(WindowManager::WindowId window) noexcept;
        /// Finds or creates the surface required by the operation.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param handle Handle identifying the target object or resource.
        /// @param primary `primary` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Surface *ensure_surface(
            Engine::Engine &engine,
            Core::RenderSurfaceHandle handle,
            bool primary);
        /// Destroys the surface renderers identified by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_surface_renderers(RHI::RhiDevice &device) noexcept;
        /// Recreates surface renderers using the supplied arguments and current state.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult recreate_surface_renderers(RHI::RhiDevice &device);
        /// Performs the process window completions operation for `WorkbenchUi` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void process_window_completions(Engine::Engine &engine);
        /// Performs the route input operation for `WorkbenchUi` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void route_input(Engine::Engine &engine);
        /// Builds frame.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param viewport `viewport` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UI::Docking::DockWorkspaceEvents build_frame(
            Engine::Engine &engine,
            Surface &surface,
            glm::vec2 viewport,
            f32 delta_seconds);
        /// Builds controls panel.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_controls_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Builds color panel.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_color_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Builds settings panel.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_settings_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Builds text panel.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_text_panel(Engine::Engine &engine, Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Builds docking panel.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_docking_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Builds metrics panel.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param ctx `ctx` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void build_metrics_panel(Surface &surface, UI::Context &ctx, f32 delta_seconds);
        /// Performs the handle dock events operation for `WorkbenchUi` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param events Event used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void handle_dock_events(Engine::Engine &engine, Surface &surface, UI::Docking::DockWorkspaceEvents events);
        /// Builds overlay hooks.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param snapshot `snapshot` value used by the operation.
        ///
        /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Renderer::UiOverlayHooks build_overlay_hooks(
            Engine::Engine &engine,
            Surface &surface,
            std::shared_ptr<UI::FrameSnapshot> snapshot);
        /// Returns the current or globally available effective background opacity value.
        ///
        /// @return Returns the current effective background opacity value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f64 effective_background_opacity() const noexcept;

        static constexpr UI::FontId font_id_ = 1;

        Text::Font font_{};


        Text::Font cjk_font_{};
        Engine::RenderGraph render_graph_{};
        Engine::DockWindowCoordinator dock_coordinator_{};
        std::unordered_map<WindowManager::WindowId, std::unique_ptr<Surface>> surfaces_;
        std::optional<WindowManager::WindowId> primary_window_;

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


        UI::DropdownState window_type_dropdown_state_;
        UI::ButtonState recreate_window_button_state_;
        usize selected_window_type_index_ = 0;


        UI::TextEditState line_input_state_{};
        UI::TextEditState password_input_state_{};
        UI::TextEditState markdown_input_state_{};


        UI::ScrollAreaState markdown_input_scroll_state_{};

        bool slider_enabled_ = true;
        bool show_slider_markers_ = true;
        bool use_custom_thumb_ = true;
        bool picker_enabled_ = true;
        bool show_alpha_ = true;
        bool show_preview_ = true;
        std::array<UI::ToggleState, 12> toggle_states_{};
        UI::ButtonState reset_button_state_{};


        bool hdr_enabled_ = false;
        bool swapchain_transparent_ = false;


        std::unordered_set<WindowManager::WindowId> legacy_window_transparency_applied_{};
        f64 ui_background_opacity_ = 0.78;
        UI::SliderState ui_background_opacity_slider_state_{};


        f64 hdr_reference_white_scale_ = 1.0;
        f64 hdr_peak_luminance_nits_ = 1000.0;
        f64 hdr_max_content_light_level_nits_ = 1000.0;
        f64 hdr_max_frame_average_light_level_nits_ = 400.0;
        UI::SliderState hdr_reference_white_scale_state_{};
        UI::SliderState hdr_peak_luminance_state_{};
        UI::SliderState hdr_max_content_light_level_state_{};
        UI::SliderState hdr_max_frame_average_light_level_state_{};


        usize selected_fullscreen_mode_index_ = 0;
        UI::DropdownState fullscreen_mode_dropdown_state_{};
        bool window_decorated_ = true;
        bool window_blur_enabled_ = false;


        usize selected_blur_kind_index_ = 0;
        UI::DropdownState blur_dropdown_state_{};


        std::vector<WindowManager::WindowEffectKind> supported_blur_kinds_;


        bool scroll_click_drag_ = false;
        bool scroll_smooth_ = true;
        f64 scroll_smoothing_rate_ = 18.0;
        UI::SliderState scroll_smoothing_rate_state_{};


        UI::ScrollbarStyle scrollbar_style_{};


        u32 frames_to_skip_after_graphics_reconstruction_ = 0;


        bool ui_renderer_rebuild_failed_ = false;
        std::string status_message_ = "Ready — drag any tab beyond a window edge to tear it off.";
    };

} // namespace SFT::UiWorkbench
