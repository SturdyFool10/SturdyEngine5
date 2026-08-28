/// SturdyEngine 5 stable C ABI.
///
/// This is the language-neutral seam every non-C++ consumer (Rust, C#, Java, ...) binds against.
/// It is deliberately narrow: it covers the application-hosting seam only — configure a runtime,
/// supply game-logic callbacks, drive per-frame camera/lighting/render-graph choices, and read
/// input. Everything else in the engine stays C++ and is reached by writing more of this layer,
/// never by widening the boundary types.
///
/// Every rule below exists because a foreign caller cannot be trusted to honor a C++-side
/// invariant the way a same-language caller would.
///
/// ## Boundary rules
///
/// - **No C++ crosses this header.** Only fixed-width scalars, enums with an explicit 32-bit
///   sentinel, pointers, and structs of those. No `std::` types, no exceptions, no virtual
///   dispatch, no templates. This is what keeps the ABI identical across the Microsoft C++ ABI
///   (Clang-on-Windows) and the Itanium ABI (Linux/macOS/FreeBSD).
/// - **No exception ever escapes.** Every entry point below catches everything and converts it to
///   a `SturdyResult`. Conversely, a callback you supply must never unwind into engine code — a
///   Rust panic or a .NET exception crossing back through a function pointer is undefined
///   behavior. Catch on your own side of the boundary.
/// - **Handles are scope-bound.** `SturdyEngine` and `SturdyFrame` are only valid for the duration
///   of the callback that received them. Storing one and using it later is detected and rejected
///   with `STURDY_ERROR_HANDLE_EXPIRED`, not silently dereferenced — this is what makes the
///   boundary safe for a garbage-collected caller that might hold a wrapper object past its scope.
/// - **Structs are versioned by size.** Every caller-supplied struct begins with `struct_size`.
///   Always set it to `sizeof` the struct as your binding sees it. Fields are only ever appended,
///   so a newer engine can serve an older caller and reject a caller from the future.
/// - **Outputs are written only on success.** When a call returns anything other than `STURDY_OK`,
///   its output parameters are left exactly as the caller passed them. Zero-initialize them if you
///   intend to inspect them regardless of status, rather than reading whatever was on the stack.
///   The one exception is the string convention below, which fills `*out_length` even when it
///   reports `STURDY_ERROR_BUFFER_TOO_SMALL` — that length is what the caller needs to retry.
///
/// ## String outputs
///
/// Functions returning text take `char *buffer, size_t capacity, size_t *out_length`. They write at
/// most `capacity` bytes including the null terminator, always null-terminate when `capacity` is
/// nonzero, and set `*out_length` to the number of bytes required *including* the terminator.
/// Pass `NULL`/`0` to ask for the required size, then call again with a buffer that size. A
/// truncated write returns `STURDY_ERROR_BUFFER_TOO_SMALL` and still fills `*out_length`.
///
/// Caller-provided buffers rather than engine-owned pointers: it removes every question about who
/// frees what and how long a returned pointer stays valid, which is the part of a C ABI that most
/// often goes wrong across a language boundary.
///
/// ## Supported targets
///
/// 64-bit only, matching the engine itself: Windows/Linux/macOS/FreeBSD on x86-64 and Arm64.
/// wasm32 is not a supported ABI consumer target (32-bit pointers, different calling convention).
#ifndef STURDY_FFI_STURDY_H
#define STURDY_FFI_STURDY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------
// Linkage
// ---------------------------------------------------------------------------------------------

/// Symbol visibility for the ABI.
///
/// `STURDY_FFI_SHARED` is defined by the build for consumers of a shared build; the engine's own
/// translation units additionally define `STURDY_FFI_BUILD` so the same declarations export
/// rather than import.
#if defined(_WIN32)
#if defined(STURDY_FFI_BUILD) && defined(STURDY_FFI_SHARED)
#define STURDY_ABI __declspec(dllexport)
#elif defined(STURDY_FFI_SHARED)
#define STURDY_ABI __declspec(dllimport)
#else
#define STURDY_ABI
#endif
#else
#if defined(STURDY_FFI_BUILD) || defined(STURDY_FFI_SHARED)
#define STURDY_ABI __attribute__((visibility("default")))
#else
#define STURDY_ABI
#endif
#endif

/// Calling convention for every ABI entry point and callback.
///
/// Deliberately empty. Every target in the engine's matrix is 64-bit, where each platform has
/// exactly one C calling convention (Windows x64, SysV AMD64, AAPCS64) — there is no `__cdecl` vs
/// `__stdcall` ambiguity to pin down, and no MinGW target exists that would reintroduce one. The
/// macro is spelled out anyway so that a future 32-bit or unusual target has one place to change.
#define STURDY_ABI_CALL

// ---------------------------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------------------------

/// Boolean with a pinned width.
///
/// `uint8_t` rather than `bool`/`_Bool` so the size is identical regardless of which language and
/// compiler is on the other side. Compare against zero rather than against `STURDY_TRUE`: any
/// nonzero value read out of the engine means true.
typedef uint8_t SturdyBool;

#define STURDY_FALSE ((SturdyBool)0)
#define STURDY_TRUE ((SturdyBool)1)

/// ABI version this header declares.
///
/// Major changes when an existing declaration changes meaning, layout, or disappears; minor
/// changes when declarations are appended. Check it at load time with
/// `sturdy_abi_version_major()` / `sturdy_abi_version_minor()` before calling anything else.
#define STURDY_ABI_VERSION_MAJOR 0u
#define STURDY_ABI_VERSION_MINOR 14u

// ---------------------------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------------------------

/// Status returned by every fallible entry point.
///
/// Each enum in this header carries an explicit `0x7fffffff` sentinel so the underlying type is a
/// full 32 bits on every compiler, rather than something a compiler may narrow to `char` or
/// `short` based on the values present.
typedef enum SturdyResult {
    /// The call succeeded.
    STURDY_OK = 0,
    /// A required pointer was null, or a value was outside its documented range.
    STURDY_ERROR_INVALID_ARGUMENT = 1,
    /// A handle was never valid, or carried the wrong kind.
    STURDY_ERROR_INVALID_HANDLE = 2,
    /// A handle was valid once but its callback scope has ended. See the handle scope rules above.
    STURDY_ERROR_HANDLE_EXPIRED = 3,
    /// A `struct_size` field did not match any layout this engine build understands.
    STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE = 4,
    /// The engine failed to initialize; the window or graphics device could not be created.
    STURDY_ERROR_INITIALIZATION_FAILED = 5,
    /// A caller-supplied callback reported failure.
    STURDY_ERROR_CALLBACK_FAILED = 6,
    /// A runtime is already executing in this process. Only one may run at a time.
    STURDY_ERROR_ALREADY_RUNNING = 7,
    /// An allocation failed.
    STURDY_ERROR_OUT_OF_MEMORY = 8,
    /// An unexpected internal failure. `sturdy_last_error_message()` carries the detail.
    STURDY_ERROR_INTERNAL = 9,
    /// A supplied text buffer was too small. The output length was still written; call again with
    /// a buffer of that size.
    STURDY_ERROR_BUFFER_TOO_SMALL = 10,
    /// The requested capability is not present on this device or in this build — a Vulkan handle
    /// query on a D3D12 device, or a native handle query when native access was not enabled.
    STURDY_ERROR_NOT_AVAILABLE = 11,
    /// An index was past the end of the collection being enumerated.
    STURDY_ERROR_OUT_OF_RANGE = 12,
    /// The entity was never valid, or has been destroyed. Expected in ordinary use — an entity can
    /// die between the frame you stored it and the frame you used it.
    STURDY_ERROR_ENTITY_NOT_ALIVE = 13,
    /// The entity does not carry the requested component.
    STURDY_ERROR_COMPONENT_MISSING = 14,
    /// The entity already carries that component.
    STURDY_ERROR_COMPONENT_PRESENT = 15,
    /// The component cannot be accessed as raw bytes because its C++ type is not trivially
    /// copyable. See `sturdy_ecs_component_info`.
    STURDY_ERROR_COMPONENT_NOT_BLITTABLE = 16,
    /// The world cannot be changed right now — an iteration is in progress on this thread, or a
    /// system schedule is running.
    STURDY_ERROR_BUSY = 17,
    STURDY_RESULT_FORCE_U32 = 0x7fffffff
} SturdyResult;

/// Detail for the most recent failing call **on the calling thread**.
///
/// @return A null-terminated UTF-8 string, never null; empty when the last call succeeded. The
///         pointer stays valid until the next ABI call on this same thread, so copy it if you
///         need to keep it. Never freed by the caller.
/// @note Thread-local by design: engine frames can run on their own render threads, so a
///       process-wide error slot would let one thread's failure overwrite another's.
STURDY_ABI const char *STURDY_ABI_CALL sturdy_last_error_message(void);

/// Major component of the ABI version this library implements.
STURDY_ABI uint32_t STURDY_ABI_CALL sturdy_abi_version_major(void);

/// Minor component of the ABI version this library implements.
STURDY_ABI uint32_t STURDY_ABI_CALL sturdy_abi_version_minor(void);

// ---------------------------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------------------------

/// Borrowed reference to the live engine, valid only inside the callback that received it.
///
/// A struct wrapping an integer rather than a raw pointer: the integer is a token that is minted
/// when a callback begins and revoked when it returns, and tokens are never reused. A stale token
/// therefore always resolves to "expired" instead of aliasing a later object.
typedef struct SturdyEngine {
    uint64_t token;
} SturdyEngine;

/// Borrowed reference to the render-frame parameters being built for the current frame.
///
/// Same scope and reuse rules as `SturdyEngine`. Valid only inside `request_render_frame`.
typedef struct SturdyFrame {
    uint64_t token;
} SturdyFrame;

/// Identifies the render surface (window) a frame is being produced for.
///
/// Opaque and informational in this ABI version: use it to tell surfaces apart across callbacks
/// when an application has multiple windows. It carries no lifetime of its own.
typedef struct SturdySurface {
    uint64_t id;
} SturdySurface;

// ---------------------------------------------------------------------------------------------
// Configuration enums
// ---------------------------------------------------------------------------------------------

/// Graphics backend to request.
///
/// `STURDY_BACKEND_DEFAULT` picks the platform's normal choice (D3D12 on Windows, Vulkan
/// elsewhere) and is what a portable caller should use. Requesting a backend the running platform
/// does not build — D3D12 anywhere but Windows — fails initialization rather than silently
/// substituting one, so a mistake surfaces at startup instead of as a confusing later failure.
typedef enum SturdyBackend {
    STURDY_BACKEND_DEFAULT = 0,
    STURDY_BACKEND_VULKAN = 1,
    STURDY_BACKEND_D3D12 = 2,
    STURDY_BACKEND_FORCE_U32 = 0x7fffffff
} SturdyBackend;

