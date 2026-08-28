/// Compiles `FFI/Sturdy.h` as C99 and exercises the parts of the ABI that need no window or
/// graphics device.
///
/// Being built as C rather than C++ is the entire point of this test: it is what proves the header
/// contains no C++-only construct, since every other test in the tree compiles it as C++ and would
/// happily accept declarations no C consumer could use.

#include <stdio.h>

#include <FFI/Sturdy.h>

static int failures = 0;

/// Records a failed expectation.
///
/// Deliberately not `assert`: these checks must hold in optimized configurations too, and `assert`
/// compiles to nothing once `NDEBUG` is defined, which would leave the test passing vacuously in
/// exactly the builds shipped to users.
///
/// @param condition Nonzero when the expectation held.
/// @param description What was expected, reported when it did not hold.
static void check(int condition, const char *description) {
    if (!condition) {
        (void)fprintf(stderr, "FfiHeaderCTest: %s\n", description);
        ++failures;
    }
}

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
int main(void) {
    SturdyRuntimeConfig config;
    SturdyBool down = STURDY_TRUE;
    SturdyResult result;

    check(sturdy_abi_version_major() == STURDY_ABI_VERSION_MAJOR,
          "major ABI version disagrees with the header");
    check(sturdy_abi_version_minor() == STURDY_ABI_VERSION_MINOR,
          "minor ABI version disagrees with the header");

    // The ABI is 64-bit only, matching the engine's own static assertions. A 32-bit consumer would
    // silently disagree about every pointer and handle, so refuse to compile rather than link.
    check(sizeof(void *) == 8, "the C ABI supports 64-bit targets only");
    check(sizeof(SturdyBool) == 1, "SturdyBool must be exactly one byte");
    check(sizeof(SturdyResult) == 4, "enums must be exactly four bytes wide");
    check(sizeof(SturdyEngine) == 8, "handles must be exactly eight bytes");
    /* Every struct carrying pointers must stay 8-byte aligned so its layout is identical across
       every 64-bit target in the matrix. */
    check(sizeof(SturdyVulkanHandles) % 8 == 0, "SturdyVulkanHandles must be 8-byte aligned in size");
    check(sizeof(SturdyD3D12Handles) % 8 == 0, "SturdyD3D12Handles must be 8-byte aligned in size");
    check(sizeof(SturdyDeviceLimits) % 8 == 0, "SturdyDeviceLimits must be 8-byte aligned in size");

    check(sturdy_last_error_message() != NULL, "the error message pointer is never null");

    result = sturdy_runtime_config_init(NULL);
    check(result == STURDY_ERROR_INVALID_ARGUMENT, "a null config must be rejected");

    result = sturdy_runtime_config_init(&config);
    check(result == STURDY_OK, "config initialization must succeed");
    check(config.struct_size == sizeof(SturdyRuntimeConfig),
          "config initialization must stamp struct_size");
    check(config.window_width != 0 && config.window_height != 0,
          "config initialization must supply nonzero window dimensions");
    check(sturdy_last_error_message()[0] == '\0',
          "a successful call must leave the error message empty");

    // A zero-initialized handle is the shape a caller gets from calloc or a default-constructed
    // binding struct. It must be rejected rather than treated as referring to anything.
    {
        SturdyEngine unset_engine;
        SturdyFrame unset_frame;
        unset_engine.token = 0;
        unset_frame.token = 0;

        result = sturdy_engine_key_down(unset_engine, (int32_t)' ', &down);
        check(result == STURDY_ERROR_INVALID_HANDLE, "a zero engine handle must be rejected");
        check(sturdy_last_error_message()[0] != '\0', "a failure must leave error detail behind");

        result = sturdy_frame_set_resolution_scale(unset_frame, 1.0f);
        check(result == STURDY_ERROR_INVALID_HANDLE, "a zero frame handle must be rejected");
    }

    // Arguments are validated before the runtime claims anything, so these are safe to call in a
    // headless test: none of them reach window or device creation.
    result = sturdy_runtime_run(NULL, NULL, 0, NULL, NULL);
    check(result == STURDY_ERROR_INVALID_ARGUMENT, "a null config and game logic must be rejected");

    /* Feature enumeration needs no engine, so a C consumer can resolve the feature names it cares
       about at load time, before any device exists. */
    {
        uint32_t feature_count = 0;
        uint32_t index = 0;
        size_t length = 0;
        char feature[256];

        result = sturdy_rhi_feature_count(&feature_count);
        check(result == STURDY_OK && feature_count > 0, "the RHI must expose features");

        result = sturdy_rhi_feature_name(0, feature, sizeof(feature), &length);
        check(result == STURDY_OK, "a feature name must be readable");
        check(length > 1 && feature[0] != '\0', "a feature name must be non-empty");

        result = sturdy_rhi_feature_index(feature, &index);
        check(result == STURDY_OK && index == 0, "a feature name must round-trip to its index");

        result = sturdy_rhi_feature_name(feature_count, feature, sizeof(feature), &length);
        check(result == STURDY_ERROR_OUT_OF_RANGE, "an out-of-range feature index must be rejected");
    }

    {
        SturdyGameLogic logic;
        logic.struct_size = 1; /* deliberately wrong */
        logic.reserved = 0;
        logic.user_data = NULL;
        logic.on_engine_initialized = NULL;
        logic.request_render_frame = NULL;
        logic.on_shutdown = NULL;
        logic.destroy = NULL;

        result = sturdy_runtime_run(&config, &logic, 0, NULL, NULL);
        check(result == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
              "a game logic struct of unknown size must be rejected");
    }

    if (failures != 0) {
        (void)fprintf(stderr, "FfiHeaderCTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
