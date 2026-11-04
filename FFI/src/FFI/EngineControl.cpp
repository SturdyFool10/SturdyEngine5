/// C ABI for the engine-level controls that were only reachable from Rust and C++: frame mode and redraw requests, synthetic
/// input, drag-and-drop events, render health, and enumerating and switching the graphics device.
///
/// Everything forwards to `Engine` (`EngineModule.hpp`, `DeviceSwitch.hpp`). The two drained event queues (drops and device
/// changes) are popped one at a time, so the ABI keeps a small per-engine backlog of what `Engine` handed over in a batch.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace E = SFT::Engine;
    namespace R = SFT::RHI;

    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    /// Events the engine handed over in a batch but the caller has not popped yet.
    struct Backlog {
        std::deque<E::Engine::DropEvent> drops;
        std::deque<E::DeviceChangeEvent> device_changes;
    };

    std::mutex g_backlog_mutex;
    std::map<E::Engine *, Backlog> g_backlogs;

    /// Copies `text` into a fixed field, truncating (never past the terminator) so the field is always a C string.
    template <std::size_t N>
    void copy_text(char (&field)[N], std::string_view text) noexcept {
        const std::size_t length = std::min(text.size(), N - 1);
        std::memcpy(field, text.data(), length);
        field[length] = '\0';
    }

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept {
        return set_error(STURDY_ERROR_INVALID_ARGUMENT, what);
    }

    [[nodiscard]] SFT::WindowManager::WindowId window_of(SturdySurface surface) noexcept {
        return static_cast<SFT::WindowManager::WindowId>(surface.id);
    }

    [[nodiscard]] bool translate_backend(R::BackendType backend, SturdyBackend *out) noexcept {
        switch (backend) {
        case R::BackendType::Vulkan:
            *out = STURDY_BACKEND_VULKAN;
            return true;
        case R::BackendType::D3D12:
            *out = STURDY_BACKEND_D3D12;
            return true;
        case R::BackendType::Metal:
            *out = STURDY_BACKEND_METAL;
            return true;
        case R::BackendType::WebGpu:
            *out = STURDY_BACKEND_WEBGPU;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool translate_backend(SturdyBackend backend, std::optional<R::BackendType> *out) noexcept {
        switch (backend) {
        case STURDY_BACKEND_DEFAULT:
            *out = std::nullopt;
            return true;
        case STURDY_BACKEND_VULKAN:
            *out = R::BackendType::Vulkan;
            return true;
        case STURDY_BACKEND_D3D12:
            *out = R::BackendType::D3D12;
            return true;
        case STURDY_BACKEND_METAL:
            *out = R::BackendType::Metal;
            return true;
        case STURDY_BACKEND_WEBGPU:
            *out = R::BackendType::WebGpu;
            return true;
        case STURDY_BACKEND_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] bool translate_mouse_button(SturdyMouseButton button, SFT::WindowManager::MouseButton *out) noexcept {
        using B = SFT::WindowManager::MouseButton;
        switch (button) {
        case STURDY_MOUSE_BUTTON_UNKNOWN:
            *out = B::Unknown;
            return true;
        case STURDY_MOUSE_BUTTON_LEFT:
            *out = B::Left;
            return true;
        case STURDY_MOUSE_BUTTON_MIDDLE:
            *out = B::Middle;
            return true;
        case STURDY_MOUSE_BUTTON_RIGHT:
            *out = B::Right;
            return true;
        case STURDY_MOUSE_BUTTON_EXTRA1:
            *out = B::Extra1;
            return true;
        case STURDY_MOUSE_BUTTON_EXTRA2:
            *out = B::Extra2;
            return true;
        case STURDY_MOUSE_BUTTON_FORCE_U32:
        default:
            return false;
        }
    }

    [[nodiscard]] SturdyDeviceType translate_device_type(R::DeviceType type) noexcept {
        switch (type) {
        case R::DeviceType::IntegratedGpu:
            return STURDY_DEVICE_TYPE_INTEGRATED_GPU;
        case R::DeviceType::DiscreteGpu:
            return STURDY_DEVICE_TYPE_DISCRETE_GPU;
        case R::DeviceType::VirtualGpu:
            return STURDY_DEVICE_TYPE_VIRTUAL_GPU;
        case R::DeviceType::Cpu:
            return STURDY_DEVICE_TYPE_CPU;
        case R::DeviceType::Other:
        default:
            return STURDY_DEVICE_TYPE_OTHER;
        }
    }

    [[nodiscard]] SturdyAdapterPresentable translate_presentable(E::AdapterPresentability presentable) noexcept {
        switch (presentable) {
        case E::AdapterPresentability::Yes:
            return STURDY_ADAPTER_PRESENTABLE_YES;
        case E::AdapterPresentability::No:
            return STURDY_ADAPTER_PRESENTABLE_NO;
        case E::AdapterPresentability::Unknown:
        default:
            return STURDY_ADAPTER_PRESENTABLE_UNKNOWN;
        }
    }

    [[nodiscard]] bool translate_preference(SturdyDevicePreference preference, E::DevicePreference *out) noexcept {
        switch (preference) {
        case STURDY_DEVICE_PREFERENCE_DEFAULT:
            *out = E::DevicePreference::Default;
            return true;
        case STURDY_DEVICE_PREFERENCE_HIGH_PERFORMANCE:
            *out = E::DevicePreference::HighPerformance;
            return true;
        case STURDY_DEVICE_PREFERENCE_LOW_POWER:
            *out = E::DevicePreference::LowPower;
            return true;
        case STURDY_DEVICE_PREFERENCE_DISCRETE_ONLY:
            *out = E::DevicePreference::DiscreteOnly;
            return true;
        case STURDY_DEVICE_PREFERENCE_INTEGRATED_ONLY:
            *out = E::DevicePreference::IntegratedOnly;
            return true;
        case STURDY_DEVICE_PREFERENCE_FORCE_U32:
        default:
            return false;
        }
    }

    /// Resolves `engine` and runs `body(engine)`.
    template <class Body>
    [[nodiscard]] SturdyResult with_engine(SturdyEngine engine, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            E::Engine *resolved = nullptr;
            const SturdyResult result = resolve_engine(engine, &resolved);
            if (result != STURDY_OK) {
                return result;
            }
            return body(*resolved);
        });
    }

    /// Resolves the engine and checks the window id is non-null before running `body`.
    template <class Body>
    [[nodiscard]] SturdyResult with_window(SturdyEngine engine, SturdySurface surface, Body &&body) noexcept {
        return with_engine(engine, [&](E::Engine &resolved) -> SturdyResult { return body(resolved, window_of(surface)); });
    }

    [[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

    /// Owns a foreign callback's user data: the release hook runs when the last copy of the engine-side std::function goes away.
    struct CallbackOwner {
        CallbackOwner(void *user_data, void(STURDY_ABI_CALL *release)(void *)) : user(user_data), release_user(release) {}
        CallbackOwner(const CallbackOwner &) = delete;
        CallbackOwner &operator=(const CallbackOwner &) = delete;
        ~CallbackOwner() {
            if (release_user != nullptr) {
                release_user(user);
            }
        }

        void *user;
        void(STURDY_ABI_CALL *release_user)(void *);
    };

    void translate_change(const E::DeviceChangeEvent &change, SturdyDeviceChangeEvent *out) noexcept {
        *out = {};
        switch (change.kind) {
        case E::DeviceChangeKind::Switching:
            out->kind = STURDY_DEVICE_CHANGE_SWITCHING;
            break;
        case E::DeviceChangeKind::Switched:
            out->kind = STURDY_DEVICE_CHANGE_SWITCHED;
            break;
        case E::DeviceChangeKind::Recovered:
            out->kind = STURDY_DEVICE_CHANGE_RECOVERED;
            break;
        case E::DeviceChangeKind::LostUnrecoverable:
            out->kind = STURDY_DEVICE_CHANGE_LOST_UNRECOVERABLE;
            break;
        }
        if (!translate_backend(change.backend, &out->backend)) {
            out->backend = STURDY_BACKEND_DEFAULT;
        }
        out->generation = change.generation;
        copy_text(out->adapter_name, change.adapter_name);
        copy_text(out->message, change.message);
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_engine_set_frame_mode(SturdyEngine engine, SturdyFrameMode mode) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        switch (mode) {
        case STURDY_FRAME_MODE_CONTINUOUS:
            e.set_frame_mode(E::FrameMode::Continuous);
            return STURDY_OK;
        case STURDY_FRAME_MODE_ON_DEMAND:
            e.set_frame_mode(E::FrameMode::OnDemand);
            return STURDY_OK;
        case STURDY_FRAME_MODE_FORCE_U32:
        default:
            return invalid("unrecognized frame mode");
        }
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_frame_mode(SturdyEngine engine, SturdyFrameMode *out_mode) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_mode == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_mode = e.frame_mode() == E::FrameMode::OnDemand ? STURDY_FRAME_MODE_ON_DEMAND : STURDY_FRAME_MODE_CONTINUOUS;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_request_redraw(SturdyEngine engine) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        e.request_redraw();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_request_redraw_after(SturdyEngine engine, uint64_t delay_nanoseconds) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        // The delay is added to a steady_clock reading; clamp so a huge value cannot overflow it.
        constexpr uint64_t longest = static_cast<uint64_t>(std::numeric_limits<SFT::i64>::max() / 4);
        e.request_redraw_after(std::chrono::nanoseconds{static_cast<SFT::i64>(std::min(delay_nanoseconds, longest))});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_inject_key(SturdyEngine engine, SturdySurface surface, int32_t key, SturdyBool pressed, uint32_t modifiers,
                                                     SturdyBool repeat) {
    return with_window(engine, surface, [&](E::Engine &e, SFT::WindowManager::WindowId window) -> SturdyResult {
        e.inject_key_event(window, static_cast<SFT::WindowManager::KeyboardKey>(key), pressed != 0, modifiers, repeat != 0);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_inject_text(SturdyEngine engine, SturdySurface surface, const char *utf8) {
    return with_window(engine, surface, [&](E::Engine &e, SFT::WindowManager::WindowId window) -> SturdyResult {
        if (utf8 == nullptr) {
            return invalid("text must not be null");
        }
        e.inject_text_event(window, std::string_view{utf8});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_inject_mouse_move(SturdyEngine engine, SturdySurface surface, float x, float y, float delta_x, float delta_y,
                                                            uint32_t buttons) {
    return with_window(engine, surface, [&](E::Engine &e, SFT::WindowManager::WindowId window) -> SturdyResult {
        if (!finite(x) || !finite(y) || !finite(delta_x) || !finite(delta_y)) {
            return invalid("coordinates must be finite");
        }
        e.inject_mouse_move(window, x, y, delta_x, delta_y, buttons);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_inject_mouse_button(SturdyEngine engine, SturdySurface surface, SturdyMouseButton button, SturdyBool pressed, float x,
                                                              float y, uint8_t clicks) {
    return with_window(engine, surface, [&](E::Engine &e, SFT::WindowManager::WindowId window) -> SturdyResult {
        SFT::WindowManager::MouseButton translated{};
        if (!translate_mouse_button(button, &translated)) {
            return invalid("unrecognized mouse button");
        }
        if (!finite(x) || !finite(y)) {
            return invalid("coordinates must be finite");
        }
        e.inject_mouse_button(window, translated, pressed != 0, x, y, clicks);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_inject_mouse_wheel(SturdyEngine engine, SturdySurface surface, float x, float y, float mouse_x, float mouse_y) {
    return with_window(engine, surface, [&](E::Engine &e, SFT::WindowManager::WindowId window) -> SturdyResult {
        if (!finite(x) || !finite(y) || !finite(mouse_x) || !finite(mouse_y)) {
            return invalid("coordinates must be finite");
        }
        e.inject_mouse_wheel(window, x, y, mouse_x, mouse_y);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_next_drop_event(SturdyEngine engine, SturdyDropEvent *out_event, char *data_buffer, size_t capacity, size_t *out_length) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_event == nullptr) {
            return invalid("output pointer must not be null");
        }
        const std::lock_guard lock{g_backlog_mutex};
        Backlog &backlog = g_backlogs[&e];
        if (backlog.drops.empty()) {
            for (E::Engine::DropEvent &event : e.take_drop_events()) {
                backlog.drops.push_back(std::move(event));
            }
        }
        if (backlog.drops.empty()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no drag-and-drop event is waiting");
        }
        const E::Engine::DropEvent &front = backlog.drops.front();
        out_event->window = static_cast<uint64_t>(front.window);
        switch (front.kind) {
        case E::Engine::DropEvent::Kind::Begin:
            out_event->kind = STURDY_DROP_BEGIN;
            break;
        case E::Engine::DropEvent::Kind::Hover:
            out_event->kind = STURDY_DROP_HOVER;
            break;
        case E::Engine::DropEvent::Kind::File:
            out_event->kind = STURDY_DROP_FILE;
            break;
        case E::Engine::DropEvent::Kind::Text:
            out_event->kind = STURDY_DROP_TEXT;
            break;
        case E::Engine::DropEvent::Kind::Complete:
            out_event->kind = STURDY_DROP_COMPLETE;
            break;
        }
        out_event->x = static_cast<float>(front.position.x);
        out_event->y = static_cast<float>(front.position.y);
        out_event->data_length = static_cast<uint32_t>(front.data.size());
        const SturdyResult copied = copy_string_out(front.data, data_buffer, capacity, out_length);
        if (copied != STURDY_OK) {
            return copied; // too small: the event stays queued so the caller can retry with room
        }
        backlog.drops.pop_front();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_render_health(SturdyEngine engine, SturdyRenderHealth *out_health) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_health == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto health = e.render_health();
        *out_health = {health.frames_rendered, health.frames_failed, health.consecutive_failures};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_render_health_last_error(SturdyEngine engine, char *buffer, size_t capacity, size_t *out_length) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        const auto health = e.render_health();
        return copy_string_out(health.last_error, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_device_generation(SturdyEngine engine, uint64_t *out_generation) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_generation == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_generation = e.device_generation();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_available_backends(SturdyEngine engine, SturdyBackend *out_backends, uint32_t capacity, uint32_t *out_count) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("out_count must not be null");
        }
        const auto backends = e.available_backends();
        *out_count = static_cast<uint32_t>(backends.size());
        if (out_backends == nullptr) {
            return STURDY_OK;
        }
        const uint32_t count = std::min<uint32_t>(*out_count, capacity);
        for (uint32_t i = 0; i < count; ++i) {
            if (!translate_backend(backends[i], &out_backends[i])) {
                return set_error(STURDY_ERROR_NOT_AVAILABLE, "an available backend has no ABI spelling");
            }
        }
        return *out_count > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "output buffer is smaller than the list") : STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_adapter_count(SturdyEngine engine, uint32_t *out_count) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = static_cast<uint32_t>(e.available_adapters().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_adapter_description(SturdyEngine engine, uint32_t adapter_index, SturdyAdapterDescription *out_adapter) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_adapter == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto adapters = e.available_adapters();
        if (adapter_index >= adapters.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "adapter index is out of range");
        }
        const E::AdapterDescription &adapter = adapters[adapter_index];
        *out_adapter = {};
        copy_text(out_adapter->stable_id, adapter.stable_id);
        copy_text(out_adapter->physical_device_id, adapter.physical_device_id);
        copy_text(out_adapter->name, adapter.name);
        copy_text(out_adapter->vendor, adapter.vendor);
        out_adapter->vendor_id = adapter.vendor_id;
        out_adapter->device_id = adapter.device_id;
        out_adapter->device_type = translate_device_type(adapter.device_type);
        out_adapter->backend_count = static_cast<uint32_t>(adapter.backends.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_adapter_backend(SturdyEngine engine, uint32_t adapter_index, uint32_t offer_index, SturdyBackendOffer *out_offer) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_offer == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto adapters = e.available_adapters();
        if (adapter_index >= adapters.size() || offer_index >= adapters[adapter_index].backends.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "adapter or backend index is out of range");
        }
        const E::BackendOffer &offer = adapters[adapter_index].backends[offer_index];
        *out_offer = {};
        if (!translate_backend(offer.backend, &out_offer->backend)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the backend has no ABI spelling");
        }
        copy_text(out_offer->driver_version, offer.driver_version);
        copy_text(out_offer->api_version, offer.api_version);
        out_offer->presentable = translate_presentable(offer.presentable);
        out_offer->active = offer.active ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_device_request_init(SturdyDeviceRequest *request) {
    return guarded([&]() -> SturdyResult {
        if (request == nullptr) {
            return invalid("request must not be null");
        }
        *request = {sizeof(SturdyDeviceRequest), STURDY_BACKEND_DEFAULT, nullptr, STURDY_DEVICE_PREFERENCE_DEFAULT, STURDY_FALSE};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_switch_device(SturdyEngine engine, const SturdyDeviceRequest *request, SturdyDeviceSwitchResult *out_result) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (request == nullptr || out_result == nullptr) {
            return invalid("request and output pointer must not be null");
        }
        if (request->struct_size != sizeof(SturdyDeviceRequest)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyDeviceRequest::struct_size does not match this build");
        }
        E::DeviceRequest native;
        if (!translate_backend(request->backend, &native.backend)) {
            return invalid("unrecognized backend");
        }
        if (!translate_preference(request->preference, &native.preference)) {
            return invalid("unrecognized device preference");
        }
        if (request->adapter_id != nullptr && request->adapter_id[0] != '\0') {
            native.adapter_id = std::string{request->adapter_id};
        }
        native.force_recreate = request->force_recreate != 0;

        const E::DeviceSwitchResult result = e.switch_device(native);
        *out_result = {};
        switch (result.outcome) {
        case E::DeviceSwitchOutcome::Switched:
            out_result->outcome = STURDY_DEVICE_SWITCH_SWITCHED;
            break;
        case E::DeviceSwitchOutcome::SwitchedWithFallback:
            out_result->outcome = STURDY_DEVICE_SWITCH_FALLBACK;
            break;
        case E::DeviceSwitchOutcome::Failed:
            out_result->outcome = STURDY_DEVICE_SWITCH_FAILED;
            break;
        }
        if (!translate_backend(result.backend, &out_result->backend)) {
            out_result->backend = STURDY_BACKEND_DEFAULT;
        }
        out_result->generation = result.generation;
        out_result->rolled_back = result.rolled_back ? STURDY_TRUE : STURDY_FALSE;
        copy_text(out_result->reason, result.reason);
        copy_text(out_result->adapter_name, result.adapter_name);
        copy_text(out_result->adapter_stable_id, result.adapter_stable_id);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_next_device_change(SturdyEngine engine, SturdyDeviceChangeEvent *out_event) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_event == nullptr) {
            return invalid("output pointer must not be null");
        }
        const std::lock_guard lock{g_backlog_mutex};
        Backlog &backlog = g_backlogs[&e];
        if (backlog.device_changes.empty()) {
            for (E::DeviceChangeEvent &event : e.take_device_change_events()) {
                backlog.device_changes.push_back(std::move(event));
            }
        }
        if (backlog.device_changes.empty()) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no device change is waiting");
        }
        translate_change(backlog.device_changes.front(), out_event);
        backlog.device_changes.pop_front();
        return STURDY_OK;
    });
}

// --- update boundary hooks ---

SturdyResult STURDY_ABI_CALL sturdy_engine_defer(SturdyEngine engine, SturdyDeferredAction action, void *user, void(STURDY_ABI_CALL *release_user)(void *user)) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (action == nullptr) {
            return invalid("action must not be null");
        }
        // The release hook runs when the last copy of the std::function dies, which is right after the action ran (or never ran: the engine shut down).
        auto owner = std::make_shared<CallbackOwner>(user, release_user);
        E::Engine *engine_ptr = &e;
        e.defer_to_update_boundary([owner, action, engine_ptr]() {
            const SFT::Ffi::ScopedHandle handle{SFT::Ffi::HandleKind::Engine, engine_ptr};
            action(SturdyEngine{handle.token()}, owner->user);
        });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_add_update_hook(SturdyEngine engine, SturdyUpdateHook hook, void *user, void(STURDY_ABI_CALL *release_user)(void *user),
                                                           uint64_t *out_hook) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (hook == nullptr || out_hook == nullptr) {
            return invalid("hook and output pointer must not be null");
        }
        auto owner = std::make_shared<CallbackOwner>(user, release_user);
        E::Engine *engine_ptr = &e;
        *out_hook = e.add_update_hook([owner, hook, engine_ptr](SFT::f64 delta_seconds) {
            const SFT::Ffi::ScopedHandle handle{SFT::Ffi::HandleKind::Engine, engine_ptr};
            hook(SturdyEngine{handle.token()}, delta_seconds, owner->user);
        });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_remove_update_hook(SturdyEngine engine, uint64_t hook) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        return e.remove_update_hook(hook) ? STURDY_OK : set_error(STURDY_ERROR_NOT_AVAILABLE, "no update hook has that id");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_add_device_observer(SturdyEngine engine, SturdyDeviceObserver observer, void *user,
                                                               void(STURDY_ABI_CALL *release_user)(void *user), uint64_t *out_observer) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (observer == nullptr || out_observer == nullptr) {
            return invalid("observer and output pointer must not be null");
        }
        auto owner = std::make_shared<CallbackOwner>(user, release_user);
        E::Engine *engine_ptr = &e;
        *out_observer = e.add_device_observer([owner, observer, engine_ptr](const E::DeviceChangeEvent &change) {
            SturdyDeviceChangeEvent foreign{};
            translate_change(change, &foreign);
            const SFT::Ffi::ScopedHandle handle{SFT::Ffi::HandleKind::Engine, engine_ptr};
            observer(SturdyEngine{handle.token()}, &foreign, owner->user);
        });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_remove_device_observer(SturdyEngine engine, uint64_t observer) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        e.remove_device_observer(static_cast<SFT::usize>(observer));
        return STURDY_OK;
    });
}

// --- runtime settings and presentation timing ---

SturdyResult STURDY_ABI_CALL sturdy_engine_apply_runtime_settings(SturdyEngine engine, SturdySurface surface, const SturdyRuntimeConfig *config,
                                                                  SturdyRuntimeSettingsResult *out_result) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (config == nullptr || out_result == nullptr) {
            return invalid("config and output pointer must not be null");
        }
        if (config->struct_size < sizeof(SturdyRuntimeConfig)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyRuntimeConfig::struct_size is smaller than this build's");
        }
        E::EngineConfig native;
        std::optional<R::BackendType> backend;
        if (!translate_backend(config->graphics_backend, &backend)) {
            return invalid("unrecognized graphics backend");
        }
        // The default means "whatever this platform normally runs", which for a running engine is the backend it is on.
        native.graphics_backend = backend.value_or(e.rhi_device() != nullptr ? e.rhi_device()->backend_type() : R::BackendType::Vulkan);
        if (config->physical_device_id != nullptr) {
            native.graphics_physical_device_id = config->physical_device_id;
        }
        native.features.raytracing = config->enable_raytracing != 0;
        native.features.enable_native_access_extension = config->enable_native_access != 0;
        native.enable_shader_disk_cache = config->enable_shader_disk_cache != 0;
        const std::string app_name = config->app_name != nullptr ? config->app_name : "Sturdy Engine 5";
        native.app_name = app_name.c_str();
        if (config->shaders_directory != nullptr) {
            native.shaders_directory = config->shaders_directory;
        }
        const auto applied = e.apply_runtime_settings(SFT::Core::RenderSurfaceHandle{static_cast<SFT::WindowManager::WindowId>(surface.id)}, native);
        if (!applied) {
            return set_error(STURDY_ERROR_INITIALIZATION_FAILED, std::string{applied.error().message});
        }
        *out_result = {};
        using Mode = SFT::Core::RuntimeSettingApplyMode;
        switch (applied->mode) {
        case Mode::NoChange:
            out_result->mode = STURDY_RUNTIME_SETTING_NO_CHANGE;
            break;
        case Mode::HotApplied:
            out_result->mode = STURDY_RUNTIME_SETTING_HOT_APPLIED;
            break;
        case Mode::SurfaceRecreated:
            out_result->mode = STURDY_RUNTIME_SETTING_SURFACE_RECREATED;
            break;
        case Mode::DeviceRecreated:
            out_result->mode = STURDY_RUNTIME_SETTING_DEVICE_RECREATED;
            break;
        case Mode::BackendRecreated:
            out_result->mode = STURDY_RUNTIME_SETTING_BACKEND_RECREATED;
            break;
        case Mode::Unsupported:
            out_result->mode = STURDY_RUNTIME_SETTING_UNSUPPORTED;
            break;
        }
        copy_text(out_result->message, applied->message.cpp_string_view());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_present_timing_feedback(SturdyEngine engine, SturdySurface surface, SturdyPresentTimingFeedback *out_feedback) {
    return with_engine(engine, [&](E::Engine &e) -> SturdyResult {
        if (out_feedback == nullptr) {
            return invalid("output pointer must not be null");
        }
        const SFT::Core::PresentTimingFeedback feedback =
            e.present_timing_feedback(SFT::Core::RenderSurfaceHandle{static_cast<SFT::WindowManager::WindowId>(surface.id)});
        SturdyPresentTimingFeedback out{};
        out.available = feedback.available ? STURDY_TRUE : STURDY_FALSE;
        out.has_refresh_duration = feedback.refresh_duration_seconds.has_value() ? STURDY_TRUE : STURDY_FALSE;
        out.refresh_duration_seconds = feedback.refresh_duration_seconds.value_or(0.0);
        out.has_variable_refresh = feedback.variable_refresh_active.has_value() ? STURDY_TRUE : STURDY_FALSE;
        out.variable_refresh_active = feedback.variable_refresh_active.value_or(false) ? STURDY_TRUE : STURDY_FALSE;
        out.sample_count = feedback.sample_count;
        out.mean_display_interval = feedback.mean_display_interval;
        out.display_interval_jitter = feedback.display_interval_jitter;
        out.max_display_interval = feedback.max_display_interval;
        out.missed_refreshes = feedback.missed_refreshes;
        out.presents_reported = feedback.presents_reported;
        out.last_present_id = feedback.last_present_id;
        out.last_display_time_ns = feedback.last_display_time_ns;
        out.frame_to_display_estimate = feedback.frame_to_display_estimate;
        *out_feedback = out;
        return STURDY_OK;
    });
}

} // extern "C"