/// Presentation synchronization policy.
typedef enum SturdyVSync {
    STURDY_VSYNC_OFF = 0,
    STURDY_VSYNC_ON = 1,
    /// Tears rather than stalling when a frame misses the refresh deadline.
    STURDY_VSYNC_ADAPTIVE = 2,
    STURDY_VSYNC_FORCE_U32 = 0x7fffffff
} SturdyVSync;

/// Render-graph stages that can be toggled per frame.
typedef enum SturdyRenderFeature {
    STURDY_RENDER_FEATURE_SCENE = 0,
    STURDY_RENDER_FEATURE_SHADOWS = 1,
    STURDY_RENDER_FEATURE_AMBIENT_OCCLUSION = 2,
    STURDY_RENDER_FEATURE_ANTI_ALIASING = 3,
    STURDY_RENDER_FEATURE_BLOOM = 4,
    STURDY_RENDER_FEATURE_TONE_MAPPING = 5,
    STURDY_RENDER_FEATURE_DEBUG_OVERLAY = 6,
    STURDY_RENDER_FEATURE_RESTIR_GI = 7,
    STURDY_RENDER_FEATURE_MOTION_BLUR = 8,
    STURDY_RENDER_FEATURE_FORCE_U32 = 0x7fffffff
} SturdyRenderFeature;

/// Tone-mapping operator applied to the scene's HDR output.
typedef enum SturdyToneMapping {
    STURDY_TONE_MAPPING_NONE = 0,
    STURDY_TONE_MAPPING_REINHARD = 1,
    STURDY_TONE_MAPPING_EXPONENTIAL = 2,
    STURDY_TONE_MAPPING_AGX = 3,
    STURDY_TONE_MAPPING_HERMITE_SPLINE = 4,
    STURDY_TONE_MAPPING_PSYCHO_V = 5,
    STURDY_TONE_MAPPING_FORCE_U32 = 0x7fffffff
} SturdyToneMapping;

/// Mouse buttons, matching the engine's own ordering.
typedef enum SturdyMouseButton {
    STURDY_MOUSE_BUTTON_UNKNOWN = 0,
    STURDY_MOUSE_BUTTON_LEFT = 1,
    STURDY_MOUSE_BUTTON_MIDDLE = 2,
    STURDY_MOUSE_BUTTON_RIGHT = 3,
    STURDY_MOUSE_BUTTON_EXTRA1 = 4,
    STURDY_MOUSE_BUTTON_EXTRA2 = 5,
    STURDY_MOUSE_BUTTON_FORCE_U32 = 0x7fffffff
} SturdyMouseButton;

// ---------------------------------------------------------------------------------------------
// Frame input
// ---------------------------------------------------------------------------------------------

/// Per-frame timing and surface geometry, supplied by the engine to `request_render_frame`.
///
/// Owned by the engine and valid only for the duration of that callback.
typedef struct SturdyFrameInput {
    /// Set by the engine to `sizeof(SturdyFrameInput)` as this engine build sees it. Compare
    /// against your own `sizeof` before reading fields your binding added later.
    uint32_t struct_size;
    uint32_t reserved;
    /// Wall-clock seconds since the previous frame on this surface.
    double delta_seconds;
    /// Monotonically increasing per-surface frame counter.
    uint64_t frame_index;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    /// Nonzero while the user is actively dragging the window edge. Frames produced during a live
    /// resize are on a latency-sensitive path; keep work in the callback minimal.
    SturdyBool live_resize;
    uint8_t reserved_tail[7];
} SturdyFrameInput;

// ---------------------------------------------------------------------------------------------
// Game logic
// ---------------------------------------------------------------------------------------------

/// Callbacks implementing your application, mirroring the C++ `Engine::GameLogic` interface.
///
/// Every callback may be null; a null callback behaves as a no-op that neither fails nor renders.
/// `user_data` is passed back to each callback untouched and is never interpreted by the engine.
///
/// @note Callbacks must not unwind. See the boundary rules at the top of this header.
/// @note `request_render_frame` may be invoked from a dedicated per-window render thread, not the
///       thread that called `sturdy_runtime_run`. `on_engine_initialized` and `on_shutdown` are
///       always called on the runtime thread.
typedef struct SturdyGameLogic {
    /// Set to `sizeof(SturdyGameLogic)`.
    uint32_t struct_size;
    uint32_t reserved;

    /// Opaque pointer handed back to every callback below.
    void *user_data;

    /// Called once after the engine and its first window are live, before any frame.
    ///
    /// @return `STURDY_TRUE` to continue startup; `STURDY_FALSE` to abort it, which surfaces as
    ///         `STURDY_ERROR_CALLBACK_FAILED` from `sturdy_runtime_run`.
    SturdyBool(STURDY_ABI_CALL *on_engine_initialized)(SturdyEngine engine, void *user_data);

    /// Called once per frame per surface to describe what should be drawn.
    ///
    /// Configure `frame` with the `sturdy_frame_*` setters, then return `STURDY_TRUE` to render
    /// it. Return `STURDY_FALSE` to skip this frame entirely — the correct answer when there is
    /// nothing new to show, since it presents no frame rather than presenting a blank one.
    ///
    /// `frame` arrives pre-populated with the engine's standard render pipeline and a camera whose
    /// aspect ratio already matches this surface, so a caller that configures nothing still
    /// produces a complete frame. Every setter overrides part of that starting point.
    SturdyBool(STURDY_ABI_CALL *request_render_frame)(SturdyEngine engine,
                                                      SturdySurface surface,
                                                      const SturdyFrameInput *input,
                                                      SturdyFrame frame,
                                                      void *user_data);

    /// Called once during teardown, before the engine is destroyed. Failures cannot be reported
    /// from here; shutdown proceeds regardless.
    void(STURDY_ABI_CALL *on_shutdown)(SturdyEngine engine, void *user_data);

    /// Called exactly once after `on_shutdown`, to release whatever `user_data` owns. This is the
    /// hook a binding uses to drop its language-side object (a boxed Rust struct, a pinned GC
    /// handle) at a well-defined point rather than leaking it.
    void(STURDY_ABI_CALL *destroy)(void *user_data);
} SturdyGameLogic;

// ---------------------------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------------------------

/// Application and window configuration for `sturdy_runtime_run`.
///
/// Populate with `sturdy_runtime_config_init` first — that sets `struct_size` and every default —
/// then override only the fields you care about. String fields are borrowed UTF-8 and must stay
/// alive for the duration of the `sturdy_runtime_run` call; the engine copies what it needs.
typedef struct SturdyRuntimeConfig {
    /// Set to `sizeof(SturdyRuntimeConfig)`. `sturdy_runtime_config_init` does this for you.
    uint32_t struct_size;
    uint32_t reserved;

    /// Application name reported to the graphics driver. Null selects the engine default.
    const char *app_name;
    /// Initial title of the primary window. Null selects the engine default.
    const char *window_title;
    /// Directory the engine loads shaders from, relative to the working directory unless
    /// absolute. Null selects `"Shaders"`.
    const char *shaders_directory;

    uint32_t window_width;
    uint32_t window_height;
    SturdyBool window_resizable;
    uint8_t reserved_window[3];

    SturdyBackend graphics_backend;
    SturdyVSync vsync;

    /// Request ray-tracing support at device creation. Initialization fails if the selected
    /// adapter cannot provide it, rather than silently continuing without — features that depend
    /// on it (ReSTIR GI) would otherwise fail confusingly at first use.
    SturdyBool enable_raytracing;
    /// Cache compiled shaders on disk between runs.
    SturdyBool enable_shader_disk_cache;
    /// Publish the backend's raw Vulkan or D3D12 objects, making the `sturdy_native_*` family
    /// usable. Off by default and opt-in on purpose: it is the one part of this ABI that hands out
    /// pointers the engine's own invariants do not cover, so a build that never asks for it cannot
    /// accidentally come to depend on native access.
    SturdyBool enable_native_access;
    uint8_t reserved_features[5];

    /// Selects a specific GPU by the stable identifier reported through
    /// `STURDY_ADAPTER_STRING_PHYSICAL_DEVICE_ID`. Null or empty lets the engine pick.
    ///
    /// This is the other half of persisting a user's GPU choice: read the id from a previous run's
    /// adapter, store it, and pass it back here. Identifiers agree across Vulkan and D3D12 for the
    /// same physical device, so a choice made under one backend still resolves under the other.
    ///
    /// An id that matches no device on this machine **fails startup** with
    /// `STURDY_ERROR_INITIALIZATION_FAILED` rather than quietly selecting a different GPU — the
    /// engine treats an explicit device request as a requirement, not a hint. Hardware does change
    /// between runs, so an application persisting this value should be prepared to clear it and
    /// retry with null once, instead of treating the failure as fatal.
    const char *physical_device_id;
} SturdyRuntimeConfig;

/// Fills `config` with engine defaults and the correct `struct_size`.
///
/// Always call this before setting fields. It is what makes a binding compiled against an older
/// header keep working against a newer engine: fields you have never heard of still get sane
/// values instead of whatever was on your stack.
///
/// @param config Destination. Must not be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_runtime_config_init(SturdyRuntimeConfig *config);

/// Runs the application to completion: creates the window and graphics device, drives the frame
/// loop, and returns once the last window closes.
///
/// Blocks the calling thread for the entire lifetime of the application. Only one runtime may be
/// active per process; a concurrent or nested call fails with `STURDY_ERROR_ALREADY_RUNNING`
/// rather than corrupting the first one's state.
///
/// @param config Configuration, previously initialized by `sturdy_runtime_config_init`.
/// @param game_logic Your callbacks. Must not be null, though its individual callbacks may be.
///        `destroy` is invoked before this function returns, on every path including failure.
/// @param argc Number of command-line arguments in `argv`. Pass 0 with a null `argv` if you have
///        none; the engine parses its own recognized flags from these.
/// @param argv Command-line arguments as null-terminated UTF-8. May be null only when `argc` is 0.
/// @param out_exit_code Receives the application's own exit code. Written whenever the runtime
///        actually ran, including when this function goes on to return
///        `STURDY_ERROR_CALLBACK_FAILED`. May be null.
///
/// @return `STURDY_OK` when the runtime ran to completion — check `out_exit_code` for the
///         application's own status, which is nonzero if it terminated unhappily.
///         `STURDY_ERROR_CALLBACK_FAILED` when one of your callbacks reported failure, and
///         `STURDY_ERROR_INITIALIZATION_FAILED` when the engine never came up at all (no window,
///         no graphics device, or a `physical_device_id` this machine does not have). Those three
///         are kept distinct so a binding can tell its own rejection apart from an engine failure
///         apart from an ordinary nonzero exit. Argument and configuration problems are reported
///         before anything starts.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_runtime_run(const SturdyRuntimeConfig *config,
                                                           const SturdyGameLogic *game_logic,
                                                           int32_t argc,
                                                           const char *const *argv,
                                                           int32_t *out_exit_code);

