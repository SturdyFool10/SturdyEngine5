#include <CxxApi/Engine.hpp>

#include <CxxApi/Conversions.hpp>

namespace SFT::CxxApi {

    namespace {
        namespace E = SFT::Engine;
        namespace W = SFT::WindowManager;

        [[nodiscard]] W::KeyboardKey key(std::int32_t k) noexcept { return static_cast<W::KeyboardKey>(k); }
        [[nodiscard]] W::MouseButton button(std::uint8_t b) noexcept { return static_cast<W::MouseButton>(b); }
        [[nodiscard]] W::WindowId window_id(std::uint64_t id) noexcept { return static_cast<W::WindowId>(id); }

        // The Rust `Key` enum mirrors these numerically; catch drift at compile time.
        static_assert(static_cast<int>(W::KeyboardKey::Escape) == 0x100);
        static_assert(static_cast<int>(W::KeyboardKey::F1) == 0x200);
        static_assert(static_cast<int>(W::KeyboardKey::Numpad0) == 0x300);
        static_assert(static_cast<int>(W::KeyboardKey::VolumeUp) == 0x400);
    } // namespace

    double engine_delta_seconds(const E::Engine &e) noexcept { return e.frame_time().delta_seconds(); }
    double engine_unscaled_delta_seconds(const E::Engine &e) noexcept { return e.frame_time().unscaled_delta_seconds(); }
    std::uint64_t engine_tick_index(const E::Engine &e) noexcept { return e.frame_time().tick_index(); }
    double engine_time_scale(const E::Engine &e) noexcept { return e.time_scale().value(); }
    void engine_set_time_scale(E::Engine &e, double scale) noexcept { e.time_scale().set(scale); }

    bool engine_gpu_description(const E::Engine &e, GpuDescription &out) {
        const auto info = e.gpu_info();
        if (!info) {
            return false;
        }
        out.vendor_id = info->vendor_id;
        out.device_id = info->device_id;
        return true;
    }

    std::unique_ptr<std::string> engine_gpu_string(const E::Engine &e, GpuString which) {
        const auto info = e.gpu_info();
        if (!info) {
            return std::make_unique<std::string>();
        }
        switch (which) {
            case GpuString::Name: return owned_string(info->name);
            case GpuString::Vendor: return owned_string(info->vendor);
            case GpuString::DriverVersion: return owned_string(info->driver_version);
            case GpuString::ApiVersion: return owned_string(info->api_version);
            case GpuString::DeviceType: return owned_string(info->device_type);
        }
        return std::make_unique<std::string>();
    }

    RendererCapabilities engine_capabilities(const E::Engine &e) noexcept {
        const auto &c = e.capabilities();
        return RendererCapabilities{
            .multithreaded_command_recording = static_cast<bool>(c.multithreaded_command_recording),
            .async_compute = static_cast<bool>(c.async_compute),
            .raytracing = static_cast<bool>(c.raytracing),
            .mesh_shaders = static_cast<bool>(c.mesh_shaders),
            .bindless = static_cast<bool>(c.bindless),
            .timeline_semaphores = static_cast<bool>(c.timeline_semaphores),
            .max_frames_in_flight = c.max_frames_in_flight,
        };
    }

    void engine_wait_idle(E::Engine &e) noexcept { e.wait_idle(); }

    bool input_key_down(const E::Engine &e, std::int32_t k) noexcept { return e.input_state().key_down(key(k)); }
    bool input_key_just_pressed(const E::Engine &e, std::int32_t k) noexcept { return e.input_state().key_just_pressed(key(k)); }
    bool input_key_just_released(const E::Engine &e, std::int32_t k) noexcept { return e.input_state().key_just_released(key(k)); }
    bool input_mouse_down(const E::Engine &e, std::uint8_t b) noexcept { return e.input_state().mouse_down(button(b)); }
    bool input_mouse_just_pressed(const E::Engine &e, std::uint8_t b) noexcept { return e.input_state().mouse_just_pressed(button(b)); }
    bool input_mouse_just_released(const E::Engine &e, std::uint8_t b) noexcept { return e.input_state().mouse_just_released(button(b)); }
    std::array<float, 2> input_mouse_position(const E::Engine &e) noexcept { return {e.input_state().mouse_x(), e.input_state().mouse_y()}; }
    std::array<float, 2> input_mouse_delta(const E::Engine &e) noexcept {
        return {e.input_state().mouse_delta_x(), e.input_state().mouse_delta_y()};
    }
    std::array<float, 2> input_wheel_delta(const E::Engine &e) noexcept {
        return {e.input_state().wheel_delta_x(), e.input_state().wheel_delta_y()};
    }
    std::unique_ptr<std::string> input_text_this_tick(const E::Engine &e) { return owned_string(e.input_state().text_this_tick()); }
    std::unique_ptr<std::string> input_composition_text(const E::Engine &e) { return owned_string(e.input_state().composition_text()); }
    bool input_composing(const E::Engine &e) noexcept { return e.input_state().composing(); }
    std::uint32_t input_modifiers(const E::Engine &e) noexcept { return static_cast<std::uint32_t>(e.input_state().modifiers()); }

    void input_inject_key(E::Engine &e, std::uint64_t window, std::int32_t k, bool pressed, std::uint32_t modifiers, bool repeat) {
        e.inject_key_event(window_id(window), key(k), pressed, modifiers, repeat);
    }
    void input_inject_text(E::Engine &e, std::uint64_t window, const std::string &utf8) { e.inject_text_event(window_id(window), utf8); }
    void input_inject_mouse_move(E::Engine &e, std::uint64_t window, std::array<float, 2> p, std::array<float, 2> d, std::uint32_t buttons) {
        e.inject_mouse_move(window_id(window), p[0], p[1], d[0], d[1], buttons);
    }
    void input_inject_mouse_button(E::Engine &e, std::uint64_t window, std::uint8_t b, bool pressed, std::array<float, 2> p,
                                   std::uint8_t clicks) {
        e.inject_mouse_button(window_id(window), button(b), pressed, p[0], p[1], clicks);
    }
    void input_inject_mouse_wheel(E::Engine &e, std::uint64_t window, std::array<float, 2> d, std::array<float, 2> p) {
        e.inject_mouse_wheel(window_id(window), d[0], d[1], p[0], p[1]);
    }

} // namespace SFT::CxxApi
