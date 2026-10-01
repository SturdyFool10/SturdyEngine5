#include <Engine/ScreenUi.hpp>

#include <Engine/EngineModule.hpp>

namespace SFT::Engine {

    void apply_window_event(UI::UiInput &input, const MouseMoveEvent &event, glm::vec2 pixel_density) noexcept {
        // Mouse events arrive in window coordinates; UiInput works in framebuffer pixels.
        input.move_pointer(glm::vec2{event.mouse.x, event.mouse.y} * pixel_density);
    }

    void apply_window_event(UI::UiInput &input, const MouseButtonEvent &event, glm::vec2 pixel_density) noexcept {
        if (event.mouse.button_code != WindowManager::MouseButton::Left) {
            return;
        }
        input.move_pointer(glm::vec2{event.mouse.x, event.mouse.y} * pixel_density);
        input.set_pointer_down(event.action == ButtonAction::Pressed);
    }

    void apply_window_event(UI::UiInput &input, const MouseWheelEvent &event) noexcept {
        input.add_scroll({-event.wheel.x, event.wheel.y});
    }

    void apply_window_event(UI::UiInput &input, const TextInputEvent &event) { input.add_text(event.text.utf8); }

    void apply_window_event(UI::UiInput &input, const TextEditingEvent &event) { input.set_composition(event.text.utf8); }

    void apply_window_event(UI::UiInput &input, const KeyboardEvent &event) {
        // Modifiers are tracked as held state and are not edits themselves.
        if (event.key_code == KeyboardKey::LeftShift || event.key_code == KeyboardKey::RightShift) {
            input.set_shift_held(event.pressed());
            return;
        }
        if (event.key_code == KeyboardKey::LeftControl || event.key_code == KeyboardKey::RightControl) {
            input.set_word_modifier_held(event.pressed());
            return;
        }
        if (!event.pressed()) {
            return;
        }
        if (input.word_modifier_held()) {
            switch (event.key_code) {
                case KeyboardKey::A: input.press_key(UI::EditKey::SelectAll); break;
                case KeyboardKey::C: input.press_key(UI::EditKey::Copy); break;
                case KeyboardKey::X: input.press_key(UI::EditKey::Cut); break;
                case KeyboardKey::V: input.press_key(UI::EditKey::Paste); break;
                default: break;
            }
        }
        switch (event.key_code) {
            case KeyboardKey::Left: input.press_key(UI::EditKey::Left); break;
            case KeyboardKey::Right: input.press_key(UI::EditKey::Right); break;
            case KeyboardKey::Up: input.press_key(UI::EditKey::Up); break;
            case KeyboardKey::Down: input.press_key(UI::EditKey::Down); break;
            case KeyboardKey::Home: input.press_key(UI::EditKey::Home); break;
            case KeyboardKey::End: input.press_key(UI::EditKey::End); break;
            case KeyboardKey::Backspace: input.press_key(UI::EditKey::Backspace); break;
            case KeyboardKey::Delete: input.press_key(UI::EditKey::Delete); break;
            case KeyboardKey::Enter: input.press_key(UI::EditKey::Enter); break;
            case KeyboardKey::Escape: input.press_key(UI::EditKey::Escape); break;
            default: break;
        }
    }

    ScreenUi::ScreenUi(Engine &engine) : engine_(engine) {
        systems_.push_back(engine.update_schedule().add_system(
            [this](Ecs::EventReader<MouseMoveEvent> mouse_move, Ecs::EventReader<MouseButtonEvent> mouse_button,
                   Ecs::EventReader<MouseWheelEvent> mouse_wheel) noexcept {
                const auto wanted = [this](WindowManager::WindowId window) { return !window_ || *window_ == window; };
                for (const MouseMoveEvent &event : mouse_move.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event, pixel_density_for(event.window));
                }
                for (const MouseButtonEvent &event : mouse_button.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event, pixel_density_for(event.window));
                }
                for (const MouseWheelEvent &event : mouse_wheel.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event);
                }
            }));
        systems_.push_back(engine.update_schedule().add_system(
            [this](Ecs::EventReader<KeyboardEvent> keyboard, Ecs::EventReader<TextInputEvent> text,
                   Ecs::EventReader<TextEditingEvent> text_editing) noexcept {
                const auto wanted = [this](WindowManager::WindowId window) { return !window_ || *window_ == window; };
                for (const KeyboardEvent &event : keyboard.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event);
                }
                for (const TextInputEvent &event : text.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event);
                }
                for (const TextEditingEvent &event : text_editing.read()) {
                    if (wanted(event.window)) apply_window_event(surface_.input(), event);
                }
            }));
    }

    ScreenUi::~ScreenUi() {
        for (const Ecs::SystemHandle system : systems_) {
            (void)engine_.update_schedule().remove_system(system);
        }
        if (RHI::RhiDevice *device = engine_.rhi_device()) {
            surface_.destroy(*device);
        }
    }

    bool ScreenUi::ensure_ready(RHI::Format color_format) {
        RHI::RhiDevice *device = engine_.rhi_device();
        return device != nullptr && surface_.ensure_ready(*device, color_format);
    }

    const WindowSnapshot *ScreenUi::tracked_window() const noexcept {
        const WindowState &windows = engine_.window_state();
        return window_ ? windows.find(*window_) : windows.primary();
    }

    glm::vec2 ScreenUi::pixel_density_for(WindowManager::WindowId window) const noexcept {
        const WindowSnapshot *snapshot = engine_.window_state().find(window);
        return snapshot != nullptr ? snapshot->pixel_density() : glm::vec2{1.0f};
    }

    UI::Context &ScreenUi::begin_frame(glm::vec2 viewport, f32 delta_seconds) {
        if (const WindowSnapshot *window = tracked_window()) {
            surface_.set_content_scale(window->content_scale);
        }
        return surface_.begin_frame(viewport, delta_seconds);
    }

    Renderer::OverlayPass ScreenUi::finish_overlay(UI::UiOverlayOptions options) {
        return surface_.finish_overlay(engine_.renderer(), std::move(options));
    }

    void ScreenUi::release_gpu_objects() noexcept {
        if (RHI::RhiDevice *device = engine_.rhi_device()) {
            surface_.release_renderer(*device);
        }
    }

} // namespace SFT::Engine