// ---------------------------------------------------------------------------------------------
// Frame parameters
// ---------------------------------------------------------------------------------------------

/// Places the camera at a world-space position.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_position(SturdyFrame frame,
                                                                         float x,
                                                                         float y,
                                                                         float z);

/// Points the camera at a world-space target, keeping `+Y` as up.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_camera_look_at(SturdyFrame frame,
                                                                    float x,
                                                                    float y,
                                                                    float z);

/// Sets a perspective projection.
///
/// @param vertical_fov_degrees Vertical field of view. Must be in (0, 180).
/// @param near_clip Near plane distance. Must be greater than 0 and less than `far_clip`.
/// @param far_clip Far plane distance.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_perspective(
    SturdyFrame frame, float vertical_fov_degrees, float near_clip, float far_clip);

/// Sets the camera's aspect ratio from a viewport size in pixels.
///
/// Pass the `framebuffer_width`/`framebuffer_height` from this frame's `SturdyFrameInput` unless
/// you are deliberately rendering at a different aspect. Both must be nonzero.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_viewport(SturdyFrame frame,
                                                                         uint32_t width,
                                                                         uint32_t height);

/// Sets uniform ambient radiance and the scene exposure multiplier.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_ambient_light(
    SturdyFrame frame, float red, float green, float blue, float exposure);

/// Sets the color the scene is cleared to, as linear RGBA.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_background_color(
    SturdyFrame frame, float red, float green, float blue, float alpha);

/// Enables or disables one render-graph stage for this frame.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_feature_enabled(SturdyFrame frame,
                                                                         SturdyRenderFeature feature,
                                                                         SturdyBool enabled);

/// Selects the tone-mapping operator and its exposure/white-point/saturation controls.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_tone_mapping(SturdyFrame frame,
                                                                      SturdyToneMapping operation,
                                                                      float exposure,
                                                                      float white_point,
                                                                      float saturation);

/// Scales the internal render resolution relative to the surface. Must be in (0, 2].
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_resolution_scale(SturdyFrame frame,
                                                                          float scale);

/// Attaches a debug label to this frame, surfaced in graphics debuggers and engine logs.
///
/// @param label Null-terminated UTF-8, or null to clear. Copied; need not outlive the call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_debug_label(SturdyFrame frame,
                                                                     const char *label);

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

/// Reports whether a key is currently held.
///
/// @param key Engine key code. Printable ASCII maps to its own character value; see the engine's
///        `WindowManager::KeyboardKey` for the full set. Out-of-range codes report not-held rather
///        than failing, so a caller mapping from another platform's key enum cannot fault here.
/// @param out_down Receives nonzero when held. Must not be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_key_down(SturdyEngine engine,
                                                               int32_t key,
                                                               SturdyBool *out_down);

/// Reports whether a key transitioned to held during this tick.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_key_just_pressed(SturdyEngine engine,
                                                                       int32_t key,
                                                                       SturdyBool *out_pressed);

/// Reports whether a key transitioned to released during this tick.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_key_just_released(SturdyEngine engine,
                                                                        int32_t key,
                                                                        SturdyBool *out_released);

/// Reports whether a mouse button is currently held.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_down(SturdyEngine engine,
                                                                 SturdyMouseButton button,
                                                                 SturdyBool *out_down);

/// Reads the cursor position in window coordinates.
///
/// @param out_x Receives the horizontal position. May be null.
/// @param out_y Receives the vertical position. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_position(SturdyEngine engine,
                                                                     float *out_x,
                                                                     float *out_y);

/// Reads cursor movement accumulated during this tick.
///
/// This is the value to integrate for camera look, not the difference between successive
/// `sturdy_engine_mouse_position` reads: it accumulates every motion event the platform delivered
/// this tick, so it stays correct at high polling rates where several events land per frame.
///
/// @param out_x Receives horizontal movement. May be null.
/// @param out_y Receives vertical movement. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_delta(SturdyEngine engine,
                                                                  float *out_x,
                                                                  float *out_y);

/// Reads scroll-wheel movement accumulated during this tick.
///
/// @param out_x Receives horizontal scroll. May be null.
/// @param out_y Receives vertical scroll. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_engine_wheel_delta(SturdyEngine engine,
                                                                  float *out_x,
                                                                  float *out_y);

// ---------------------------------------------------------------------------------------------
// ECS
// ---------------------------------------------------------------------------------------------

/// Identifies an entity.
///
/// The `generation` half is what makes a stale handle safe: entity indices are recycled after a
/// destroy, so an entity value kept from an earlier frame can name a live row belonging to someone
/// else. Every call below checks the generation and reports `STURDY_ERROR_ENTITY_NOT_ALIVE` rather
/// than acting on the wrong entity. A zeroed `SturdyEntity` is never alive.
typedef struct SturdyEntity {
    uint32_t index;
    uint32_t generation;
} SturdyEntity;

/// Identifies a resource or event type. Derived from its canonical name and stable across runs.
typedef struct SturdyResourceId {
    uint64_t high;
    uint64_t low;
} SturdyResourceId;

/// Identifies a component type within the engine's registry. Stable for the process lifetime.
typedef uint32_t SturdyComponentId;

/// One component's initial value, for `sturdy_ecs_spawn`.
typedef struct SturdyComponentInit {
    SturdyComponentId component;
    uint32_t size;
    /// Bytes to copy in. Must address at least `size` bytes, and `size` must equal the component's
    /// registered size.
    const void *data;
} SturdyComponentInit;

/// What the engine knows about a registered component.
typedef struct SturdyComponentInfo {
    /// Set by the engine to `sizeof(SturdyComponentInfo)` as this build sees it.
    uint32_t struct_size;
    SturdyComponentId component;
    uint32_t size;
    uint32_t align;
    uint32_t schema_version;
    /// Whether this component's bytes can be copied through `sturdy_ecs_get_component` and friends.
    /// False for engine components whose C++ type owns heap memory; those can still be matched in
    /// `sturdy_ecs_for_each` but not copied.
    SturdyBool blittable;
    /// Whether this component carries no data and exists only to mark entities. Tags report a
    /// `size` of 1, matching what an empty struct occupies in C++.
    SturdyBool is_tag;
    uint8_t reserved[2];
} SturdyComponentInfo;

/// Receives one matching entity during `sturdy_ecs_for_each`.
///
/// `components` points at an array parallel to the component ids you asked to match, each entry
/// addressing that component's storage for this entity. Write through them to mutate in place.
/// The pointers are valid only for the duration of this call.
///
/// While a visit is in progress the world is locked, so this callback must **not** call any other
/// `sturdy_ecs_*` function; each returns `STURDY_ERROR_BUSY`. That guard is doing real work — the
/// engine underneath treats a structural change made during iteration as a fatal contract
/// violation, so without it a reentrant spawn would terminate the process rather than fail.
/// Read and write through `components`; if you need to spawn or destroy, record what you want and
/// do it after `sturdy_ecs_for_each` returns.
typedef void(STURDY_ABI_CALL *SturdyEcsVisitFn)(SturdyEntity entity,
                                                void **components,
                                                void *user_data);

/// Registers a plain-data component type, or returns the existing id if one is already registered
/// under that name.
///
/// The component is treated as raw bytes: copied with the equivalent of `memcpy` and destroyed with
/// no cleanup. That is why only plain data works here — a foreign caller has no way to supply C++
/// construction and destruction semantics, so anything owning a resource must be defined in C++.
///
/// @param name Canonical, globally unique name. Use a namespaced form such as `"mygame.health"`;
///        the engine derives a stable key from it, so it must not collide with another component.
/// @param size Bytes per instance. Must be nonzero; use `sturdy_ecs_register_tag_component` for a
///        component that carries no data.
/// @param align Required alignment in bytes. Must be a power of two and at most 64.
/// @param out_component Receives the id.
///
/// @return `STURDY_OK`. Re-registering the same name with the same layout returns the original id;
///         re-registering it with a *different* layout is rejected, since existing storage and
///         existing entities are already laid out the old way.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_register_component(SturdyEngine engine,
                                                                      const char *name,
                                                                      uint32_t size,
                                                                      uint32_t align,
                                                                      SturdyComponentId *out_component);

/// Registers a tag component: one that marks an entity without carrying data.
///
/// Tags are for querying — "is this entity a player", "is this one asleep" — where the answer is
/// the presence of the component itself. Add and remove them with `sturdy_ecs_add_tag` and
/// `sturdy_ecs_remove_component`, and match them in `sturdy_ecs_for_each` like any other component.
///
/// A tag occupies **one byte**, not zero, because that is what an empty struct occupies in C++ and
/// this ABI does not invent a different layout. The pointer a tag yields during iteration is valid
/// but meaningless; there is nothing to read from it.
///
/// @param name Canonical, globally unique name.
/// @param out_component Receives the id.
///
/// @return `STURDY_OK`. Re-registering an existing tag returns the original id.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_register_tag_component(SturdyEngine engine,
                                                                          const char *name,
                                                                          SturdyComponentId *out_component);

/// Attaches a tag component to an entity.
///
/// A convenience over `sturdy_ecs_add_component` that supplies the byte for you, since a tag has no
/// value to pass.
///
/// @return `STURDY_ERROR_COMPONENT_PRESENT` if the entity already carries it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_add_tag(SturdyEngine engine,
                                                           SturdyEntity entity,
                                                           SturdyComponentId component);

/// Looks up a component id by canonical name.
///
/// Also the way to reach the engine's own components — `"sturdy.engine.world_transform"` and the
/// rest — without registering anything.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when nothing is registered under that name.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_find_component(SturdyEngine engine,
                                                                  const char *name,
                                                                  SturdyComponentId *out_component);

/// Reads what the engine knows about a component.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_info(SturdyEngine engine,
                                                                  SturdyComponentId component,
                                                                  SturdyComponentInfo *out_info);

/// Reads a component's canonical name. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_name(SturdyEngine engine,
                                                                  SturdyComponentId component,
                                                                  char *buffer,
                                                                  size_t capacity,
                                                                  size_t *out_length);

/// Creates an entity carrying the supplied components.
///
/// @param components Initial value for each component. Must be non-empty and free of duplicates —
///        an entity must carry at least one component.
/// @param component_count How many entries `components` has.
/// @param out_entity Receives the new entity.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_spawn(SturdyEngine engine,
                                                         const SturdyComponentInit *components,
                                                         uint32_t component_count,
                                                         SturdyEntity *out_entity);

/// Destroys an entity. Destroying an already-dead entity is a no-op, not an error, so cleanup code
/// does not have to track whether it already ran.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_destroy(SturdyEngine engine, SturdyEntity entity);

/// Reports whether an entity is alive.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_is_alive(SturdyEngine engine,
                                                            SturdyEntity entity,
                                                            SturdyBool *out_alive);

/// Reports whether an entity carries a component. A dead entity reports false rather than failing.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_has_component(SturdyEngine engine,
                                                                 SturdyEntity entity,
                                                                 SturdyComponentId component,
                                                                 SturdyBool *out_present);

