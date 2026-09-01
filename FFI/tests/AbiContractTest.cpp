/// Verifies the safety properties the C ABI promises but that no C consumer can provoke on its
/// own: that a handle stops working the moment its callback scope ends, that a handle of the wrong
/// kind is rejected rather than reinterpreted, and that argument validation happens before any
/// engine state is touched.
///
/// Reaches into `FFI/AbiSupport.hpp` (the internal handle table) because the interesting cases —
/// a token that was valid and now is not — cannot be constructed through the public header by
/// design. That is also why this test is only built for static configurations, where the internal
/// symbols are still linkable.

#include <cstddef>
#include <cstdio>
#include <limits>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;

    /// Records a failed expectation.
    ///
    /// Deliberately not `assert`: these checks must hold in optimized configurations too, and
    /// `assert` compiles to nothing once `NDEBUG` is defined, which would leave the test passing
    /// vacuously in exactly the builds shipped to users.
    ///
    /// @param condition True when the expectation held.
    /// @param description What was expected, reported when it did not hold.
    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "FfiContractTest: %s\n", description);
            ++failures;
        }
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::revoke_handle;

    SFT::Engine::RenderFrameParameters parameters{};
    parameters.render_graph = SFT::Engine::RenderGraph::standard();

    // A live handle behaves exactly as it would inside request_render_frame.
    const SFT::u64 live_token = mint_handle(HandleKind::Frame, &parameters);
    const SturdyFrame live{live_token};

    check(sturdy_frame_set_resolution_scale(live, 0.5f) == STURDY_OK,
          "a live frame handle must accept a valid resolution scale");
    check(parameters.render_graph.description().resolution_scale == 0.5f,
          "the setter must reach the underlying render graph");

    check(sturdy_frame_set_camera_position(live, 1.0f, 2.0f, 3.0f) == STURDY_OK,
          "a live frame handle must accept a camera position");
    check(parameters.camera.position() == glm::vec3(1.0f, 2.0f, 3.0f),
          "the camera position must be applied");

    // Argument validation: values a C++ caller could not express by accident, but a foreign caller
    // can pass trivially. Each must be refused without mutating anything.
    check(sturdy_frame_set_resolution_scale(live, 0.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "a zero resolution scale must be rejected");
    check(sturdy_frame_set_resolution_scale(live, 5.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "an out-of-range resolution scale must be rejected");
    check(parameters.render_graph.description().resolution_scale == 0.5f,
          "a rejected setter must leave the previous value intact");

    check(sturdy_frame_set_camera_perspective(live, 0.0f, 0.1f, 100.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "a degenerate field of view must be rejected");
    check(sturdy_frame_set_camera_perspective(live, 60.0f, 10.0f, 1.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "inverted clip planes must be rejected");
    check(sturdy_frame_set_camera_viewport(live, 0, 720) == STURDY_ERROR_INVALID_ARGUMENT,
          "a zero viewport dimension must be rejected");
    check(sturdy_frame_set_ambient_light(live, -1.0f, 0.0f, 0.0f, 1.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "negative ambient radiance must be rejected");

    // look_at() is degenerate when the target coincides with the camera; the ABI refuses rather
    // than producing a NaN orientation that would surface much later as a blank frame.
    check(sturdy_frame_camera_look_at(live, 1.0f, 2.0f, 3.0f) == STURDY_ERROR_INVALID_ARGUMENT,
          "looking at the camera's own position must be rejected");

    // Unrecognized enum values: the compiler cannot stop a foreign caller from inventing one.
    check(sturdy_frame_set_feature_enabled(live, static_cast<SturdyRenderFeature>(9999), STURDY_TRUE) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "an unrecognized render feature must be rejected");
    check(sturdy_frame_set_tone_mapping(live, static_cast<SturdyToneMapping>(9999), 1.0f, 1.0f, 1.0f) ==
              STURDY_ERROR_INVALID_ARGUMENT,
          "an unrecognized tone mapping operator must be rejected");

    // Kind confusion: SturdyEngine and SturdyFrame are structurally identical at the ABI, so only
    // the recorded kind can tell them apart.
    const SturdyEngine frame_as_engine{live_token};
    SturdyBool down = STURDY_TRUE;
    check(sturdy_engine_key_down(frame_as_engine, 32, &down) == STURDY_ERROR_INVALID_HANDLE,
          "a frame handle used as an engine handle must be rejected");

    check(sturdy_engine_mouse_position(frame_as_engine, nullptr, nullptr) == STURDY_ERROR_INVALID_HANDLE,
          "handle validation must run before null output pointers are tolerated");

    // The core scope property: once the callback that provided a handle returns, the handle is
    // dead. A binding that stashes one — easy to do accidentally from a garbage-collected language
    // — gets an error rather than a dangling write into a destroyed frame.
    revoke_handle(live_token);
    check(sturdy_frame_set_resolution_scale(live, 1.0f) == STURDY_ERROR_HANDLE_EXPIRED,
          "a revoked handle must report expiry, not invalidity");

    // Tokens are never reused, so a stale handle can never alias a later object.
    const SFT::u64 next_token = mint_handle(HandleKind::Frame, &parameters);
    check(next_token != live_token, "handle tokens must never be reused");
    revoke_handle(next_token);

    // A token from beyond the counter was never minted at all, which is a different diagnosis.
    const SturdyFrame never_minted{next_token + 1000};
    check(sturdy_frame_set_resolution_scale(never_minted, 1.0f) == STURDY_ERROR_INVALID_HANDLE,
          "a token that was never minted must report invalidity, not expiry");

    // ── RHI feature enumeration ────────────────────────────────────────────────────────────────
    // Engine-free by design, so it is fully testable headlessly: a binding is meant to resolve the
    // feature names it cares about once at startup, before any device exists.
    uint32_t feature_count = 0;
    check(sturdy_rhi_feature_count(&feature_count) == STURDY_OK, "feature count must be readable");
    check(feature_count > 0, "the engine must expose at least one RHI feature");
    check(sturdy_rhi_feature_count(nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
          "a null feature count output must be rejected");

    // Size query: null buffer asks how much room the name needs, without writing anything.
    std::size_t required = 0;
    check(sturdy_rhi_feature_name(0, nullptr, 0, &required) == STURDY_OK,
          "a null buffer must be accepted as a size query");
    check(required > 1, "a feature name must be more than just a terminator");

    char name[256] = {};
    std::size_t written = 0;
    check(sturdy_rhi_feature_name(0, name, sizeof(name), &written) == STURDY_OK,
          "a feature name must fit a generous buffer");
    check(written == required, "the size query must agree with the real write");
    check(name[0] != '\0', "a feature name must not be empty");

    // Truncation: the caller still gets a null-terminated string, so ignoring the status yields a
    // short name rather than a buffer overrun or an unterminated read.
    char tiny[4] = {'x', 'x', 'x', 'x'};
    std::size_t tiny_required = 0;
    check(sturdy_rhi_feature_name(0, tiny, sizeof(tiny), &tiny_required) == STURDY_ERROR_BUFFER_TOO_SMALL,
          "a too-small buffer must be reported");
    check(tiny[sizeof(tiny) - 1] == '\0', "a truncated write must still be null-terminated");
    check(tiny_required == required, "a truncated write must still report the required size");

    check(sturdy_rhi_feature_name(feature_count, name, sizeof(name), &written) == STURDY_ERROR_OUT_OF_RANGE,
          "a feature index past the end must be rejected");

    // Name round-trip: the index-to-name and name-to-index directions must agree, since that pair
    // is what lets a binding survive an engine that has added features it has never heard of.
    uint32_t resolved_index = feature_count;
    check(sturdy_rhi_feature_index(name, &resolved_index) == STURDY_OK,
          "a name produced by this ABI must resolve back to an index");
    check(resolved_index == 0, "the round-tripped index must match the original");
    check(sturdy_rhi_feature_index("definitely-not-a-real-feature", &resolved_index) ==
              STURDY_ERROR_NOT_AVAILABLE,
          "an unknown feature name must report unavailability, not an invalid argument");
    check(sturdy_rhi_feature_index(nullptr, &resolved_index) == STURDY_ERROR_INVALID_ARGUMENT,
          "a null feature name must be rejected");

    // ── Engine-scoped queries reject dead handles ──────────────────────────────────────────────
    // The device-backed calls need a real GPU, so what is verified here is the part this layer
    // actually owns: that every one of them runs the same handle checks before touching anything.
    {
        SturdyBackend backend = STURDY_BACKEND_VULKAN;
        SturdyAdapterInfo adapter_info{};
        SturdyDeviceLimits device_limits{};
        SturdyQueueInfo queue_info{};
        SturdyVulkanHandles vulkan_handles{};
        SturdyD3D12Handles d3d12_handles{};
        SturdyBool native_available = STURDY_TRUE;
        uint32_t count = 0;
        void *queue = nullptr;

        const SturdyEngine dead{live_token};
        check(sturdy_rhi_backend(dead, &backend) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_backend must reject an expired handle");
        check(sturdy_rhi_adapter_info(dead, &adapter_info) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_adapter_info must reject an expired handle");
        check(sturdy_rhi_adapter_string(dead, STURDY_ADAPTER_STRING_NAME, name, sizeof(name), &written) ==
                  STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_adapter_string must reject an expired handle");
        check(sturdy_rhi_device_limits(dead, &device_limits) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_device_limits must reject an expired handle");
        check(sturdy_rhi_feature_enabled(dead, 0, &native_available) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_feature_enabled must reject an expired handle");
        check(sturdy_rhi_queue_count(dead, &count) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_queue_count must reject an expired handle");
        check(sturdy_rhi_queue_info(dead, 0, &queue_info) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_queue_info must reject an expired handle");
        check(sturdy_rhi_extension_count(dead, &count) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_extension_count must reject an expired handle");
        check(sturdy_rhi_extension_name(dead, 0, name, sizeof(name), &written, nullptr) ==
                  STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_rhi_extension_name must reject an expired handle");
        check(sturdy_native_available(dead, &native_available) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_native_available must reject an expired handle");
        check(sturdy_native_vulkan(dead, &vulkan_handles) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_native_vulkan must reject an expired handle");
        check(sturdy_native_d3d12(dead, &d3d12_handles) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_native_d3d12 must reject an expired handle");
        check(sturdy_native_vulkan_queue(dead, STURDY_QUEUE_CLASS_GRAPHICS, 0, &queue, nullptr) ==
                  STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_native_vulkan_queue must reject an expired handle");
        check(sturdy_native_d3d12_queue(dead, STURDY_QUEUE_CLASS_GRAPHICS, 0, &queue) ==
                  STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_native_d3d12_queue must reject an expired handle");

        // Argument validation must run before handle resolution where the argument is nonsense
        // regardless of which engine it would have been applied to.
        check(sturdy_native_vulkan_queue(dead, static_cast<SturdyQueueClass>(9999), 0, &queue, nullptr) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized queue class must be rejected");
    }

    // ── Window and time queries reject dead handles ────────────────────────────────────────────
    {
        SturdyWindowSnapshot snapshot{};
        double seconds = 0.0;
        uint64_t tick = 0;
        uint32_t count = 0;

        const SturdyEngine dead{live_token};
        const SturdySurface any_surface{0};

        check(sturdy_time_delta_seconds(dead, &seconds) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_time_delta_seconds must reject an expired handle");
        check(sturdy_time_unscaled_delta_seconds(dead, &seconds) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_time_unscaled_delta_seconds must reject an expired handle");
        check(sturdy_time_tick_index(dead, &tick) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_time_tick_index must reject an expired handle");
        check(sturdy_time_scale(dead, &seconds) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_time_scale must reject an expired handle");
        check(sturdy_time_set_scale(dead, 1.0) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_time_set_scale must reject an expired handle");
        check(sturdy_window_count(dead, &count) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_count must reject an expired handle");
        check(sturdy_window_snapshot(dead, 0, &snapshot) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_snapshot must reject an expired handle");
        check(sturdy_window_primary(dead, &snapshot) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_primary must reject an expired handle");
        check(sturdy_window_find(dead, any_surface, &snapshot) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_find must reject an expired handle");
        check(sturdy_window_request_close(dead, any_surface, nullptr) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_request_close must reject an expired handle");
        check(sturdy_window_set_decorated(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_decorated must reject an expired handle");
        check(sturdy_window_set_transparent(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_transparent must reject an expired handle");
        check(sturdy_window_set_relative_mouse_mode(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_relative_mouse_mode must reject an expired handle");
        check(sturdy_window_set_mouse_locked(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_mouse_locked must reject an expired handle");
        check(sturdy_window_set_cursor_grabbed(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_cursor_grabbed must reject an expired handle");
        check(sturdy_window_set_effect(dead, any_surface, STURDY_WINDOW_EFFECT_BLUR, STURDY_TRUE) ==
                  STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_effect must reject an expired handle");
        check(sturdy_window_set_text_input_active(dead, any_surface, STURDY_TRUE) == STURDY_ERROR_HANDLE_EXPIRED,
              "sturdy_window_set_text_input_active must reject an expired handle");
        {
            const SturdyTextInputArea area{0.0f, 0.0f, 100.0f, 20.0f, 0.0f};
            check(sturdy_window_set_text_input_area(dead, any_surface, &area) == STURDY_ERROR_HANDLE_EXPIRED,
                  "sturdy_window_set_text_input_area must reject an expired handle");
        }
        {
            SturdyWindowConfig window_config{};
            check(sturdy_window_config_init(&window_config) == STURDY_OK,
                  "sturdy_window_config_init must succeed with a valid output pointer");
            check(sturdy_window_spawn(dead, &window_config, nullptr) == STURDY_ERROR_HANDLE_EXPIRED,
                  "sturdy_window_spawn must reject an expired handle");
            check(sturdy_window_recreate_primary(dead, &window_config, nullptr) == STURDY_ERROR_HANDLE_EXPIRED,
                  "sturdy_window_recreate_primary must reject an expired handle");
        }
        {
            uint32_t completion_count = 0;
            check(sturdy_window_take_completions(dead, nullptr, 0, &completion_count) ==
                      STURDY_ERROR_HANDLE_EXPIRED,
                  "sturdy_window_take_completions must reject an expired handle");
        }

        // Values a foreign caller can pass that no engine state could make sensible are rejected
        // before the handle is even consulted.
        check(sturdy_time_set_scale(dead, -1.0) == STURDY_ERROR_INVALID_ARGUMENT,
              "a negative time scale must be rejected");
        check(sturdy_time_set_scale(dead, std::numeric_limits<double>::quiet_NaN()) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "a NaN time scale must be rejected");
        check(sturdy_window_set_cursor_icon(dead, any_surface, static_cast<SturdyCursorIcon>(9999)) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized cursor icon must be rejected");
        check(sturdy_window_set_mode(dead, any_surface, static_cast<SturdyWindowMode>(9999)) ==
                  STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized window mode must be rejected");
        check(sturdy_window_set_effect(dead, any_surface, static_cast<SturdyWindowEffectKind>(9999),
                                       STURDY_TRUE) == STURDY_ERROR_INVALID_ARGUMENT,
              "an unrecognized window effect kind must be rejected");
        check(sturdy_window_spawn(dead, nullptr, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null window config must be rejected");
        {
            SturdyWindowConfig bad_size_config{};
            check(sturdy_window_config_init(&bad_size_config) == STURDY_OK,
                  "sturdy_window_config_init must succeed with a valid output pointer");
            bad_size_config.struct_size = 1;
            check(sturdy_window_spawn(dead, &bad_size_config, nullptr) ==
                      STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                  "a mismatched SturdyWindowConfig struct_size must be rejected");

            SturdyWindowConfig bad_mode_config{};
            check(sturdy_window_config_init(&bad_mode_config) == STURDY_OK,
                  "sturdy_window_config_init must succeed with a valid output pointer");
            bad_mode_config.mode = static_cast<SturdyWindowMode>(9999);
            check(sturdy_window_spawn(dead, &bad_mode_config, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
                  "an unrecognized window mode in SturdyWindowConfig must be rejected");
        }
        check(sturdy_window_take_completions(dead, nullptr, 1, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null completions output count pointer must be rejected");
        {
            uint32_t completion_count = 0;
            check(sturdy_window_take_completions(dead, nullptr, 1, &completion_count) ==
                      STURDY_ERROR_INVALID_ARGUMENT,
                  "a null completions buffer with nonzero capacity must be rejected");
        }
        check(sturdy_window_set_text_input_area(dead, any_surface, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null text input area must be rejected");
        {
            const SturdyTextInputArea non_finite_area{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, 0.0f};
            check(sturdy_window_set_text_input_area(dead, any_surface, &non_finite_area) ==
                      STURDY_ERROR_INVALID_ARGUMENT,
                  "a non-finite text input area must be rejected");
        }

        // Null outputs are caller bugs regardless of engine state.
        check(sturdy_window_count(dead, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null window count output must be rejected");
        check(sturdy_time_delta_seconds(dead, nullptr) == STURDY_ERROR_INVALID_ARGUMENT,
              "a null time output must be rejected");
    }

    check(sturdy_last_error_message() != nullptr, "the error message pointer is never null");

    if (failures != 0) {
        (void)std::fprintf(stderr, "FfiContractTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