/// Attaches a component to an existing entity.
///
/// @param size Must equal the component's registered size.
/// @return `STURDY_ERROR_COMPONENT_PRESENT` if the entity already carries it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_add_component(SturdyEngine engine,
                                                                 SturdyEntity entity,
                                                                 SturdyComponentId component,
                                                                 const void *data,
                                                                 uint32_t size);

/// Detaches a component from an entity.
///
/// @return `STURDY_ERROR_COMPONENT_MISSING` if the entity does not carry it, or
///         `STURDY_ERROR_INVALID_ARGUMENT` if it is the entity's last component — destroy the
///         entity instead of emptying it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_remove_component(SturdyEngine engine,
                                                                    SturdyEntity entity,
                                                                    SturdyComponentId component);

/// Copies a component's current value out.
///
/// A copy rather than a pointer: component storage moves when entities gain or lose components, so
/// a pointer handed across the boundary would go stale in a way the caller could not detect.
///
/// @param size Must equal the component's registered size. A mismatch is rejected rather than
///        partially copied, because it means your struct and the engine's disagree.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_get_component(SturdyEngine engine,
                                                                 SturdyEntity entity,
                                                                 SturdyComponentId component,
                                                                 void *out_data,
                                                                 uint32_t size);

/// Overwrites a component's value in place.
///
/// @param size Must equal the component's registered size.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_set_component(SturdyEngine engine,
                                                                 SturdyEntity entity,
                                                                 SturdyComponentId component,
                                                                 const void *data,
                                                                 uint32_t size);

/// Visits every live entity carrying all of the listed components.
///
/// This is the query primitive: the engine walks only the archetypes that match, so cost scales
/// with matching entities rather than with the whole world.
///
/// @param components Components an entity must all carry to be visited. At most 16.
/// @param component_count How many entries `components` has.
/// @param visit Invoked once per matching entity. Must not unwind, and must not call back into
///        `sturdy_ecs_*` — see `SturdyEcsVisitFn`.
/// @param user_data Passed through to `visit` untouched.
/// @param out_visited Receives how many entities were visited. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_for_each(SturdyEngine engine,
                                                            const SturdyComponentId *components,
                                                            uint32_t component_count,
                                                            SturdyEcsVisitFn visit,
                                                            void *user_data,
                                                            uint32_t *out_visited);

/// How a system intends to touch one component.
typedef enum SturdyAccessMode {
    /// The system reads the component but never writes it. Two systems that only read the same
    /// component can run at the same time.
    STURDY_ACCESS_READ = 0,
    /// The system writes the component. Nothing else touching it may run concurrently.
    STURDY_ACCESS_WRITE = 1,
    STURDY_ACCESS_FORCE_U32 = 0x7fffffff
} SturdyAccessMode;

/// One component a system operates on, and how.
typedef struct SturdySystemAccess {
    SturdyComponentId component;
    SturdyAccessMode mode;
} SturdySystemAccess;

/// Queues structural changes from inside a system. Opaque; valid only for the callback that
/// received it.
typedef struct SturdyCommands {
    uint64_t token;
} SturdyCommands;

/// Body of a system registered with `sturdy_ecs_add_system`.
///
/// `components` points at an array parallel to the access list the system was registered with,
/// each entry addressing that component's storage for this entity. Write through the ones declared
/// `STURDY_ACCESS_WRITE`; the pointers are valid only for this call.
///
/// `commands` queues spawns, destroys and component changes for the engine to apply once the stage
/// finishes. That indirection is not optional — the world cannot be restructured while systems are
/// reading it, which is why the direct `sturdy_ecs_spawn` family reports `STURDY_ERROR_BUSY` here.
typedef void(STURDY_ABI_CALL *SturdySystemFn)(SturdyEntity entity,
                                              void **components,
                                              SturdyCommands commands,
                                              void *user_data);

/// Registers a system that runs every frame, once per entity carrying all the listed components.
///
/// The access list is what makes automatic parallelism safe. A C++ system declares its access
/// through its parameter types and the engine derives the rest; a function pointer carries no such
/// information, so you declare it here. The scheduler then orders systems so that nothing writing a
/// component runs alongside anything else touching it, and lets non-conflicting systems run
/// together.
///
/// **Declaring less access than you take is the one way to get data races.** If a system might
/// write a component, declare `STURDY_ACCESS_WRITE` even when it usually only reads — an
/// over-declared system costs parallelism, an under-declared one costs correctness.
///
/// Systems stay registered for the life of the engine; there is no removal yet.
///
/// @param access Components the system operates on and how. Must be non-empty, at most 16, and
///        free of duplicates.
/// @param access_count How many entries `access` has.
/// @param system Body to run per entity. Must not unwind.
/// @param user_data Passed through to `system` untouched.
///
/// @return `STURDY_OK` once registered. The system first runs on the next frame.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_add_system(SturdyEngine engine,
                                                              const SturdySystemAccess *access,
                                                              uint32_t access_count,
                                                              SturdySystemFn system,
                                                              void *user_data);

/// One resource or event channel a system operates on, and how.
typedef struct SturdySystemResourceAccess {
    SturdyResourceId resource;
    SturdyAccessMode mode;
    uint32_t reserved;
} SturdySystemResourceAccess;

/// Registers a system that also touches resources or event channels.
///
/// The component half behaves exactly as `sturdy_ecs_add_system`. The resource half is what lets
/// the scheduler order a foreign system against systems — C++ or otherwise — that use the same
/// resources: two systems writing the same resource will not run concurrently, and a reader will
/// not overlap a writer.
///
/// Passing zero components registers a **global** system: with nothing to iterate, the body runs
/// exactly once per frame, with a default-constructed entity and a null component array. That is
/// the right shape for a system that only drains an event channel or advances a resource.
///
/// @param access Components the system operates on. May be null when `access_count` is zero.
/// @param access_count How many entries `access` has. At most 16.
/// @param resource_access Resources and event channels the system operates on. May be null when
///        `resource_access_count` is zero.
/// @param resource_access_count How many entries `resource_access` has.
/// @param system Body to run. Must not unwind.
/// @param user_data Passed through to `system` untouched.
STURDY_ABI SturdyResult STURDY_ABI_CALL
sturdy_ecs_add_system_with_resources(SturdyEngine engine,
                                     const SturdySystemAccess *access,
                                     uint32_t access_count,
                                     const SturdySystemResourceAccess *resource_access,
                                     uint32_t resource_access_count,
                                     SturdySystemFn system,
                                     void *user_data);

/// Queues creation of an entity, applied after the current stage finishes.
///
/// @param commands Handle supplied to your system.
/// @param components Initial value for each component; copied immediately, so your storage need
///        not outlive this call.
/// @param component_count How many entries `components` has.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_spawn(SturdyCommands commands,
                                                                  const SturdyComponentInit *components,
                                                                  uint32_t component_count);

/// Queues destruction of an entity, applied after the current stage finishes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_destroy(SturdyCommands commands,
                                                                    SturdyEntity entity);

/// Queues attaching a component, applied after the current stage finishes.
///
/// @param size Must equal the component's registered size.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_add_component(SturdyCommands commands,
                                                                          SturdyEntity entity,
                                                                          SturdyComponentId component,
                                                                          const void *data,
                                                                          uint32_t size);

/// Queues detaching a component, applied after the current stage finishes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_commands_remove_component(SturdyCommands commands,
                                                                             SturdyEntity entity,
                                                                             SturdyComponentId component);

// ---------------------------------------------------------------------------------------------
// ECS resources and events
// ---------------------------------------------------------------------------------------------

/// Derives the identifier for a canonical resource or event name.
///
/// Pure and engine-independent, so a binding can compute its ids once at load time. The same name
/// always yields the same id, which is what lets a foreign caller name a resource the engine's own
/// C++ code also uses.
///
/// @param name Canonical, globally unique name such as `"mygame.score"`.
/// @param out_id Receives the identifier.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_resource_id(const char *name,
                                                               SturdyResourceId *out_id);

/// Creates a resource, taking ownership of its storage.
///
/// The engine's own resources are bound by reference to storage their C++ owner keeps alive. A
/// foreign caller has no comparable place to put that storage, so the engine allocates and owns it
/// here, zero-initialized or copied from `initial_data`. It lives until
/// `sturdy_ecs_destroy_resource` or process exit.
///
/// Binding a name that already exists rebinds nothing and succeeds, provided the size matches; a
/// different size under the same name is rejected, because anything already reading it expects the
/// old layout.
///
/// @param name Canonical, globally unique name.
/// @param size Byte size. Must be nonzero.
/// @param initial_data Initial value, or null to zero-initialize.
/// @param out_id Receives the resource identifier. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_create_resource(SturdyEngine engine,
                                                                   const char *name,
                                                                   uint32_t size,
                                                                   const void *initial_data,
                                                                   SturdyResourceId *out_id);

/// Destroys a resource created through `sturdy_ecs_create_resource` and frees its storage.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when nothing is bound under that id, or when it is bound to
///         storage this ABI does not own — the engine's own C++ resources cannot be destroyed here.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_destroy_resource(SturdyEngine engine,
                                                                    SturdyResourceId resource);

/// Reports whether a resource is bound.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_has_resource(SturdyEngine engine,
                                                                SturdyResourceId resource,
                                                                SturdyBool *out_present);

/// Copies a resource's current value out.
///
/// @param size Must equal the resource's byte size.
/// @return `STURDY_ERROR_NOT_AVAILABLE` when nothing is bound under that id.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_get_resource(SturdyEngine engine,
                                                                SturdyResourceId resource,
                                                                void *out_data,
                                                                uint32_t size);

/// Overwrites a resource's value.
///
/// @param size Must equal the resource's byte size.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_set_resource(SturdyEngine engine,
                                                                SturdyResourceId resource,
                                                                const void *data,
                                                                uint32_t size);

/// Creates an event channel: a buffer of fixed-size records the engine drains every frame.
///
/// Events differ from resources in exactly that draining. Anything sent during a frame is readable
/// for the rest of that frame and gone by the next, which is what makes them the right way to
/// communicate between systems without either one owning the data.
///
/// @param name Canonical, globally unique name such as `"mygame.damage_event"`.
/// @param event_size Byte size of one event. Must be nonzero.
/// @param out_id Receives the channel identifier. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_create_event_channel(SturdyEngine engine,
                                                                        const char *name,
                                                                        uint32_t event_size,
                                                                        SturdyResourceId *out_id);

/// Appends one event to a channel.
///
/// @param size Must equal the channel's event size.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_send_event(SturdyEngine engine,
                                                              SturdyResourceId channel,
                                                              const void *event,
                                                              uint32_t size);

/// Returns how many events are currently buffered on a channel.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_event_count(SturdyEngine engine,
                                                               SturdyResourceId channel,
                                                               uint32_t *out_count);

/// Copies one buffered event out of a channel.
///
/// @param index Event to read, less than `sturdy_ecs_event_count`.
/// @param size Must equal the channel's event size.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_read_event(SturdyEngine engine,
                                                              SturdyResourceId channel,
                                                              uint32_t index,
                                                              void *out_event,
                                                              uint32_t size);

/// Discards every event currently buffered on a channel.
///
/// Rarely needed: the engine drains channels once per frame on its own. Use this when a channel is
/// filled and consumed entirely within one frame and should not be seen again.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_clear_events(SturdyEngine engine,
                                                                SturdyResourceId channel);

// ---------------------------------------------------------------------------------------------
// Rendering: assets and scene
// ---------------------------------------------------------------------------------------------

/// An engine asset — a shader, model or texture.
///
/// Opaque by value: the bytes are the engine's own asset handle, copied in and out rather than
/// interpreted. Copy it freely, compare it with `memcmp`, and treat a zeroed value as invalid. It
/// stays valid for as long as the engine that produced it.
typedef struct SturdyAsset {
    uint64_t opaque[4];
} SturdyAsset;

/// Which built-in shape `sturdy_render_create_shape_model` generates.
///
/// Each shape reads only the fields of `SturdyShapeParams` documented for it and ignores the rest,
/// so one struct covers all of them without a union the ABI would have to version.
typedef enum SturdyShape {
    /// Uses `size`.
    STURDY_SHAPE_CUBE = 0,
    /// Uses `extents_x`, `extents_y`, `extents_z`.
    STURDY_SHAPE_BOX = 1,
    /// Uses `radius`, `rings`, `segments`.
    STURDY_SHAPE_UV_SPHERE = 2,
    /// Uses `radius`, `subdivisions`.
    STURDY_SHAPE_ICO_SPHERE = 3,
    /// Uses `width`, `depth`, `width_segments`, `depth_segments`.
    STURDY_SHAPE_PLANE = 4,
    /// Uses `radius`, `height`, `radial_segments`, `capped`.
    STURDY_SHAPE_CYLINDER = 5,
    /// Uses `radius`, `height`, `radial_segments`, `capped`.
    STURDY_SHAPE_CONE = 6,
    /// Uses `major_radius`, `minor_radius`, `major_segments`, `minor_segments`.
    STURDY_SHAPE_TORUS = 7,
    /// Uses `size`.
    STURDY_SHAPE_TETRAHEDRON = 8,
    STURDY_SHAPE_FORCE_U32 = 0x7fffffff
} SturdyShape;

/// Dimensions for a built-in shape. Only the fields listed for the chosen shape are read.
///
/// Initialize with `sturdy_render_shape_params_init`, which fills the same defaults the engine's
/// C++ primitives use, then override what you care about.
typedef struct SturdyShapeParams {
    /// Set to `sizeof(SturdyShapeParams)`.
    uint32_t struct_size;
    uint32_t reserved;

    float size;
    float radius;
    float height;
    float width;
    float depth;
    float major_radius;
    float minor_radius;
    float extents_x;
    float extents_y;
    float extents_z;

    uint32_t rings;
    uint32_t segments;
    uint32_t subdivisions;
    uint32_t radial_segments;
    uint32_t width_segments;
    uint32_t depth_segments;
    uint32_t major_segments;
    uint32_t minor_segments;

    /// Whether cylinders and cones get end caps.
    SturdyBool capped;
    uint8_t reserved_tail[3];
} SturdyShapeParams;

/// One vertex of a custom mesh. Matches the engine's own vertex layout field for field.
typedef struct SturdyVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[4];
    /// Tangent, with handedness in `w`.
    float tangent[4];
} SturdyVertex;

/// Fills `params` with the engine's default shape dimensions.
///
/// @param params Destination. Must not be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_shape_params_init(SturdyShapeParams *params);

/// Loads a shader from a Slang source file.
///
/// A model needs a shader, and the engine's standard one is `"Shaders/gbuffer_geometry.slang"` with
/// a depth-only entry point of `"depthOnlyMain"` — that pairing is what the deferred renderer
/// expects, so pass it unless you have written your own.
///
/// @param source Path to the shader source, relative to the working directory unless absolute.
/// @param depth_only_entry_point Depth-only fragment entry point, or null for none.
/// @param out_shader Receives the shader asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_load_shader(SturdyEngine engine,
                                                                  const char *source,
                                                                  const char *depth_only_entry_point,
                                                                  SturdyAsset *out_shader);

/// Creates a renderable model from a built-in shape.
///
/// @param shape Which shape to generate.
/// @param params Dimensions, previously initialized by `sturdy_render_shape_params_init`.
/// @param shader Shader the model draws with.
/// @param label Debug label, or null. Surfaced in graphics debuggers and engine logs.
/// @param out_model Receives the model asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_create_shape_model(SturdyEngine engine,
                                                                         SturdyShape shape,
                                                                         const SturdyShapeParams *params,
                                                                         SturdyAsset shader,
                                                                         const char *label,
                                                                         SturdyAsset *out_model);

/// Creates a renderable model from your own geometry.
///
/// @param vertices Vertex array. Must not be null.
/// @param vertex_count Number of vertices.
/// @param indices Triangle indices, three per triangle. Every index must be less than
///        `vertex_count`; this is checked, because an out-of-range index would otherwise read past
///        the vertex buffer on the GPU.
/// @param index_count Number of indices. Must be a multiple of three.
/// @param shader Shader the model draws with.
/// @param label Debug label, or null.
/// @param out_model Receives the model asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_create_mesh_model(SturdyEngine engine,
                                                                        const SturdyVertex *vertices,
                                                                        uint32_t vertex_count,
                                                                        const uint32_t *indices,
                                                                        uint32_t index_count,
                                                                        SturdyAsset shader,
                                                                        const char *label,
                                                                        SturdyAsset *out_model);

/// Sets a scalar material parameter on one of a model's primitives.
///
/// @param primitive Which primitive to affect; 0 for a single-shape model.
/// @param name Parameter name as the shader declares it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_model_float(SturdyEngine engine,
                                                                      SturdyAsset model,
                                                                      uint32_t primitive,
                                                                      const char *name,
                                                                      float value);

/// Sets a four-component material parameter on one of a model's primitives.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_model_vec4(SturdyEngine engine,
                                                                     SturdyAsset model,
                                                                     uint32_t primitive,
                                                                     const char *name,
                                                                     float x,
                                                                     float y,
                                                                     float z,
                                                                     float w);

/// Binds a texture to a named slot on one of a model's primitives.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_model_texture(SturdyEngine engine,
                                                                        SturdyAsset model,
                                                                        uint32_t primitive,
                                                                        const char *slot,
                                                                        SturdyAsset texture);

/// Loads a texture from an encoded image file (PNG, JPEG, and the rest the engine decodes).
///
/// @param source Path to the image file.
/// @param srgb Nonzero to treat the contents as sRGB, which is right for color maps and wrong for
///        normal and roughness maps.
/// @param out_texture Receives the texture asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_load_texture(SturdyEngine engine,
                                                                   const char *source,
                                                                   SturdyBool srgb,
                                                                   SturdyAsset *out_texture);

/// Reports how much geometry a model asset holds.
///
/// @param out_primitives Receives the primitive count. May be null.
/// @param out_vertices Receives the total vertex count. May be null.
/// @param out_triangles Receives the total triangle count. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_model_info(SturdyEngine engine,
                                                                 SturdyAsset model,
                                                                 uint32_t *out_primitives,
                                                                 uint32_t *out_vertices,
                                                                 uint32_t *out_triangles);

// ---------------------------------------------------------------------------------------------
// Rendering: scene composition
// ---------------------------------------------------------------------------------------------

/// Creates an entity ready to be rendered, carrying an identity transform.
///
/// The natural way to start a renderable: an entity must be spawned with at least one component,
/// and the engine's own components register themselves lazily the first time something uses them —
/// so looking `sturdy.engine.world_transform` up by name before anything has touched it reports
/// `STURDY_ERROR_NOT_AVAILABLE`. This registers it and spawns in one step.
///
/// Follow with `sturdy_render_set_transform` to place it and `sturdy_render_set_model` or one of
/// the light setters to give it something to contribute.
///
/// @param out_entity Receives the new entity.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_spawn(SturdyEngine engine,
                                                            SturdyEntity *out_entity);

/// Gives an entity a world transform.
///
/// Rendering is driven from the ECS: an entity with a transform and a model is drawn, an entity
/// with a transform and a light lights the scene. These helpers write the engine's own components
/// rather than making you reproduce their memory layout — which you could do through
/// `sturdy_ecs_add_component`, but only by hard-coding a layout that is free to change.
///
/// Replaces any transform the entity already has.
///
/// @param matrix Column-major 4x4 model matrix, 16 floats. Must not be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_transform(SturdyEngine engine,
                                                                    SturdyEntity entity,
                                                                    const float *matrix);

/// Reads back an entity's world transform.
///
/// @param out_matrix Receives 16 floats, column-major. Must not be null.
/// Orients a light entity so it shines along `(x, y, z)`.
///
/// Writes the entity's `WorldTransform` rotation, leaving no translation. Lights are the only
/// renderable whose transform is read as a direction rather than a placement, and the axis it is
/// taken from is an engine-internal convention, so this exists rather than asking a caller to
/// construct the matrix. Use `sturdy_render_set_transform` when a light also needs a position.
///
/// @param engine Engine handle supplied to a game-logic callback.
/// @param entity Entity carrying a light component.
/// @param x Direction x component, in world space, pointing the way light travels.
/// @param y Direction y component.
/// @param z Direction z component.
///
/// @return `STURDY_OK`; `STURDY_ERROR_INVALID_ARGUMENT` when the direction is not finite or has
///         zero length; a handle or entity error otherwise.
/// @note This function does not throw exceptions.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_light_direction(SturdyEngine engine,
                                                                         SturdyEntity entity,
                                                                         float x,
                                                                         float y,
                                                                         float z);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_get_transform(SturdyEngine engine,
                                                                    SturdyEntity entity,
                                                                    float *out_matrix);

/// Makes an entity draw a model.
///
/// The entity also needs a transform to be drawn. Replaces any model the entity already has.
///
/// @param model Model asset to draw.
/// @param visible Nonzero to draw it. A hidden entity keeps its components and costs nothing to
///        show again, which is cheaper than destroying and respawning it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_model(SturdyEngine engine,
                                                                SturdyEntity entity,
                                                                SturdyAsset model,
                                                                SturdyBool visible);

/// Makes an entity a directional light — the sun.
///
/// Direction comes from the entity's transform, so it needs one. Replaces any directional light the
/// entity already has.
///
/// @param radiance Linear RGB radiance, three floats. Values well above 1 are normal for a sun.
/// @param angular_radius_degrees Apparent size, which is what softens shadow edges. The real sun is
///        about 0.27.
/// @param casts_shadows Nonzero to render shadow maps for this light.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_directional_light(SturdyEngine engine,
                                                                            SturdyEntity entity,
                                                                            const float *radiance,
                                                                            float angular_radius_degrees,
                                                                            SturdyBool casts_shadows);

/// Makes an entity a point light. Position comes from its transform.
///
/// @param radiance Linear RGB radiance, three floats.
/// @param range Distance beyond which the light contributes nothing.
/// @param source_radius Emitter radius, which softens shadows and highlights.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_point_light(SturdyEngine engine,
                                                                      SturdyEntity entity,
                                                                      const float *radiance,
                                                                      float range,
                                                                      float source_radius,
                                                                      SturdyBool casts_shadows);

/// Makes an entity a spot light. Position and direction come from its transform.
///
/// @param radiance Linear RGB radiance, three floats.
/// @param range Distance beyond which the light contributes nothing.
/// @param inner_cone_degrees Half-angle of the fully lit cone.
/// @param outer_cone_degrees Half-angle at which the light has fallen off entirely. Must be at
///        least `inner_cone_degrees`.
/// @param source_radius Emitter radius.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_set_spot_light(SturdyEngine engine,
                                                                     SturdyEntity entity,
                                                                     const float *radiance,
                                                                     float range,
                                                                     float inner_cone_degrees,
                                                                     float outer_cone_degrees,
                                                                     float source_radius,
                                                                     SturdyBool casts_shadows);

// ---------------------------------------------------------------------------------------------
// glTF import
// ---------------------------------------------------------------------------------------------

/// An imported glTF scene: its models, the node instances that place them, and its lights.
///
/// Unlike `SturdyEngine` and `SturdyFrame`, this handle is **owned, not borrowed**. It stays valid
/// until you pass it to `sturdy_gltf_release`, which is the one thing on this ABI you must
/// explicitly free. Releasing it does not destroy the models it produced — those belong to the
/// engine's asset manager and outlive the scene, which is what lets you keep spawning them.
typedef struct SturdyGltfScene {
    uint64_t token;
} SturdyGltfScene;

/// What kind of light a glTF file declared.
typedef enum SturdyGltfLightKind {
    STURDY_GLTF_LIGHT_DIRECTIONAL = 0,
    STURDY_GLTF_LIGHT_POINT = 1,
    STURDY_GLTF_LIGHT_SPOT = 2,
    STURDY_GLTF_LIGHT_FORCE_U32 = 0x7fffffff
} SturdyGltfLightKind;

/// One placement of a model within an imported scene.
///
/// A glTF file can reference the same mesh from many nodes, so several instances may carry the same
/// `model` with different transforms. Spawning one entity per instance is what reproduces the
/// scene as authored.
typedef struct SturdyGltfInstance {
    /// Set by the engine to `sizeof(SturdyGltfInstance)` as this build sees it.
    uint32_t struct_size;
    uint32_t reserved;
    /// The model this node draws. May be invalid (all-zero) for a node that carries no mesh.
    SturdyAsset model;
    /// Column-major 4x4 world transform, with the node's ancestors already applied.
    float world_transform[16];
} SturdyGltfInstance;

/// One light declared by an imported scene.
typedef struct SturdyGltfLight {
    /// Set by the engine to `sizeof(SturdyGltfLight)` as this build sees it.
    uint32_t struct_size;
    SturdyGltfLightKind kind;
    /// Linear RGB radiance.
    float radiance[3];
    /// Falloff distance. Meaningless for a directional light.
    float range;
    /// Spot cone half-angles in degrees. Meaningless for other kinds.
    float inner_cone_degrees;
    float outer_cone_degrees;
    uint32_t reserved;
    /// Column-major 4x4 world transform, giving position and direction.
    float world_transform[16];
} SturdyGltfLight;

/// Imports a glTF or GLB file, creating a model asset for every mesh it contains.
///
/// Importing does not put anything in the world — it produces assets and a description of how the
/// file arranged them. Call `sturdy_gltf_spawn_all` to reproduce that arrangement, or walk the
/// instances yourself if you want to filter or re-place them.
///
/// @param source Path to a `.gltf` or `.glb` file. A `.gltf` referencing external buffers and
///        images resolves them relative to its own directory, so move the whole folder together.
/// @param shader Shader every imported model draws with. Load one with
///        `sturdy_render_load_shader`.
/// @param out_scene Receives the scene handle. Release it with `sturdy_gltf_release`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_import(SturdyEngine engine,
                                                           const char *source,
                                                           SturdyAsset shader,
                                                           SturdyGltfScene *out_scene);

/// Releases an imported scene.
///
/// Frees the instance and light lists this ABI is holding. The model assets survive — they belong
/// to the asset manager — so anything already spawned keeps rendering.
///
/// Releasing twice reports `STURDY_ERROR_HANDLE_EXPIRED` rather than corrupting anything, since
/// scene tokens are never reused.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_release(SturdyGltfScene scene);

/// Returns how many distinct models the import produced.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_model_count(SturdyGltfScene scene,
                                                                uint32_t *out_count);

/// Reads the model asset at `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_model_at(SturdyGltfScene scene,
                                                             uint32_t index,
                                                             SturdyAsset *out_model);

/// Returns how many node instances the scene places.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_count(SturdyGltfScene scene,
                                                                   uint32_t *out_count);

/// Reads the node instance at `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_at(SturdyGltfScene scene,
                                                                uint32_t index,
                                                                SturdyGltfInstance *out_instance);

/// Reads the name of the node instance at `index`. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_name(SturdyGltfScene scene,
                                                                  uint32_t index,
                                                                  char *buffer,
                                                                  size_t capacity,
                                                                  size_t *out_length);

/// Returns how many lights the scene declares.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_light_count(SturdyGltfScene scene,
                                                                uint32_t *out_count);

/// Reads the light at `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_light_at(SturdyGltfScene scene,
                                                             uint32_t index,
                                                             SturdyGltfLight *out_light);

/// Reads the name of the light at `index`. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_light_name(SturdyGltfScene scene,
                                                               uint32_t index,
                                                               char *buffer,
                                                               size_t capacity,
                                                               size_t *out_length);

/// Spawns an entity for every instance and light in the scene, reproducing it as authored.
///
/// The convenience path: import, spawn, release. Instances with no model are skipped, since an
/// entity that draws nothing and lights nothing would only cost iteration.
///
/// @param out_spawned Receives how many entities were created. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gltf_spawn_all(SturdyEngine engine,
                                                              SturdyGltfScene scene,
                                                              uint32_t *out_spawned);

// ---------------------------------------------------------------------------------------------
// Async tasks
// ---------------------------------------------------------------------------------------------

/// A running background task.
///
/// Owned like `SturdyGltfScene`, not scope-bound: it stays valid until `sturdy_async_release`.
/// Releasing does **not** cancel the task — there is no cancellation — it only says you are done
/// observing it. A task whose handle has been released still runs to completion, so whatever its
/// `user_data` points at must stay alive until the task itself finishes.
typedef struct SturdyTask {
    uint64_t token;
} SturdyTask;

/// How much work a task represents, which decides where the scheduler puts it.
typedef enum SturdyTaskWeight {
    /// Short work. Queued where latency matters more than throughput.
    STURDY_TASK_LIGHT = 0,
    /// Long work. Kept off the lanes that short tasks depend on, so one heavy job cannot stall a
    /// frame's worth of small ones.
    STURDY_TASK_HEAVY = 1,
    STURDY_TASK_WEIGHT_FORCE_U32 = 0x7fffffff
} SturdyTaskWeight;

/// Body of a background task.
///
/// Runs on a scheduler worker thread, not the thread that spawned it. Must not unwind.
///
/// Nothing in this ABI is safe to call concurrently with the engine's own frame from a worker
/// thread — the ECS and renderer entry points assume the caller is the thread the engine handed a
/// handle to. Use tasks for work that does not touch the engine (loading, decoding, pathfinding,
/// network), publish the result somewhere your game logic reads, and apply it from a system or
/// frame callback.
typedef void(STURDY_ABI_CALL *SturdyTaskFn)(void *user_data);

/// Reports whether the engine's task scheduler is running.
///
/// It starts on first use — the first frame or the first `sturdy_async_spawn` — so this reads false
/// before then.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_is_running(SturdyBool *out_running);

/// Returns how many worker threads the scheduler has.
///
/// Zero when it is not running. Useful for sizing your own work splitting to the machine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_worker_count(uint32_t *out_count);

/// Reports whether the calling thread is a scheduler worker.
///
/// Worth checking before doing anything that blocks: blocking a worker starves the pool it belongs
/// to.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_is_worker_thread(SturdyBool *out_worker);

/// Runs `task` on a background thread.
///
/// Starts the scheduler if it is not already running.
///
/// @param task Body to run. Must not be null and must not unwind.
/// @param user_data Passed to `task` untouched. Must stay alive until the task completes, which is
///        not the same as until the handle is released.
/// @param weight Whether this is short or long work.
/// @param out_task Receives the task handle. May be null if you never need to observe it — the
///        task still runs, and nothing needs releasing.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_spawn(SturdyTaskFn task,
                                                           void *user_data,
                                                           SturdyTaskWeight weight,
                                                           SturdyTask *out_task);

/// Reports whether a task has finished.
///
/// The non-blocking way to observe one: poll this from your frame callback rather than waiting.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_is_done(SturdyTask task, SturdyBool *out_done);

/// Blocks until a task finishes.
///
/// Reports `STURDY_ERROR_BUSY` when called from a scheduler worker thread. Waiting there can
/// deadlock — the task you are waiting for may be queued behind you on the very pool you are
/// occupying — so it is refused rather than risked.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_wait(SturdyTask task);

/// Releases a task handle.
///
/// Does not cancel or wait. Releasing before the task finishes is fine and is the normal thing to
/// do for fire-and-forget work; the task keeps running.
///
/// Releasing twice reports `STURDY_ERROR_HANDLE_EXPIRED`, since task tokens are never reused.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_release(SturdyTask task);

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

/// How an element sizes itself along one axis.
typedef enum SturdyUiSizing {
    /// Shrink to fit the children.
    STURDY_UI_SIZING_FIT = 0,
    /// Expand to fill the parent, sharing the space with other growing siblings.
    STURDY_UI_SIZING_GROW = 1,
    /// Exactly `value` pixels.
    STURDY_UI_SIZING_FIXED = 2,
    /// A fraction of the parent, where `value` is 0..1.
    STURDY_UI_SIZING_PERCENT = 3,
    STURDY_UI_SIZING_FORCE_U32 = 0x7fffffff
} SturdyUiSizing;

/// Which way an element stacks its children.
typedef enum SturdyUiDirection {
    STURDY_UI_DIRECTION_LEFT_TO_RIGHT = 0,
    STURDY_UI_DIRECTION_TOP_TO_BOTTOM = 1,
    STURDY_UI_DIRECTION_FORCE_U32 = 0x7fffffff
} SturdyUiDirection;

/// One element in the layout tree.
///
/// A deliberately small slice of what the engine's C++ `ElementDecl` can express — enough for
/// panels, rows, columns and spacing. Borders, clipping, floating elements and per-element cursors
/// are not exposed yet.
///
/// Initialize with `sturdy_ui_element_init` before setting fields.
typedef struct SturdyUiElement {
    /// Set to `sizeof(SturdyUiElement)`.
    uint32_t struct_size;

    SturdyUiSizing width_kind;
    float width_value;
    SturdyUiSizing height_kind;
    float height_value;

    float padding_left;
    float padding_right;
    float padding_top;
    float padding_bottom;

    /// Space between children, in pixels.
    float child_gap;
    SturdyUiDirection direction;

    /// Background, as non-linear sRGB with straight alpha. Fully transparent by default.
    float background[4];
    /// Corner radii: top-left, top-right, bottom-left, bottom-right.
    float corner_radius[4];

    /// Stable identifier, or null for an unnamed element. Needed for `sturdy_ui_hovered` and
    /// `sturdy_ui_clicked`, which address elements by id.
    const char *id;
} SturdyUiElement;

/// How text is drawn.
typedef struct SturdyUiTextStyle {
    /// Set to `sizeof(SturdyUiTextStyle)`.
    uint32_t struct_size;
    /// Font registered with `sturdy_ui_register_font`.
    uint32_t font_id;
    uint32_t font_size;
    uint32_t reserved;
    /// Non-linear sRGB with straight alpha.
    float color[4];
} SturdyUiTextStyle;

/// Fills an element declaration with defaults: fit sizing, no padding, transparent, left-to-right.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_element_init(SturdyUiElement *element);

/// Fills a text style with defaults: font 0, 16px, opaque white.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_text_style_init(SturdyUiTextStyle *style);

/// Loads a font file and registers it under `font_id`.
///
/// Text draws nothing until a font is registered, so do this once during
/// `on_engine_initialized`. `font_id` 0 is the default the text style starts with.
///
/// @param source Path to a TTF or OTF file.
/// @param font_id Identifier to register it under.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_register_font(SturdyEngine engine,
                                                                const char *source,
                                                                uint32_t font_id);

/// Opens a UI frame, to be built and then closed with `sturdy_ui_end`.
///
/// Call this from `request_render_frame` before building any UI. It readies the UI renderer,
/// starts a layout pass sized to the surface, and picks up this frame's pointer state so hover and
/// click queries work.
///
/// Between begin and end, the ABI holds an open layout. Build the tree, then call `sturdy_ui_end`
/// on every path — including when you decide not to render the frame — or the next frame's begin
/// reports `STURDY_ERROR_BUSY`.
///
/// @param input This frame's input, as handed to `request_render_frame`.
/// @return `STURDY_ERROR_NOT_AVAILABLE` if the UI renderer could not be created, which is not
///         fatal — skip UI for this frame and try again next.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_begin(SturdyEngine engine,
                                                        const SturdyFrameInput *input);

/// Closes the UI frame and attaches the result to `frame` as an overlay.
///
/// Every element opened with `sturdy_ui_begin_element` must be closed first.
///
/// @param frame The frame being configured, as handed to `request_render_frame`. Pass a zeroed
///        handle to discard the UI instead of drawing it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_end(SturdyEngine engine, SturdyFrame frame);

/// Opens an element. Children declared until the matching `sturdy_ui_end_element` go inside it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_begin_element(SturdyEngine engine,
                                                                const SturdyUiElement *element);

/// Closes the most recently opened element.
///
/// Reports `STURDY_ERROR_INVALID_ARGUMENT` if nothing is open, rather than unbalancing the tree.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_end_element(SturdyEngine engine);

/// Draws text inside the current element.
///
/// @param text Null-terminated UTF-8.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_text(SturdyEngine engine,
                                                       const char *text,
                                                       const SturdyUiTextStyle *style);

/// Reports whether the pointer is over the element with this id.
///
/// Reflects the previous frame's layout, which is how immediate-mode UI works: an element's bounds
/// are not known until it has been laid out once.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_hovered(SturdyEngine engine,
                                                          const char *id,
                                                          SturdyBool *out_hovered);

/// Reports whether the element with this id was clicked this frame.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_clicked(SturdyEngine engine,
                                                          const char *id,
                                                          SturdyBool *out_clicked);

/// Reads the pointer position in UI coordinates.
///
/// @param out_x Receives the horizontal position. May be null.
/// @param out_y Receives the vertical position. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_position(SturdyEngine engine,
                                                                   float *out_x,
                                                                   float *out_y);

/// Reports whether the pointer is currently held down.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_pointer_down(SturdyEngine engine,
                                                               SturdyBool *out_down);

// ---------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------

/// Reads the time step for the current tick, after the time scale is applied.
///
/// This is the value gameplay should integrate against. It differs from `SturdyFrameInput`'s
/// `delta_seconds`, which is raw per-surface frame time and ignores the scale entirely.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_time_delta_seconds(SturdyEngine engine,
                                                                  double *out_seconds);

/// Reads the time step for the current tick before the time scale is applied.
///
/// Use this for anything that must keep running at real speed while the game is slowed or paused —
/// UI animation, profiling, input smoothing.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_time_unscaled_delta_seconds(SturdyEngine engine,
                                                                           double *out_seconds);

/// Reads the monotonically increasing simulation tick counter.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_time_tick_index(SturdyEngine engine,
                                                               uint64_t *out_tick_index);

/// Reads the current time scale. 1.0 is real time, 0.0 is fully paused.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_time_scale(SturdyEngine engine, double *out_scale);

/// Sets the time scale.
///
/// @param scale Multiplier applied to the simulation step. Must be finite and non-negative;
///        negative values are rejected rather than running the simulation backwards, which nothing
///        downstream is written to handle.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_time_set_scale(SturdyEngine engine, double scale);

// ---------------------------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------------------------

/// Cursor shapes a window can request.
typedef enum SturdyCursorIcon {
    STURDY_CURSOR_ICON_DEFAULT = 0,
    STURDY_CURSOR_ICON_POINTER = 1,
    STURDY_CURSOR_ICON_TEXT = 2,
    STURDY_CURSOR_ICON_GRAB = 3,
    STURDY_CURSOR_ICON_GRABBING = 4,
    STURDY_CURSOR_ICON_RESIZE_HORIZONTAL = 5,
    STURDY_CURSOR_ICON_RESIZE_VERTICAL = 6,
    STURDY_CURSOR_ICON_RESIZE_NWSE = 7,
    STURDY_CURSOR_ICON_RESIZE_NESW = 8,
    STURDY_CURSOR_ICON_NOT_ALLOWED = 9,
    STURDY_CURSOR_ICON_FORCE_U32 = 0x7fffffff
} SturdyCursorIcon;

/// Presentation mode of a window.
typedef enum SturdyWindowMode {
    STURDY_WINDOW_MODE_WINDOWED = 0,
    STURDY_WINDOW_MODE_BORDERLESS_FULLSCREEN = 1,
    STURDY_WINDOW_MODE_EXCLUSIVE_FULLSCREEN = 2,
    STURDY_WINDOW_MODE_FORCE_U32 = 0x7fffffff
} SturdyWindowMode;

/// Observed state of one window this frame.
///
/// `size` is in logical units and `framebuffer_size` in physical pixels; they differ on a
/// high-DPI display, and rendering must use the framebuffer values while UI hit-testing uses the
/// logical ones.
typedef struct SturdyWindowSnapshot {
    /// Set by the engine to `sizeof(SturdyWindowSnapshot)` as this build sees it.
    uint32_t struct_size;
    uint32_t reserved;
    /// Matches the `SturdySurface::id` handed to `request_render_frame`, so a frame callback can
    /// look up the window it is drawing into.
    uint64_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    int32_t position_x;
    int32_t position_y;
    float opacity;
    SturdyBool mouse_locked;
    SturdyBool focused;
    uint8_t reserved_tail[2];
} SturdyWindowSnapshot;

/// Returns how many windows the engine currently manages.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_count(SturdyEngine engine, uint32_t *out_count);

/// Reads the window at `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_snapshot(SturdyEngine engine,
                                                               uint32_t index,
                                                               SturdyWindowSnapshot *out_snapshot);

/// Reads the primary window.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when there is no primary window, which is the case during
///         teardown after it has closed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_primary(SturdyEngine engine,
                                                              SturdyWindowSnapshot *out_snapshot);

/// Reads the window a surface belongs to.
///
/// @param surface Surface identifier, as handed to `request_render_frame`.
/// @return `STURDY_ERROR_NOT_AVAILABLE` when no managed window matches.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_find(SturdyEngine engine,
                                                           SturdySurface surface,
                                                           SturdyWindowSnapshot *out_snapshot);

// ---------------------------------------------------------------------------------------------
// Window requests
// ---------------------------------------------------------------------------------------------

/// Requests that a window close.
///
/// Queued rather than immediate: the engine applies window changes at a defined point in the frame
/// so a request made from a render callback cannot destroy the window being drawn. Every function
/// in this section behaves that way.
///
/// @param out_request_id Receives an identifier for the queued request. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_request_close(SturdyEngine engine,
                                                                    SturdySurface surface,
                                                                    uint64_t *out_request_id);

/// Requests a cursor shape for a window.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_cursor_icon(SturdyEngine engine,
                                                                      SturdySurface surface,
                                                                      SturdyCursorIcon icon);

/// Requests a windowed/fullscreen mode change.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_mode(SturdyEngine engine,
                                                               SturdySurface surface,
                                                               SturdyWindowMode mode);

/// Requests that a window show or hide its system decorations.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_decorated(SturdyEngine engine,
                                                                    SturdySurface surface,
                                                                    SturdyBool decorated);

/// Requests that a window become transparent or opaque.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_transparent(SturdyEngine engine,
                                                                      SturdySurface surface,
                                                                      SturdyBool transparent);

// ---------------------------------------------------------------------------------------------
// RHI introspection
// ---------------------------------------------------------------------------------------------

/// Broad class of the physical device the engine is running on.
typedef enum SturdyDeviceType {
    STURDY_DEVICE_TYPE_OTHER = 0,
    STURDY_DEVICE_TYPE_INTEGRATED_GPU = 1,
    STURDY_DEVICE_TYPE_DISCRETE_GPU = 2,
    STURDY_DEVICE_TYPE_VIRTUAL_GPU = 3,
    STURDY_DEVICE_TYPE_CPU = 4,
    STURDY_DEVICE_TYPE_FORCE_U32 = 0x7fffffff
} SturdyDeviceType;

/// Class of work a queue accepts.
typedef enum SturdyQueueClass {
    STURDY_QUEUE_CLASS_GRAPHICS = 0,
    STURDY_QUEUE_CLASS_COMPUTE = 1,
    STURDY_QUEUE_CLASS_TRANSFER = 2,
    STURDY_QUEUE_CLASS_SPARSE = 3,
    STURDY_QUEUE_CLASS_VIDEO_DECODE = 4,
    STURDY_QUEUE_CLASS_VIDEO_ENCODE = 5,
    STURDY_QUEUE_CLASS_FORCE_U32 = 0x7fffffff
} SturdyQueueClass;

/// Bit flags describing what a queue can do. Combined with bitwise OR in
/// `SturdyQueueInfo::capabilities`.
typedef enum SturdyQueueCapability {
    STURDY_QUEUE_CAPABILITY_NONE = 0,
    STURDY_QUEUE_CAPABILITY_GRAPHICS = 1 << 0,
    STURDY_QUEUE_CAPABILITY_COMPUTE = 1 << 1,
    STURDY_QUEUE_CAPABILITY_TRANSFER = 1 << 2,
    STURDY_QUEUE_CAPABILITY_PRESENT = 1 << 3,
    STURDY_QUEUE_CAPABILITY_SPARSE_BINDING = 1 << 4,
    STURDY_QUEUE_CAPABILITY_VIDEO_DECODE = 1 << 5,
    STURDY_QUEUE_CAPABILITY_VIDEO_ENCODE = 1 << 6,
    STURDY_QUEUE_CAPABILITY_FORCE_U32 = 0x7fffffff
} SturdyQueueCapability;

/// Numeric identity of the adapter the engine selected.
///
/// Text fields (name, vendor, driver version, API version, stable device id) are read separately
/// through `sturdy_rhi_adapter_string`, following the string-output convention above.
typedef struct SturdyAdapterInfo {
    /// Set by the engine to `sizeof(SturdyAdapterInfo)` as this build sees it.
    uint32_t struct_size;
    uint32_t vendor_id;
    uint32_t device_id;
    SturdyDeviceType device_type;
    SturdyBackend backend;
    SturdyBool is_discrete;
    uint8_t reserved[7];
} SturdyAdapterInfo;

/// Selects which of the adapter's text fields `sturdy_rhi_adapter_string` returns.
typedef enum SturdyAdapterString {
    STURDY_ADAPTER_STRING_NAME = 0,
    STURDY_ADAPTER_STRING_VENDOR = 1,
    STURDY_ADAPTER_STRING_DRIVER_VERSION = 2,
    STURDY_ADAPTER_STRING_API_VERSION = 3,
    /// Stable identifier for this physical device, suitable for persisting a user's GPU choice
    /// and passing back as configuration on a later run.
    STURDY_ADAPTER_STRING_PHYSICAL_DEVICE_ID = 4,
    STURDY_ADAPTER_STRING_FORCE_U32 = 0x7fffffff
} SturdyAdapterString;

/// Hardware limits of the active device.
typedef struct SturdyDeviceLimits {
    /// Set by the engine to `sizeof(SturdyDeviceLimits)` as this build sees it.
    uint32_t struct_size;
    uint32_t max_texture_dimension_2d;
    uint32_t max_texture_array_layers;
    uint32_t max_bind_groups;
    uint32_t max_push_constants_size;
    uint32_t max_vertex_buffers;
    uint32_t max_vertex_attributes;
    uint32_t max_color_attachments;
    uint32_t max_framebuffer_sample_count;
    /// Bit mask of supported sample counts, one bit per power of two.
    uint32_t framebuffer_sample_counts;
    uint32_t max_compute_workgroup_size_x;
    uint32_t max_compute_workgroup_size_y;
    uint32_t max_compute_workgroup_size_z;
    uint32_t timestamp_valid_bits;
    uint64_t min_uniform_buffer_offset_alignment;
    uint64_t min_storage_buffer_offset_alignment;
    float timestamp_period_ns;
    SturdyBool supports_minimum_depth_resolve;
    SturdyBool supports_bc_texture_compression;
    uint8_t reserved[2];
} SturdyDeviceLimits;

/// Description of one queue family the device exposes.
typedef struct SturdyQueueInfo {
    /// Set by the engine to `sizeof(SturdyQueueInfo)` as this build sees it.
    uint32_t struct_size;
    SturdyQueueClass queue_class;
    /// Bitwise OR of `SturdyQueueCapability` values.
    uint32_t capabilities;
    /// Number of independently submittable lanes in this class.
    uint32_t lane_count;
    /// Queues sharing a physical group contend for the same hardware, so submitting to two of them
    /// does not actually overlap.
    uint32_t physical_group;
    SturdyBool likely_parallel_with_graphics;
    SturdyBool dedicated;
    uint8_t reserved[2];
} SturdyQueueInfo;

/// Reports which graphics backend the engine actually created.
///
/// This is the resolved answer, never `STURDY_BACKEND_DEFAULT`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_backend(SturdyEngine engine,
                                                           SturdyBackend *out_backend);

/// Reads the active adapter's numeric identity.
///
/// @param out_info Destination. Must not be null; `struct_size` is written by the engine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_adapter_info(SturdyEngine engine,
                                                                SturdyAdapterInfo *out_info);

/// Reads one of the adapter's text fields. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_adapter_string(SturdyEngine engine,
                                                                  SturdyAdapterString which,
                                                                  char *buffer,
                                                                  size_t capacity,
                                                                  size_t *out_length);

/// Reads the active device's hardware limits.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_device_limits(SturdyEngine engine,
                                                                 SturdyDeviceLimits *out_limits);

/// Returns how many RHI features exist in this engine build.
///
/// Features are addressed by index rather than by a mirrored C enum on purpose: the engine defines
/// several hundred of them and adds more over time, and restating that list here would guarantee
/// the two drift apart. Enumerate names with `sturdy_rhi_feature_name`, or resolve a known name to
/// its index once at startup with `sturdy_rhi_feature_index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_count(uint32_t *out_count);

/// Reads the name of the feature at `index`. See the string-output convention above.
///
/// @param index Feature index, less than `sturdy_rhi_feature_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_name(uint32_t index,
                                                                char *buffer,
                                                                size_t capacity,
                                                                size_t *out_length);

/// Resolves a feature name to its index.
///
/// @param name Null-terminated feature name, matching `sturdy_rhi_feature_name` exactly.
/// @param out_index Receives the index.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when this build has no feature by that name — which is
///         also how a binding detects that it is running against an older engine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_index(const char *name,
                                                                 uint32_t *out_index);

/// Reports whether the feature at `index` was actually enabled on the active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_feature_enabled(SturdyEngine engine,
                                                                   uint32_t index,
                                                                   SturdyBool *out_enabled);

/// Returns how many queue families the active device exposes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_queue_count(SturdyEngine engine,
                                                               uint32_t *out_count);

/// Reads the queue family at `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_queue_info(SturdyEngine engine,
                                                              uint32_t index,
                                                              SturdyQueueInfo *out_info);

/// Returns how many RHI extensions are enabled on the active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_extension_count(SturdyEngine engine,
                                                                   uint32_t *out_count);

/// Reads the enabled extension at `index` as `"<namespace>.<name>"`, with its version.
///
/// @param out_version Receives the extension version. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_extension_name(SturdyEngine engine,
                                                                  uint32_t index,
                                                                  char *buffer,
                                                                  size_t capacity,
                                                                  size_t *out_length,
                                                                  uint32_t *out_version);

// ---------------------------------------------------------------------------------------------
// Native handles
// ---------------------------------------------------------------------------------------------

/// The engine's raw Vulkan objects.
///
/// Fields are `void *` rather than `VkInstance` and friends so this header stays standalone — it
/// must be includable by a caller that has no Vulkan SDK, and by one that has a different version
/// than the engine was built against. Cast each field to its documented type.
///
/// **Every handle is borrowed.** The engine owns them and destroys them at shutdown. Do not
/// destroy them, and do not use them after the engine is gone. Anything you do with them sits
/// outside the engine's own synchronization: if you submit work on these queues or record into the
/// device, you are responsible for not racing the engine's frame loop.
typedef struct SturdyVulkanHandles {
    /// Set by the engine to `sizeof(SturdyVulkanHandles)` as this build sees it.
    uint32_t struct_size;
    uint32_t reserved;
    /// `VkInstance`.
    void *instance;
    /// `VkPhysicalDevice`.
    void *physical_device;
    /// `VkDevice`.
    void *device;
    /// `VkQueue` for the primary graphics lane.
    void *graphics_queue;
} SturdyVulkanHandles;

/// The engine's raw D3D12 objects.
///
/// Fields are `void *` for the same reason as `SturdyVulkanHandles`, and carry the same borrowing
/// rules. These pointers are **not** `AddRef`'d on your behalf: call `AddRef` yourself if you
/// intend to hold one past the call.
typedef struct SturdyD3D12Handles {
    /// Set by the engine to `sizeof(SturdyD3D12Handles)` as this build sees it.
    uint32_t struct_size;
    uint32_t reserved;
    /// `IDXGIFactory6 *`.
    void *factory;
    /// `IDXGIAdapter4 *`.
    void *adapter;
    /// `ID3D12Device *`.
    void *device;
    /// `ID3D12CommandQueue *` for the primary graphics queue.
    void *graphics_queue;
} SturdyD3D12Handles;

/// Reports whether raw native handles are available.
///
/// False when `SturdyRuntimeConfig::enable_native_access` was not set, or when the backend could
/// not publish them. Check this before calling the getters below rather than treating their
/// failure as fatal.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_available(SturdyEngine engine,
                                                                SturdyBool *out_available);

/// Reads the engine's raw Vulkan objects.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when the active backend is not Vulkan, or when native
///         access was not enabled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_vulkan(SturdyEngine engine,
                                                             SturdyVulkanHandles *out_handles);

/// Reads the `VkQueue` and queue family index backing one queue lane.
///
/// @param queue_class Class of queue to resolve.
/// @param lane_index Lane within that class.
/// @param out_queue Receives the `VkQueue`. May be null.
/// @param out_queue_family_index Receives the Vulkan queue family index, which is what you need to
///        create your own command pool against this queue. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_vulkan_queue(SturdyEngine engine,
                                                                   SturdyQueueClass queue_class,
                                                                   uint32_t lane_index,
                                                                   void **out_queue,
                                                                   uint32_t *out_queue_family_index);

/// Reads the engine's raw D3D12 objects.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when the active backend is not D3D12, or when native
///         access was not enabled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_d3d12(SturdyEngine engine,
                                                            SturdyD3D12Handles *out_handles);

/// Reads the `ID3D12CommandQueue *` backing one queue lane.
///
/// @param queue_class Class of queue to resolve.
/// @param lane_index Lane within that class.
/// @param out_queue Receives the queue. Must not be null. Not `AddRef`'d.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_d3d12_queue(SturdyEngine engine,
                                                                  SturdyQueueClass queue_class,
                                                                  uint32_t lane_index,
                                                                  void **out_queue);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // STURDY_FFI_STURDY_H
