/// SturdyEngine 5 stable C ABI.
///
/// This is the language-neutral seam every non-C++ consumer (Rust, C#, Java, ...) binds against.
/// Its core is the application-hosting seam — configure a runtime, supply game-logic callbacks,
/// drive per-frame camera/lighting/render-graph choices, and read input — plus, for a caller that
/// needs to do its own GPU work, the RHI resource surface: buffers, textures, samplers, bind
/// groups, pipelines, command encoding, and ray tracing (see "RHI resources" and "Ray tracing"
/// below), plus per-surface presentation/HDR control (see "Presentation and HDR" below).
/// Render-graph tuning (shadow, ambient occlusion, anti-aliasing, bloom, tone-mapping, ReSTIR
/// GI, motion blur) is exposed via the `Sturdy*Settings` structs and their `_init` functions
/// below; anything finer than that still reaches for `sturdy_native_*`.
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
#define STURDY_ABI_VERSION_MINOR 27u

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
    /// The GPU device was lost (driver crash/reset, surprise-removed adapter, ...). Every
    /// resource and command buffer from before this point is invalid; the engine cannot recover
    /// within this process.
    STURDY_ERROR_DEVICE_LOST = 18,
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
// Logging
// ---------------------------------------------------------------------------------------------
//
// Lets a foreign caller host its own console/log viewer instead of only ever seeing this
// process's stdout: register a callback once, and it receives every log message the engine
// produces from that point on, alongside whatever console/file sinks are already configured.
// Independent of everything else in this header — safe to call before `sturdy_runtime_run`, from
// any thread, at any point in the process's life.

/// Severity of a log message.
typedef enum SturdyLogLevel {
    STURDY_LOG_LEVEL_TRACE = 0,
    STURDY_LOG_LEVEL_DEBUG = 1,
    STURDY_LOG_LEVEL_INFO = 2,
    STURDY_LOG_LEVEL_WARN = 3,
    STURDY_LOG_LEVEL_ERROR = 4,
    STURDY_LOG_LEVEL_CRITICAL = 5,
    STURDY_LOG_LEVEL_FORCE_U32 = 0x7fffffff
} SturdyLogLevel;

/// Handle to a registered log sink, returned by `sturdy_log_add_sink`.
typedef struct SturdyLogSink {
    uint64_t id;
} SturdyLogSink;

/// Invoked once per log message after registration via `sturdy_log_add_sink`.
///
/// @param level Severity of the message.
/// @param message Formatted message text — no timestamp/level prefix, just what was passed to the
///        engine's own `log_info`/`log_warn`/etc. Not null-terminated; use `message_length`. Only
///        valid for the duration of this call — copy it if you need to keep it.
/// @param message_length Byte length of `message`.
/// @param user_data The pointer passed to `sturdy_log_add_sink`, unchanged.
///
/// @note Called from whichever thread produced the log message — this can be any engine thread,
///       concurrently with other callback invocations. Do not block, and do not call back into
///       the engine from within this callback.
typedef void(STURDY_ABI_CALL *SturdyLogCallback)(SturdyLogLevel level,
                                                  const char *message,
                                                  size_t message_length,
                                                  void *user_data);

/// Registers a callback to receive every subsequent log message.
///
/// @param callback Function invoked per message. Must not be null.
/// @param user_data Opaque pointer passed back to `callback` unchanged. May be null.
/// @param out_sink Receives a handle to unregister this sink later with `sturdy_log_remove_sink`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_log_add_sink(SturdyLogCallback callback,
                                                             void *user_data,
                                                             SturdyLogSink *out_sink);

/// Unregisters a sink added by `sturdy_log_add_sink`. A null/zero handle, or one already removed,
/// is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_log_remove_sink(SturdyLogSink sink);

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
    STURDY_BACKEND_METAL = 3,
    STURDY_BACKEND_WEBGPU = 4,
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

/// Broad class of a physical device — the engine's active one (`SturdyAdapterInfo::device_type`)
/// or one reported by `sturdy_gpu_enumerate` before any engine exists.
typedef enum SturdyDeviceType {
    STURDY_DEVICE_TYPE_OTHER = 0,
    STURDY_DEVICE_TYPE_INTEGRATED_GPU = 1,
    STURDY_DEVICE_TYPE_DISCRETE_GPU = 2,
    STURDY_DEVICE_TYPE_VIRTUAL_GPU = 3,
    STURDY_DEVICE_TYPE_CPU = 4,
    STURDY_DEVICE_TYPE_FORCE_U32 = 0x7fffffff
} SturdyDeviceType;

/// Bitmask of graphics backends (see `SturdyBackend`) a physical GPU can be used with, as reported
/// by `sturdy_gpu_enumerate`.
typedef enum SturdyBackendMask {
    STURDY_BACKEND_MASK_NONE = 0,
    STURDY_BACKEND_MASK_VULKAN = 1u << 0,
    STURDY_BACKEND_MASK_D3D12 = 1u << 1,
    STURDY_BACKEND_MASK_METAL = 1u << 2,
    STURDY_BACKEND_MASK_WEBGPU = 1u << 3,
    STURDY_BACKEND_MASK_FORCE_U32 = 0x7fffffff
} SturdyBackendMask;

/// One physical GPU this machine can create an engine on, as reported by `sturdy_gpu_enumerate`.
///
/// Unlike other queries in this header, `name`/`vendor`/`physical_device_id` are fixed-size
/// buffers embedded directly in this struct rather than following the general string-output
/// convention above (buffer + capacity + `*out_length`, queried by calling twice): enumeration
/// itself creates and tears down a temporary graphics instance per backend, so asking a second time
/// to learn a string's length would pay that cost again and risks a different snapshot (a GPU
/// unplugged between calls) coming back. Values are always null-terminated, truncated rather than
/// overflowed if a driver-reported string is unusually long.
typedef struct SturdyGpuInfo {
    /// Set to `sizeof(SturdyGpuInfo)` by the engine.
    uint32_t struct_size;
    char name[128];
    char vendor[64];
    /// Pass as `SturdyRuntimeConfig::physical_device_id` to request this GPU.
    char physical_device_id[64];
    SturdyDeviceType device_type;
    uint32_t vendor_id;
    uint32_t device_id;
    /// Bitmask of `SturdyBackendMask` values.
    uint32_t supported_backends;
} SturdyGpuInfo;

/// Enumerates every physical GPU this machine can create an engine on, across every graphics
/// backend this build supports. Safe to call before `sturdy_runtime_run`, with no window or device
/// yet — this is how an application builds its own GPU picker rather than guessing at
/// `physical_device_id` values. Not free: it creates and immediately destroys a temporary graphics
/// instance per backend, so call it once and cache the result rather than every frame.
///
/// @param out_gpus Copies up to `capacity` entries here.
/// @param capacity Size of `out_gpus`, in entries. May be 0 to just read `*out_count`.
/// @param out_count Receives the true number of GPUs found, which may exceed `capacity` —
///        realistically never more than a handful, so a generously sized buffer (8) avoids this.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_gpu_enumerate(SturdyGpuInfo *out_gpus,
                                                             uint32_t capacity,
                                                             uint32_t *out_count);

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
// Render graph settings
// ---------------------------------------------------------------------------------------------
//
// Fine-grained tuning for each render-graph stage, beyond the coarse on/off toggle
// `sturdy_frame_set_feature_enabled` gives. Each `sturdy_frame_set_*_settings` call replaces the
// whole settings group for the frame being built; there is no getter, matching
// `sturdy_frame_set_tone_mapping`'s existing write-only shape. Call the matching
// `sturdy_*_settings_init` first to get engine defaults and the correct `struct_size`, then
// change only the fields intended to differ from default.

/// Rendering path the scene stage evaluates.
typedef enum SturdySceneIntegrator {
    STURDY_SCENE_INTEGRATOR_RASTER_DEFERRED = 0,
    STURDY_SCENE_INTEGRATOR_SHADOW_ONLY = 1,
    STURDY_SCENE_INTEGRATOR_REFLECTION_ONLY = 2,
    STURDY_SCENE_INTEGRATOR_AMBIENT_OCCLUSION_ONLY = 3,
    STURDY_SCENE_INTEGRATOR_SHADOW_AND_TRANSMISSION = 4,
    STURDY_SCENE_INTEGRATOR_FULL_PATH_TRACING = 5,
    STURDY_SCENE_INTEGRATOR_FORCE_U32 = 0x7fffffff
} SturdySceneIntegrator;

typedef struct SturdySceneSettings {
    /// Set to `sizeof(SturdySceneSettings)` by `sturdy_scene_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    SturdySceneIntegrator integrator;
    uint32_t path_samples_per_pixel;
    uint32_t path_max_bounces;
    uint32_t path_russian_roulette_start_bounce;
    uint32_t caustic_photon_count;
    float caustic_gather_radius;
    float wavelength_min_nm;
    float wavelength_max_nm;
    /// Multiplier applied to the background color/environment when nothing else occludes a ray.
    float background_intensity;
} SturdySceneSettings;

/// Fills `settings` with `struct_size` and engine defaults (raster deferred, path tracing
/// parameters set but unused until `integrator` selects a path-traced mode).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scene_settings_init(SturdySceneSettings *settings);

/// Replaces the scene stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_scene_settings(SturdyFrame frame,
                                                                        const SturdySceneSettings *settings);

/// Single-frame shadow debug visualization. See `SturdyShadowSettings::debug_view`.
typedef enum SturdyShadowDebugView {
    STURDY_SHADOW_DEBUG_VIEW_NONE = 0,
    STURDY_SHADOW_DEBUG_VIEW_CASCADE_INDEX = 1,
    STURDY_SHADOW_DEBUG_VIEW_CASCADE_FADE = 2,
    STURDY_SHADOW_DEBUG_VIEW_SHADOW_TEXEL_GRID = 3,
    STURDY_SHADOW_DEBUG_VIEW_SHADOW_UV = 4,
    STURDY_SHADOW_DEBUG_VIEW_RECEIVER_DEPTH = 5,
    STURDY_SHADOW_DEBUG_VIEW_ATLAS_DEPTH = 6,
    STURDY_SHADOW_DEBUG_VIEW_DEPTH_DELTA = 7,
    STURDY_SHADOW_DEBUG_VIEW_NORMAL_BIAS = 8,
    STURDY_SHADOW_DEBUG_VIEW_RECEIVER_PLANE_GRADIENT = 9,
    STURDY_SHADOW_DEBUG_VIEW_HARD_COMPARISON = 10,
    STURDY_SHADOW_DEBUG_VIEW_PCF = 11,
    STURDY_SHADOW_DEBUG_VIEW_DIRECTIONAL_CSM = 12,
    STURDY_SHADOW_DEBUG_VIEW_CONTACT_SHADOW = 13,
    STURDY_SHADOW_DEBUG_VIEW_COMBINED_SUN_VISIBILITY = 14,
    STURDY_SHADOW_DEBUG_VIEW_GBUFFER_DEPTH = 15,
    STURDY_SHADOW_DEBUG_VIEW_WORLD_POSITION = 16,
    STURDY_SHADOW_DEBUG_VIEW_GBUFFER_NORMAL = 17,
    STURDY_SHADOW_DEBUG_VIEW_GBUFFER_ALBEDO = 18,
    STURDY_SHADOW_DEBUG_VIEW_GBUFFER_ROUGHNESS = 19,
    STURDY_SHADOW_DEBUG_VIEW_GBUFFER_METALLIC = 20,
    STURDY_SHADOW_DEBUG_VIEW_MATERIAL_AMBIENT_OCCLUSION = 21,
    STURDY_SHADOW_DEBUG_VIEW_AMBIENT_LIGHTING = 22,
    STURDY_SHADOW_DEBUG_VIEW_SUN_N_DOT_L = 23,
    STURDY_SHADOW_DEBUG_VIEW_UNSHADOWED_SUN_LIGHTING = 24,
    STURDY_SHADOW_DEBUG_VIEW_SCREEN_SPACE_AMBIENT_OCCLUSION = 25,
    STURDY_SHADOW_DEBUG_VIEW_FORCE_U32 = 0x7fffffff
} SturdyShadowDebugView;

typedef struct SturdyShadowSettings {
    /// Set to `sizeof(SturdyShadowSettings)` by `sturdy_shadow_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    /// Edge size of the shared spot/point shadow atlas.
    uint32_t atlas_size;
    uint32_t cascade_count;
    float max_distance;
    float cascade_split_lambda;
    /// Fraction of a cascade's view-space depth range spent cross-fading into the next cascade.
    float cascade_blend;
    float depth_bias;
    float slope_bias;
    /// Per-cascade shadow-map edge resolution, near cascade first.
    uint32_t cascade_resolutions[4];
    /// PCF filter radius in shadow texels of the sampled cascade.
    float filter_radius_texels;
    /// Receiver normal-offset magnitude in shadow texels at normal incidence.
    float normal_bias;
    SturdyShadowDebugView debug_view;
    uint32_t max_shadowed_spot_lights;
    uint32_t max_shadowed_point_lights;
    SturdyBool contact_hardening;
    SturdyBool contact_shadows;
    uint8_t reserved2[2];
    float contact_shadow_distance;
    float contact_shadow_thickness;
    uint32_t contact_shadow_steps;
    /// Maximum darkening the contact term may apply, in [0, 1].
    float contact_shadow_intensity;
    float contact_shadow_fade_distance;
} SturdyShadowSettings;

/// Fills `settings` with `struct_size` and engine defaults (4 cascades, 4096 atlas, contact
/// shadows on, contact hardening off).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_shadow_settings_init(SturdyShadowSettings *settings);

/// Replaces the shadow stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_shadow_settings(SturdyFrame frame,
                                                                         const SturdyShadowSettings *settings);

typedef enum SturdyAmbientOcclusionQuality {
    STURDY_AMBIENT_OCCLUSION_QUALITY_LOW = 0,
    STURDY_AMBIENT_OCCLUSION_QUALITY_MEDIUM = 1,
    STURDY_AMBIENT_OCCLUSION_QUALITY_HIGH = 2,
    STURDY_AMBIENT_OCCLUSION_QUALITY_ULTRA = 3,
    STURDY_AMBIENT_OCCLUSION_QUALITY_FORCE_U32 = 0x7fffffff
} SturdyAmbientOcclusionQuality;

typedef struct SturdyAmbientOcclusionSettings {
    /// Set to `sizeof(SturdyAmbientOcclusionSettings)` by `sturdy_ambient_occlusion_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    /// World-space radius the occlusion search covers around a shaded point.
    float radius;
    SturdyAmbientOcclusionQuality quality;
    /// Blend toward fully unoccluded. 1 = full strength, 0 = no occlusion.
    float intensity;
    /// Fraction of `radius` over which an occluder fades out.
    float falloff_range;
    /// Thin-occluder compensation, in [0, 0.7].
    float thin_occluder_compensation;
    /// Contrast curve applied to the visibility term.
    float final_value_power;
    /// Exponent of the normalized sample-distance distribution.
    float sample_distribution_power;
    /// Runs the 5x5 edge-aware spatial denoiser.
    SturdyBool denoise;
    uint8_t reserved2[3];
} SturdyAmbientOcclusionSettings;

/// Fills `settings` with `struct_size` and engine defaults (High quality, denoiser on).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ambient_occlusion_settings_init(
    SturdyAmbientOcclusionSettings *settings);

/// Replaces the ambient-occlusion stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_ambient_occlusion_settings(
    SturdyFrame frame, const SturdyAmbientOcclusionSettings *settings);

typedef enum SturdyPostProcessAntiAliasing {
    STURDY_POST_PROCESS_ANTI_ALIASING_NONE = 0,
    STURDY_POST_PROCESS_ANTI_ALIASING_FXAA = 1,
    STURDY_POST_PROCESS_ANTI_ALIASING_CONSERVATIVE_MORPHOLOGICAL = 2,
    STURDY_POST_PROCESS_ANTI_ALIASING_FORCE_U32 = 0x7fffffff
} SturdyPostProcessAntiAliasing;

typedef struct SturdyAntiAliasingSettings {
    /// Set to `sizeof(SturdyAntiAliasingSettings)` by `sturdy_anti_aliasing_settings_init`.
    uint32_t struct_size;
    uint32_t msaa_samples;
    SturdyPostProcessAntiAliasing post_process;
    float subpixel_quality;
    float edge_threshold;
} SturdyAntiAliasingSettings;

/// Fills `settings` with `struct_size` and engine defaults (1 MSAA sample, FXAA).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_anti_aliasing_settings_init(SturdyAntiAliasingSettings *settings);

/// Replaces the anti-aliasing stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_anti_aliasing_settings(
    SturdyFrame frame, const SturdyAntiAliasingSettings *settings);

typedef struct SturdyBloomSettings {
    /// Set to `sizeof(SturdyBloomSettings)` by `sturdy_bloom_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    float threshold;
    float soft_knee;
    float intensity;
    float scatter;
    float downsample_ratio;
    uint32_t max_levels;
} SturdyBloomSettings;

/// Fills `settings` with `struct_size` and engine defaults.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_bloom_settings_init(SturdyBloomSettings *settings);

/// Replaces the bloom stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_bloom_settings(SturdyFrame frame,
                                                                        const SturdyBloomSettings *settings);

/// AGX tone-curve look variant. See `SturdyToneMappingSettings::agx_look`.
typedef enum SturdyAgxLook {
    STURDY_AGX_LOOK_NONE = 0,
    STURDY_AGX_LOOK_PUNCHY = 1,
    STURDY_AGX_LOOK_GOLDEN = 2,
    STURDY_AGX_LOOK_FORCE_U32 = 0x7fffffff
} SturdyAgxLook;

/// Fine-grained tone-mapping settings. `sturdy_frame_set_tone_mapping` remains the quick path for
/// just operator/exposure/white-point/saturation; use this instead to also reach HDR display
/// mapping and the AGX/Hermite-spline/PsychoV curve-tuning knobs, which only apply when
/// `operation` selects the matching operator.
typedef struct SturdyToneMappingSettings {
    /// Set to `sizeof(SturdyToneMappingSettings)` by `sturdy_tone_mapping_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    SturdyToneMapping operation;
    float exposure;
    float white_point;
    float saturation;
    /// SDR/HDR mapping reference white, in nits.
    float hdr_paper_white_nits;
    /// HDR display peak brightness, in nits.
    float hdr_peak_nits;
    /// Only read when `operation` is `STURDY_TONE_MAPPING_AGX`.
    SturdyAgxLook agx_look;
    /// Only read when `operation` is `STURDY_TONE_MAPPING_HERMITE_SPLINE`.
    float hermite_toe_strength;
    float hermite_toe_length;
    float hermite_shoulder_strength;
    float hermite_shoulder_length;
    float hermite_shoulder_angle;
    /// Only read when `operation` is `STURDY_TONE_MAPPING_PSYCHO_V`.
    float psychov_highlights;
    float psychov_shadows;
    float psychov_contrast;
    float psychov_purity_scale;
    float psychov_gamut_compression;
    SturdyBool psychov_gamut_compression_use_bt2020;
    uint8_t reserved2[3];
    float psychov_compression;
    float psychov_adapted_gray_bt709[3];
    float psychov_background_gray_bt709[3];
} SturdyToneMappingSettings;

/// Fills `settings` with `struct_size` and engine defaults (AGX operator, no look, neutral
/// PsychoV/Hermite curve parameters).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tone_mapping_settings_init(SturdyToneMappingSettings *settings);

/// Replaces the tone-mapping stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_tone_mapping_settings(
    SturdyFrame frame, const SturdyToneMappingSettings *settings);

typedef enum SturdyRestirGiQuality {
    STURDY_RESTIR_GI_QUALITY_LOW = 0,
    STURDY_RESTIR_GI_QUALITY_MEDIUM = 1,
    STURDY_RESTIR_GI_QUALITY_HIGH = 2,
    STURDY_RESTIR_GI_QUALITY_FORCE_U32 = 0x7fffffff
} SturdyRestirGiQuality;

/// Denoiser resolving ReSTIR GI's raw reservoir output. `DLSS_RAY_RECONSTRUCTION`/`FSR_REDSTONE`
/// are reserved for future vendor-SDK integrations; selecting either falls back to `SVGF` with a
/// one-time engine warning until a real backend exists.
typedef enum SturdyRestirGiDenoiser {
    STURDY_RESTIR_GI_DENOISER_NONE = 0,
    STURDY_RESTIR_GI_DENOISER_SVGF = 1,
    STURDY_RESTIR_GI_DENOISER_DLSS_RAY_RECONSTRUCTION = 2,
    STURDY_RESTIR_GI_DENOISER_FSR_REDSTONE = 3,
    STURDY_RESTIR_GI_DENOISER_FORCE_U32 = 0x7fffffff
} SturdyRestirGiDenoiser;

typedef struct SturdyRestirGiSettings {
    /// Set to `sizeof(SturdyRestirGiSettings)` by `sturdy_restir_gi_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    SturdyRestirGiQuality quality;
    uint32_t spatial_reuse_samples;
    float spatial_reuse_radius_px;
    uint32_t temporal_history_max;
    float max_ray_distance;
    /// Damping (0-1) applied to last frame's lit scene color when read back as an extra indirect
    /// term. 0 disables multi-bounce feedback.
    float multi_bounce_feedback;
    float intensity;
    SturdyRestirGiDenoiser denoiser;
    /// Only read when `denoiser` is `STURDY_RESTIR_GI_DENOISER_SVGF`.
    uint32_t svgf_atrous_iterations;
    float svgf_temporal_alpha;
    float svgf_phi_normal;
    float svgf_phi_depth;
    float svgf_phi_luminance;
    SturdyBool show_debug_reservoirs;
    uint8_t reserved2[3];
} SturdyRestirGiSettings;

/// Fills `settings` with `struct_size` and engine defaults (Medium quality, SVGF denoiser,
/// disabled — ReSTIR GI is opt-in).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_restir_gi_settings_init(SturdyRestirGiSettings *settings);

/// Replaces the ReSTIR GI stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_restir_gi_settings(
    SturdyFrame frame, const SturdyRestirGiSettings *settings);

typedef struct SturdyMotionBlurSettings {
    /// Set to `sizeof(SturdyMotionBlurSettings)` by `sturdy_motion_blur_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    float intensity;
    float shutter_angle_degrees;
    uint32_t tile_size_px;
    uint32_t sample_count;
    float max_blur_radius_px;
    float background_foreground_weight_bias;
    SturdyBool camera_motion_only;
    uint8_t reserved2[3];
} SturdyMotionBlurSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled — motion blur is opt-in).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_motion_blur_settings_init(SturdyMotionBlurSettings *settings);

/// Replaces the motion-blur stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_motion_blur_settings(
    SturdyFrame frame, const SturdyMotionBlurSettings *settings);

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

/// Decodes a texture from an in-memory encoded image (PNG, JPEG, and the rest the engine decodes)
/// — the same decoder `sturdy_render_load_texture` uses, without requiring the bytes to already be
/// a file on disk. For an image downloaded, generated, or packaged some other way.
///
/// @param encoded_bytes Encoded image bytes (a whole PNG/JPEG file's contents, not raw pixels).
/// @param encoded_size Length of `encoded_bytes`, in bytes. Must be nonzero.
/// @param srgb Nonzero to treat the contents as sRGB, which is right for color maps and wrong for
///        normal and roughness maps.
/// @param out_texture Receives the texture asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_load_texture_from_memory(
    SturdyEngine engine,
    const uint8_t *encoded_bytes,
    size_t encoded_size,
    SturdyBool srgb,
    SturdyAsset *out_texture);

/// Describes a texture built directly from raw pixels, for `sturdy_render_create_texture`.
///
/// Named distinctly from the RHI-level `SturdyTextureDesc` (see "RHI resources" below) even
/// though both describe "a texture to create": that one configures a raw GPU resource
/// (`SturdyFormat`, mip levels, usage flags, no pixel data), while this one is the
/// asset-manager-level counterpart to `sturdy_render_load_texture` — pixels in, a `SturdyAsset`
/// usable with `sturdy_render_set_model_texture` out.
typedef struct SturdyRenderTextureDesc {
    /// Set to `sizeof(SturdyRenderTextureDesc)` by `sturdy_render_texture_desc_init`.
    uint32_t struct_size;
    /// Capped well below any real GPU's maximum, only to keep `width * height * 4` from
    /// overflowing while validating this struct — the real limit is
    /// `SturdyDeviceLimits::max_texture_dimension_2d`.
    uint32_t width;
    uint32_t height;
    SturdyBool srgb;
    SturdyBool allow_compression;
    SturdyBool generate_mipmaps;
    uint8_t reserved;
    /// RGBA8 pixel data, `width * height * 4` bytes, row-major and tightly packed (no row
    /// padding). Copied before this call returns; the pointer need not outlive it.
    const uint8_t *rgba8;
} SturdyRenderTextureDesc;

/// Fills `desc` with engine defaults (linear color space, mipmaps and compression both on) and the
/// correct `struct_size`. `width`, `height`, and `rgba8` are left for the caller to set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_texture_desc_init(SturdyRenderTextureDesc *desc);

/// Creates a texture asset directly from raw RGBA8 pixels — for a procedurally generated texture,
/// as opposed to `sturdy_render_load_texture`/`sturdy_render_load_texture_from_memory`, which
/// decode an encoded image format.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_create_texture(SturdyEngine engine,
                                                                     const SturdyRenderTextureDesc *desc,
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

/// Unloads an asset — a model, shader, or texture previously created by `sturdy_render_load_shader`,
/// `sturdy_render_create_shape_model`, `sturdy_render_create_mesh_model`, or
/// `sturdy_render_load_texture`. This is the only way to release one; without it, every asset a
/// long-running caller creates accumulates for the life of the engine.
///
/// Unloading does not check whether an entity's `sturdy_render_set_model` still points at it —
/// despawn or reassign every such entity first, or its next draw reads a destroyed asset.
///
/// @return `STURDY_ERROR_BUSY` when `asset` is a shader or texture a loaded model still depends
///         on; unload that model first. `STURDY_ERROR_INVALID_HANDLE` when `asset` does not name a
///         live asset.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_render_unload_asset(SturdyEngine engine, SturdyAsset asset);

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
// Graph widget (chart plotting)
// ---------------------------------------------------------------------------------------------
//
// A one-shot draw call (sturdy_ui_graph_draw), same shape as sturdy_ui_text: no begin/end pairing,
// draws immediately inside the current UI frame. Series data (SturdyUiGraphSeriesRef::x_data/
// y_data) is read synchronously during the call and not retained, matching this engine's own C++
// SFT::UI::SeriesRef contract.
//
// SturdyGraphSeries is a separate, optional convenience for live/streaming data (e.g. a frame-time
// graph): an owned ring buffer you push samples into across many frames, then read back as flat
// arrays to bind into a SturdyUiGraphSeriesRef each draw. A chart built from data you already hold
// in your own array does not need it.
//
// Not yet exposed: AxisConfig::scale's Custom transform and tick_label_formatter (both are C++
// callback hooks; crossing the ABI boundary safely needs more design than this pass covers) and
// most of the fixed styling knobs (StrokeStyle::dash_length/cap/join, CornerRadius overrides for
// bar fills, ...) — Linear/Log/Symlog scales and the fields below cover the common cases.

/// Chart type for `sturdy_ui_graph_draw`.
typedef enum SturdyUiGraphType {
    STURDY_UI_GRAPH_TYPE_LINE = 0,
    STURDY_UI_GRAPH_TYPE_AREA = 1,
    STURDY_UI_GRAPH_TYPE_BAR = 2,
    STURDY_UI_GRAPH_TYPE_SCATTER = 3,
    STURDY_UI_GRAPH_TYPE_PIE = 4,
    STURDY_UI_GRAPH_TYPE_FORCE_U32 = 0x7fffffff
} SturdyUiGraphType;

/// Axis scale kind. See the engine-side `SFT::UI::ScaleTransform`'s own doc comment for the math
/// behind each.
typedef enum SturdyUiGraphScale {
    STURDY_UI_GRAPH_SCALE_LINEAR = 0,
    STURDY_UI_GRAPH_SCALE_LOG = 1,
    STURDY_UI_GRAPH_SCALE_SYMLOG = 2,
    STURDY_UI_GRAPH_SCALE_FORCE_U32 = 0x7fffffff
} SturdyUiGraphScale;

typedef enum SturdyUiGraphBarStackMode {
    STURDY_UI_GRAPH_BAR_GROUPED = 0,
    STURDY_UI_GRAPH_BAR_STACKED = 1,
    STURDY_UI_GRAPH_BAR_STACK_MODE_FORCE_U32 = 0x7fffffff
} SturdyUiGraphBarStackMode;

/// Handle to a caller-owned, incrementally-appendable data series. Lives until
/// `sturdy_ui_graph_series_release`.
typedef struct SturdyGraphSeries {
    uint64_t token;
} SturdyGraphSeries;

/// One axis's configuration for `sturdy_ui_graph_draw`.
typedef struct SturdyUiGraphAxis {
    /// Set to `sizeof(SturdyUiGraphAxis)` by `sturdy_ui_graph_axis_init`.
    uint32_t struct_size;
    SturdyUiGraphScale scale;
    /// `Log`/`Symlog` only; a value `<= 1` falls back to base 10.
    double log_base;
    /// `Symlog` only: the linear region extends `+-symlog_linear_threshold` around zero.
    double symlog_linear_threshold;

    SturdyBool has_min;
    double min;
    SturdyBool has_max;
    double max;
    float autoscale_padding_percent;
    uint32_t target_tick_count;

    SturdyBool show_gridlines;
    SturdyBool show_minor_gridlines;
    /// Non-linear sRGB with straight alpha.
    float axis_color[4];
    float gridline_color[4];
    float minor_gridline_color[4];
    /// May be null.
    const char *title;

    /// `GraphType::Bar` only: one label per category, left to right. A null/zero-length array
    /// auto-derives numeric labels ("0", "1", ...) from the longest bound series.
    const char *const *categories;
    uint32_t category_count;
} SturdyUiGraphAxis;

/// One data series bound to `sturdy_ui_graph_draw`.
typedef struct SturdyUiGraphSeriesRef {
    /// Set to `sizeof(SturdyUiGraphSeriesRef)`.
    uint32_t struct_size;
    /// May be null.
    const char *name;
    /// `GraphType::Bar` ignores this — the category axis supplies bar positions instead.
    const double *x_data;
    uint32_t x_count;
    const double *y_data;
    uint32_t y_count;
    /// Non-linear sRGB with straight alpha.
    float color[4];
    float line_width;
    float feather_px;
    /// `GraphType::Area` only.
    float area_fill_opacity;
    /// `GraphType::Scatter` only; `0` falls back to a 3px default.
    float marker_radius;
} SturdyUiGraphSeriesRef;

/// One wedge of a `GraphType::Pie` chart.
typedef struct SturdyUiGraphPieSlice {
    /// Set to `sizeof(SturdyUiGraphPieSlice)`.
    uint32_t struct_size;
    /// May be null.
    const char *name;
    double value;
    /// Non-linear sRGB with straight alpha.
    float color[4];
} SturdyUiGraphPieSlice;

typedef struct SturdyUiGraphDesc {
    /// Set to `sizeof(SturdyUiGraphDesc)` by `sturdy_ui_graph_desc_init`.
    uint32_t struct_size;
    SturdyUiGraphType type;
    SturdyUiGraphAxis x_axis;
    SturdyUiGraphAxis y_axis;

    const SturdyUiGraphSeriesRef *series;
    uint32_t series_count;

    /// `GraphType::Bar` only.
    SturdyUiGraphBarStackMode bar_stack_mode;
    float bar_group_gap_fraction;
    float bar_series_gap_fraction;

    /// `GraphType::Pie` only.
    const SturdyUiGraphPieSlice *pie_slices;
    uint32_t pie_slice_count;
    float pie_hole_ratio;
    float pie_start_angle_degrees;
    float pie_gap_degrees;
    float pie_feather_px;

    /// Non-linear sRGB with straight alpha.
    float background[4];
    /// Corner radii: top-left, top-right, bottom-left, bottom-right.
    float corner_radius[4];
    /// Ignored by `GraphType::Pie` (no axes to reserve space for).
    float axis_margin_left;
    float axis_margin_bottom;
    float axis_margin_top;
    float axis_margin_right;

    /// Font tick labels, axis titles, and the legend are drawn with — same "id 0 is whatever the
    /// caller registered under id 0" convention `SturdyUiTextStyle::font_id` uses. Appended in ABI
    /// 0.21; `struct_size` still gates on the full struct, so a caller built against an older header
    /// gets `STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE` rather than these fields silently reading garbage.
    uint32_t font_id;
    uint32_t label_font_size;
    uint32_t title_font_size;
    /// Draws a small swatch+name legend, floating in the widget's top-right corner.
    SturdyBool show_legend;
} SturdyUiGraphDesc;

/// Fills a graph description with defaults: a Line chart with linear, autoscaled axes and the
/// engine's default colors/margins.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_desc_init(SturdyUiGraphDesc *desc);

/// Fills an axis config with defaults: linear scale, autoscaled, 6 target ticks, gridlines on.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_axis_init(SturdyUiGraphAxis *axis);

/// Draws a graph/plot widget as a leaf element inside the current UI frame, sized/positioned by
/// `element` the same way `sturdy_ui_begin_element`'s element is. See the engine-side
/// `SFT::UI::graph()`'s own doc comment for the bounds-latency contract (one frame behind a layout
/// change, same as every other bounds-dependent widget).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_draw(SturdyEngine engine,
                                                              const SturdyUiElement *element,
                                                              const SturdyUiGraphDesc *desc);

/// Creates an owned, incrementally-appendable data series — the ring-buffer convenience for live/
/// streaming telemetry.
///
/// @param capacity Bounds how many trailing `(x, y)` samples are kept; once full, the oldest
///        sample is dropped as a new one is pushed (a "tail -f" window). `0` means unbounded.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_create(SturdyEngine engine,
                                                                       uint32_t capacity,
                                                                       SturdyGraphSeries *out_series);

/// Appends one `(x, y)` sample to `series`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_push(SturdyGraphSeries series,
                                                                     double x,
                                                                     double y);

/// Removes every sample from `series`, without releasing it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_clear(SturdyGraphSeries series);

/// Reads back `series`' current samples as flat arrays, to bind into a
/// `SturdyUiGraphSeriesRef`'s `x_data`/`y_data`.
///
/// @return `out_x`/`out_y` are borrowed and valid only until the next call that mutates this
///         series (push/clear/release) on this thread — use them immediately, in the same
///         `sturdy_ui_graph_draw` call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_graph_series_data(SturdyGraphSeries series,
                                                                     const double **out_x,
                                                                     const double **out_y,
                                                                     uint32_t *out_count);

/// Releases a data series created with `sturdy_ui_graph_series_create`. Unknown/already-released
/// tokens are ignored.
STURDY_ABI void STURDY_ABI_CALL sturdy_ui_graph_series_release(SturdyGraphSeries series);

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

/// Platform-composited visual effect a window can request. Support is OS-dependent — an effect
/// this platform cannot render (e.g. Mica off Windows 11) is rejected with
/// `STURDY_ERROR_NOT_AVAILABLE` rather than silently ignored.
typedef enum SturdyWindowEffectKind {
    STURDY_WINDOW_EFFECT_BLUR = 0,
    STURDY_WINDOW_EFFECT_ACRYLIC = 1,
    STURDY_WINDOW_EFFECT_MICA = 2,
    STURDY_WINDOW_EFFECT_MICA_ALT = 3,
    STURDY_WINDOW_EFFECT_TABBED = 4,
    STURDY_WINDOW_EFFECT_DARK_MODE = 5,
    STURDY_WINDOW_EFFECT_BORDER_COLOR = 6,
    STURDY_WINDOW_EFFECT_CAPTION_COLOR = 7,
    STURDY_WINDOW_EFFECT_TEXT_COLOR = 8,
    STURDY_WINDOW_EFFECT_TRANSPARENT = 9,
    STURDY_WINDOW_EFFECT_FORCE_U32 = 0x7fffffff
} SturdyWindowEffectKind;

/// Screen-space hint for where an IME should draw its candidate/composition window, in the same
/// logical units as `SturdyWindowSnapshot::width`/`height`. `cursor_offset_x` is the caret's
/// offset from `x` within that area, for IMEs that anchor the popup to caret position rather than
/// the area's top-left corner.
typedef struct SturdyTextInputArea {
    float x;
    float y;
    float width;
    float height;
    float cursor_offset_x;
} SturdyTextInputArea;

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

/// Configuration for a new window, as handed to `sturdy_window_spawn` /
/// `sturdy_window_recreate_primary`. The window's graphics API always matches the engine's active
/// backend — there is no field for it here, since a second window using a different backend is not
/// something this engine supports.
typedef struct SturdyWindowConfig {
    /// Set to `sizeof(SturdyWindowConfig)` by `sturdy_window_config_init`.
    uint32_t struct_size;
    /// Copied by the engine before this call returns; the pointer need not outlive it. Null means
    /// "Sturdy Engine".
    const char *title;
    uint32_t width;
    uint32_t height;
    int32_t position_x;
    int32_t position_y;
    /// When true, `position_x`/`position_y` are ignored and the OS picks a placement.
    SturdyBool use_default_position;
    SturdyBool visible;
    SturdyBool resizable;
    SturdyBool decorated;
    /// Whether the window should render at the display's native pixel density rather than being
    /// upscaled. `SturdyWindowSnapshot::framebuffer_width/height` reflects the actual result.
    SturdyBool high_dpi;
    SturdyBool transparent;
    SturdyWindowMode mode;
} SturdyWindowConfig;

/// Fills `config` with the engine's default window configuration (1280x720, windowed, decorated,
/// visible, resizable, OS-placed).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_config_init(SturdyWindowConfig *config);

/// Requests a new, additional window.
///
/// Queued like every other request in this section: this only reports a bad engine handle,
/// argument, or struct-size mismatch. Whether the window was actually created is reported later
/// through `sturdy_window_take_completions`, keyed by `*out_request_id`.
///
/// @param out_request_id Receives an identifier for the queued request. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_spawn(SturdyEngine engine,
                                                             const SturdyWindowConfig *config,
                                                             uint64_t *out_request_id);

/// Requests that the primary window be destroyed and recreated with a new configuration —
/// changing decorations or transparency after creation on a platform where the window must be
/// recreated to do so, for instance.
///
/// Queued and completion-reported the same way as `sturdy_window_spawn`.
///
/// @param out_request_id Receives an identifier for the queued request. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_recreate_primary(SturdyEngine engine,
                                                                       const SturdyWindowConfig *config,
                                                                       uint64_t *out_request_id);

/// Which kind of window request a `SturdyWindowRequestCompletion` reports the outcome of.
typedef enum SturdyWindowRequestKind {
    STURDY_WINDOW_REQUEST_SPAWN = 0,
    STURDY_WINDOW_REQUEST_RECREATE_PRIMARY = 1,
    /// Also reported for `sturdy_window_request_close`, keyed by that call's own request id — most
    /// callers don't need to wait for it, since the window disappearing is its own confirmation.
    STURDY_WINDOW_REQUEST_CLOSE = 2,
    STURDY_WINDOW_REQUEST_FORCE_U32 = 0x7fffffff
} SturdyWindowRequestKind;

/// Outcome of one completed window request: `sturdy_window_spawn`, `sturdy_window_recreate_primary`,
/// or `sturdy_window_request_close`.
typedef struct SturdyWindowRequestCompletion {
    /// Matches the `out_request_id` the originating call produced.
    uint64_t request_id;
    SturdyWindowRequestKind kind;
    /// False when the request failed (window creation refused by the platform, runtime window
    /// management disabled, or a close naming a window that no longer exists); `surface` is not
    /// valid in that case.
    SturdyBool accepted;
    uint8_t reserved[3];
    SturdySurface surface;
} SturdyWindowRequestCompletion;

/// Copies up to `capacity` completed window requests into `out_completions` and reports the true
/// number available in `*out_count`, which may exceed `capacity` — completions beyond `capacity`
/// are dropped, not held for a later call, since window lifecycle events are rare enough that a
/// caller should simply pass a generously sized buffer (8 is enough for any realistic frame).
///
/// @param out_completions May be null only when `capacity` is 0, to just read `*out_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_take_completions(
    SturdyEngine engine,
    SturdyWindowRequestCompletion *out_completions,
    uint32_t capacity,
    uint32_t *out_count);

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

/// Requests relative mouse mode (the OS cursor is hidden and confined, and mouse motion reports
/// as unbounded deltas instead of absolute position) for a window — the mode a first-person
/// camera or any other mouse-look control needs. See `SturdyWindowSnapshot::mouse_locked` and
/// `sturdy_engine_mouse_delta` for reading the resulting state and motion.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_relative_mouse_mode(SturdyEngine engine,
                                                                              SturdySurface surface,
                                                                              SturdyBool enabled);

/// Requests that a window confine the cursor to its bounds while keeping it visible at an
/// absolute position — see `SturdyWindowSnapshot::mouse_locked` for the resulting state. Distinct
/// from `sturdy_window_set_relative_mouse_mode`, which also hides the cursor and switches to
/// unbounded motion deltas.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_mouse_locked(SturdyEngine engine,
                                                                       SturdySurface surface,
                                                                       SturdyBool locked);

/// Requests that a window grab (confine) or release the cursor at the OS level.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_cursor_grabbed(SturdyEngine engine,
                                                                         SturdySurface surface,
                                                                         SturdyBool grabbed);

/// Requests a platform-composited visual effect for a window (Windows' Mica/Acrylic/blur, etc.).
///
/// Queued like every other request in this section: this call only reports a bad engine handle or
/// argument. Whether the platform actually supports `kind` is decided when the request is applied,
/// and (like every other queued window request) that outcome is not currently reported back.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_effect(SturdyEngine engine,
                                                                 SturdySurface surface,
                                                                 SturdyWindowEffectKind kind,
                                                                 SturdyBool enabled);

/// Requests that a window start or stop IME text composition (on-screen keyboards, CJK input
/// methods, etc.). Most text fields need this active only while focused and editable.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_text_input_active(SturdyEngine engine,
                                                                            SturdySurface surface,
                                                                            SturdyBool active);

/// Hints the IME where to draw its candidate/composition window, in the window's logical units.
/// Only meaningful while text input is active (see `sturdy_window_set_text_input_active`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_set_text_input_area(SturdyEngine engine,
                                                                          SturdySurface surface,
                                                                          const SturdyTextInputArea *area);

// ---------------------------------------------------------------------------------------------
// RHI introspection
// ---------------------------------------------------------------------------------------------

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
// RHI resources
// ---------------------------------------------------------------------------------------------
//
// Buffers, textures, samplers, shader modules, bind groups, pipelines (including mesh/task-shader
// pipelines), recorded command buffers, command encoding (including indirect draws and mesh-task
// dispatches, with and without a GPU-supplied count), and render bundles. Ray tracing
// (acceleration structures, ray tracing pipelines, shader binding tables, opacity micromaps) has
// its own "Ray tracing" section below — it is not part of this resource surface either.
// (Swapchain/surface/HDR control has its own "Presentation and HDR" section above.) The RHI is
// now fully covered; a caller needing something finer-grained than this ABI exposes reaches for
// `sturdy_native_*` and drives the backend directly.
//
// Resource handles (`SturdyBuffer`, `SturdyTexture`, ...) carry no scope of their own — like
// `SturdySurface`, they are informational identifiers the RHI device owns and validates until
// the matching `sturdy_rhi_destroy_*` call. `SturdyCommandEncoder`, `SturdyRenderPassEncoder`
// and `SturdyComputePassEncoder` are the exception: they are ABI-owned handles (like
// `SturdyGltfScene`) that live until `finish()`/`end()` releases them, because encoding a pass
// is inherently a multi-call sequence a foreign caller must be able to hold across calls.

/// Pixel format for a texture, or a vertex/color-target format reference.
///
/// Ordinal values are pinned to match the engine's internal `RHI::Format` and must stay in this
/// exact order; new formats are appended, never inserted.
typedef enum SturdyFormat {
    STURDY_FORMAT_UNDEFINED = 0,
    STURDY_FORMAT_R8_UNORM,
    STURDY_FORMAT_R8_SNORM,
    STURDY_FORMAT_R8_UINT,
    STURDY_FORMAT_R8_SINT,
    STURDY_FORMAT_RG8_UNORM,
    STURDY_FORMAT_RG8_SNORM,
    STURDY_FORMAT_RG8_UINT,
    STURDY_FORMAT_RG8_SINT,
    STURDY_FORMAT_RGBA8_UNORM,
    STURDY_FORMAT_RGBA8_UNORM_SRGB,
    STURDY_FORMAT_RGBA8_SNORM,
    STURDY_FORMAT_RGBA8_UINT,
    STURDY_FORMAT_RGBA8_SINT,
    STURDY_FORMAT_BGRA8_UNORM,
    STURDY_FORMAT_BGRA8_UNORM_SRGB,
    STURDY_FORMAT_RGB10A2_UNORM,
    STURDY_FORMAT_RG11B10_FLOAT,
    STURDY_FORMAT_R16_UINT,
    STURDY_FORMAT_R16_SINT,
    STURDY_FORMAT_R16_FLOAT,
    STURDY_FORMAT_RG16_UINT,
    STURDY_FORMAT_RG16_SINT,
    STURDY_FORMAT_RG16_FLOAT,
    STURDY_FORMAT_RGBA16_UINT,
    STURDY_FORMAT_RGBA16_SINT,
    STURDY_FORMAT_RGBA16_FLOAT,
    STURDY_FORMAT_R32_UINT,
    STURDY_FORMAT_R32_SINT,
    STURDY_FORMAT_R32_FLOAT,
    STURDY_FORMAT_RG32_UINT,
    STURDY_FORMAT_RG32_SINT,
    STURDY_FORMAT_RG32_FLOAT,
    STURDY_FORMAT_RGBA32_UINT,
    STURDY_FORMAT_RGBA32_SINT,
    STURDY_FORMAT_RGBA32_FLOAT,
    STURDY_FORMAT_D16_UNORM,
    STURDY_FORMAT_D24_UNORM_S8_UINT,
    STURDY_FORMAT_D32_FLOAT,
    STURDY_FORMAT_D32_FLOAT_S8_UINT,
    STURDY_FORMAT_BC1_UNORM,
    STURDY_FORMAT_BC1_UNORM_SRGB,
    STURDY_FORMAT_BC3_UNORM,
    STURDY_FORMAT_BC3_UNORM_SRGB,
    STURDY_FORMAT_BC4_UNORM,
    STURDY_FORMAT_BC5_UNORM,
    STURDY_FORMAT_BC7_UNORM,
    STURDY_FORMAT_BC7_UNORM_SRGB,
    STURDY_FORMAT_FORCE_U32 = 0x7fffffff
} SturdyFormat;

/// Bitwise-OR'd usage flags for `SturdyBufferDesc::usage`.
typedef uint32_t SturdyBufferUsage;
#define STURDY_BUFFER_USAGE_NONE ((SturdyBufferUsage)0)
#define STURDY_BUFFER_USAGE_TRANSFER_SRC ((SturdyBufferUsage)(1u << 0))
#define STURDY_BUFFER_USAGE_TRANSFER_DST ((SturdyBufferUsage)(1u << 1))
#define STURDY_BUFFER_USAGE_VERTEX ((SturdyBufferUsage)(1u << 2))
#define STURDY_BUFFER_USAGE_INDEX ((SturdyBufferUsage)(1u << 3))
#define STURDY_BUFFER_USAGE_UNIFORM ((SturdyBufferUsage)(1u << 4))
#define STURDY_BUFFER_USAGE_STORAGE ((SturdyBufferUsage)(1u << 5))
#define STURDY_BUFFER_USAGE_INDIRECT ((SturdyBufferUsage)(1u << 6))

/// Where a buffer's backing memory lives.
typedef enum SturdyMemoryLocation {
    STURDY_MEMORY_LOCATION_DEVICE_LOCAL = 0,
    STURDY_MEMORY_LOCATION_HOST_UPLOAD = 1,
    STURDY_MEMORY_LOCATION_HOST_READBACK = 2,
    STURDY_MEMORY_LOCATION_FORCE_U32 = 0x7fffffff
} SturdyMemoryLocation;

/// Handle to a GPU buffer. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyBuffer {
    uint64_t id;
} SturdyBuffer;

/// Describes a buffer to create.
typedef struct SturdyBufferDesc {
    /// Set to `sizeof(SturdyBufferDesc)` by `sturdy_rhi_buffer_desc_init`.
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t size;
    /// Bitwise OR of `STURDY_BUFFER_USAGE_*`.
    SturdyBufferUsage usage;
    SturdyMemoryLocation memory;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyBufferDesc;

/// Fills `desc` with `struct_size` and engine defaults (`STURDY_MEMORY_LOCATION_DEVICE_LOCAL`,
/// no usage flags, zero size).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_buffer_desc_init(SturdyBufferDesc *desc);

/// Creates a buffer on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_buffer(SturdyEngine engine,
                                                                  const SturdyBufferDesc *desc,
                                                                  SturdyBuffer *out_buffer);

/// Destroys a buffer created by `sturdy_rhi_create_buffer`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_buffer(SturdyEngine engine, SturdyBuffer buffer);

/// Uploads `data` into `buffer` at `offset`, outside any command encoder. Suitable for
/// infrequent, host-visible writes; for per-frame or GPU-timed writes use
/// `sturdy_rhi_command_encoder_update_buffer` instead.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_write_buffer(SturdyEngine engine,
                                                                 SturdyBuffer buffer,
                                                                 uint64_t offset,
                                                                 const void *data,
                                                                 size_t data_size);

/// Maps `buffer` for host access. Only valid for a buffer created with a host-visible
/// `SturdyMemoryLocation`.
///
/// @param out_ptr Receives a pointer to the mapped range, valid until `sturdy_rhi_unmap_buffer`.
/// @param out_size Receives the mapped range's size in bytes. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_map_buffer(SturdyEngine engine,
                                                               SturdyBuffer buffer,
                                                               void **out_ptr,
                                                               size_t *out_size);

/// Unmaps a buffer previously mapped with `sturdy_rhi_map_buffer`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_unmap_buffer(SturdyEngine engine, SturdyBuffer buffer);

/// Reads the GPU virtual address of `buffer`. Requires the buffer to have been created with
/// storage/indirect/acceleration-structure-input usage that publishes a device address on the
/// active backend.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_buffer_device_address(SturdyEngine engine,
                                                                          SturdyBuffer buffer,
                                                                          uint64_t *out_address);

/// Dimensionality of a texture.
typedef enum SturdyTextureDimension {
    STURDY_TEXTURE_DIMENSION_1D = 0,
    STURDY_TEXTURE_DIMENSION_2D = 1,
    STURDY_TEXTURE_DIMENSION_3D = 2,
    STURDY_TEXTURE_DIMENSION_FORCE_U32 = 0x7fffffff
} SturdyTextureDimension;

/// Bitwise-OR'd usage flags for `SturdyTextureDesc::usage`.
typedef uint32_t SturdyTextureUsage;
#define STURDY_TEXTURE_USAGE_NONE ((SturdyTextureUsage)0)
#define STURDY_TEXTURE_USAGE_TRANSFER_SRC ((SturdyTextureUsage)(1u << 0))
#define STURDY_TEXTURE_USAGE_TRANSFER_DST ((SturdyTextureUsage)(1u << 1))
#define STURDY_TEXTURE_USAGE_SAMPLED ((SturdyTextureUsage)(1u << 2))
#define STURDY_TEXTURE_USAGE_STORAGE ((SturdyTextureUsage)(1u << 3))
#define STURDY_TEXTURE_USAGE_COLOR_ATTACHMENT ((SturdyTextureUsage)(1u << 4))
#define STURDY_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT ((SturdyTextureUsage)(1u << 5))

/// Handle to a GPU texture. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyTexture {
    uint64_t id;
} SturdyTexture;

/// Describes a texture to create.
typedef struct SturdyTextureDesc {
    /// Set to `sizeof(SturdyTextureDesc)` by `sturdy_rhi_texture_desc_init`.
    uint32_t struct_size;
    SturdyTextureDimension dimension;
    SturdyFormat format;
    uint32_t width;
    uint32_t height;
    /// Depth for a 3D texture, array layer count for a 1D/2D texture.
    uint32_t depth_or_layers;
    uint32_t mip_levels;
    /// MSAA sample count. Must be 1 for anything but a 2D color/depth-stencil attachment.
    uint32_t samples;
    /// Bitwise OR of `STURDY_TEXTURE_USAGE_*`.
    SturdyTextureUsage usage;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyTextureDesc;

/// Fills `desc` with `struct_size` and engine defaults (2D, `STURDY_FORMAT_UNDEFINED`, 1x1x1,
/// one mip level, one sample, no usage flags).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_texture_desc_init(SturdyTextureDesc *desc);

/// Creates a texture on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_texture(SturdyEngine engine,
                                                                   const SturdyTextureDesc *desc,
                                                                   SturdyTexture *out_texture);

/// Destroys a texture created by `sturdy_rhi_create_texture`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_texture(SturdyEngine engine, SturdyTexture texture);

/// How a texture view's subresources are addressed.
typedef enum SturdyTextureViewType {
    STURDY_TEXTURE_VIEW_TYPE_1D = 0,
    STURDY_TEXTURE_VIEW_TYPE_2D = 1,
    STURDY_TEXTURE_VIEW_TYPE_2D_ARRAY = 2,
    STURDY_TEXTURE_VIEW_TYPE_CUBE = 3,
    STURDY_TEXTURE_VIEW_TYPE_CUBE_ARRAY = 4,
    STURDY_TEXTURE_VIEW_TYPE_3D = 5,
    STURDY_TEXTURE_VIEW_TYPE_FORCE_U32 = 0x7fffffff
} SturdyTextureViewType;

/// Pass as `mip_level_count`/`array_layer_count` to cover every remaining level/layer from the
/// base one onward.
#define STURDY_ALL_REMAINING (~(uint32_t)0)

/// Handle to a texture view. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyTextureView {
    uint64_t id;
} SturdyTextureView;

/// Describes a texture view to create.
typedef struct SturdyTextureViewDesc {
    /// Set to `sizeof(SturdyTextureViewDesc)` by the caller.
    uint32_t struct_size;
    SturdyTextureViewType view_type;
    SturdyTexture texture;
    /// `STURDY_FORMAT_UNDEFINED` reuses the texture's own format.
    SturdyFormat format;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyTextureViewDesc;

/// Creates a texture view. `desc->texture` must be a live handle from
/// `sturdy_rhi_create_texture`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_texture_view(SturdyEngine engine,
                                                                        const SturdyTextureViewDesc *desc,
                                                                        SturdyTextureView *out_view);

/// Destroys a texture view created by `sturdy_rhi_create_texture_view`. A null/zero handle is a
/// no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_texture_view(SturdyEngine engine,
                                                                         SturdyTextureView view);

typedef enum SturdyFilter {
    STURDY_FILTER_NEAREST = 0,
    STURDY_FILTER_LINEAR = 1,
    STURDY_FILTER_FORCE_U32 = 0x7fffffff
} SturdyFilter;

typedef enum SturdyMipmapMode {
    STURDY_MIPMAP_MODE_NEAREST = 0,
    STURDY_MIPMAP_MODE_LINEAR = 1,
    STURDY_MIPMAP_MODE_FORCE_U32 = 0x7fffffff
} SturdyMipmapMode;

typedef enum SturdyAddressMode {
    STURDY_ADDRESS_MODE_REPEAT = 0,
    STURDY_ADDRESS_MODE_MIRRORED_REPEAT = 1,
    STURDY_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
    STURDY_ADDRESS_MODE_CLAMP_TO_BORDER = 3,
    STURDY_ADDRESS_MODE_FORCE_U32 = 0x7fffffff
} SturdyAddressMode;

typedef enum SturdyBorderColor {
    STURDY_BORDER_COLOR_TRANSPARENT_BLACK = 0,
    STURDY_BORDER_COLOR_OPAQUE_BLACK = 1,
    STURDY_BORDER_COLOR_OPAQUE_WHITE = 2,
    STURDY_BORDER_COLOR_FORCE_U32 = 0x7fffffff
} SturdyBorderColor;

/// Comparison used by depth tests, stencil tests, and comparison samplers.
typedef enum SturdyCompareOp {
    STURDY_COMPARE_OP_NEVER = 0,
    STURDY_COMPARE_OP_LESS = 1,
    STURDY_COMPARE_OP_EQUAL = 2,
    STURDY_COMPARE_OP_LESS_EQUAL = 3,
    STURDY_COMPARE_OP_GREATER = 4,
    STURDY_COMPARE_OP_NOT_EQUAL = 5,
    STURDY_COMPARE_OP_GREATER_EQUAL = 6,
    STURDY_COMPARE_OP_ALWAYS = 7,
    STURDY_COMPARE_OP_FORCE_U32 = 0x7fffffff
} SturdyCompareOp;

/// Handle to a sampler. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdySampler {
    uint64_t id;
} SturdySampler;

/// Describes a sampler to create.
typedef struct SturdySamplerDesc {
    /// Set to `sizeof(SturdySamplerDesc)` by `sturdy_rhi_sampler_desc_init`.
    uint32_t struct_size;
    SturdyFilter min_filter;
    SturdyFilter mag_filter;
    SturdyMipmapMode mipmap_mode;
    SturdyAddressMode address_u;
    SturdyAddressMode address_v;
    SturdyAddressMode address_w;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    /// Zero disables anisotropic filtering.
    float max_anisotropy;
    SturdyBool compare_enable;
    uint8_t reserved[3];
    SturdyCompareOp compare;
    SturdyBorderColor border_color;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdySamplerDesc;

/// Fills `desc` with `struct_size` and engine defaults (linear filtering/mipmapping, repeat
/// addressing, LOD range [0, 1000], no anisotropy, no comparison).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_sampler_desc_init(SturdySamplerDesc *desc);

/// Creates a sampler on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_sampler(SturdyEngine engine,
                                                                   const SturdySamplerDesc *desc,
                                                                   SturdySampler *out_sampler);

/// Destroys a sampler created by `sturdy_rhi_create_sampler`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_sampler(SturdyEngine engine, SturdySampler sampler);

/// Bytecode language a shader module was compiled to.
typedef enum SturdyShaderLanguage {
    STURDY_SHADER_LANGUAGE_SPIRV = 0,
    STURDY_SHADER_LANGUAGE_DXIL = 1,
    STURDY_SHADER_LANGUAGE_MSL = 2,
    STURDY_SHADER_LANGUAGE_WGSL = 3,
    STURDY_SHADER_LANGUAGE_FORCE_U32 = 0x7fffffff
} SturdyShaderLanguage;

/// Bitwise-OR'd shader stages. Used for pipeline entry points, bind-group-layout visibility, and
/// push-constant range visibility.
typedef uint32_t SturdyShaderStage;
#define STURDY_SHADER_STAGE_NONE ((SturdyShaderStage)0)
#define STURDY_SHADER_STAGE_VERTEX ((SturdyShaderStage)(1u << 0))
#define STURDY_SHADER_STAGE_FRAGMENT ((SturdyShaderStage)(1u << 1))
#define STURDY_SHADER_STAGE_COMPUTE ((SturdyShaderStage)(1u << 2))
#define STURDY_SHADER_STAGE_TASK ((SturdyShaderStage)(1u << 6))
#define STURDY_SHADER_STAGE_MESH ((SturdyShaderStage)(1u << 7))

/// Handle to a shader module compiled from raw backend bytecode (SPIR-V on Vulkan, DXIL on
/// D3D12). This is the low-level counterpart to `sturdy_render_load_shader`, which additionally
/// invokes the engine's Slang toolchain; use this one when bytecode was already compiled
/// offline. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyShaderModule {
    uint64_t id;
} SturdyShaderModule;

/// Describes a shader module to create from raw bytecode.
typedef struct SturdyShaderModuleDesc {
    /// Set to `sizeof(SturdyShaderModuleDesc)` by the caller.
    uint32_t struct_size;
    SturdyShaderLanguage language;
    const void *code;
    size_t code_size;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyShaderModuleDesc;

/// Creates a shader module. `desc->language` must match what the active backend consumes
/// (`STURDY_SHADER_LANGUAGE_SPIRV` for Vulkan, `STURDY_SHADER_LANGUAGE_DXIL` for D3D12); check
/// `sturdy_rhi_backend` first if targeting both.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_shader_module(SturdyEngine engine,
                                                                         const SturdyShaderModuleDesc *desc,
                                                                         SturdyShaderModule *out_module);

/// Destroys a shader module created by `sturdy_rhi_create_shader_module`. A null/zero handle is
/// a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_shader_module(SturdyEngine engine,
                                                                          SturdyShaderModule module);

/// What a bind-group-layout entry binds.
typedef enum SturdyBindingType {
    STURDY_BINDING_TYPE_UNIFORM_BUFFER = 0,
    STURDY_BINDING_TYPE_STORAGE_BUFFER = 1,
    STURDY_BINDING_TYPE_READ_ONLY_STORAGE_BUFFER = 2,
    STURDY_BINDING_TYPE_SAMPLED_TEXTURE = 3,
    STURDY_BINDING_TYPE_STORAGE_TEXTURE = 4,
    STURDY_BINDING_TYPE_SAMPLER = 5,
    STURDY_BINDING_TYPE_COMBINED_IMAGE_SAMPLER = 6,
    STURDY_BINDING_TYPE_ACCELERATION_STRUCTURE = 7,
    STURDY_BINDING_TYPE_INPUT_ATTACHMENT = 8,
    STURDY_BINDING_TYPE_FORCE_U32 = 0x7fffffff
} SturdyBindingType;

/// Bitwise-OR'd flags for `SturdyBindGroupLayoutEntry::flags`.
typedef uint32_t SturdyBindingFlags;
#define STURDY_BINDING_FLAGS_NONE ((SturdyBindingFlags)0)
#define STURDY_BINDING_FLAGS_PARTIALLY_BOUND ((SturdyBindingFlags)(1u << 0))
#define STURDY_BINDING_FLAGS_UPDATE_AFTER_BIND ((SturdyBindingFlags)(1u << 1))
#define STURDY_BINDING_FLAGS_VARIABLE_DESCRIPTOR_COUNT ((SturdyBindingFlags)(1u << 2))

/// One binding slot within a bind group layout.
typedef struct SturdyBindGroupLayoutEntry {
    uint32_t binding;
    /// D3D-style shader register this binding compiles to. Ignored on Vulkan; leave at
    /// `0xffffffff` (the default) when only targeting Vulkan.
    uint32_t shader_register;
    SturdyBindingType type;
    /// Bitwise OR of `STURDY_SHADER_STAGE_*`.
    SturdyShaderStage visibility;
    /// Array size for this binding; 1 for a non-array binding.
    uint32_t count;
    SturdyBool has_dynamic_offset;
    uint8_t reserved[3];
    /// Bitwise OR of `STURDY_BINDING_FLAGS_*`.
    SturdyBindingFlags flags;
    /// Only meaningful when `type` is `STURDY_BINDING_TYPE_INPUT_ATTACHMENT`.
    uint32_t input_attachment_index;
} SturdyBindGroupLayoutEntry;

/// Handle to a bind group layout. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyBindGroupLayout {
    uint64_t id;
} SturdyBindGroupLayout;

/// Describes a bind group layout to create.
typedef struct SturdyBindGroupLayoutDesc {
    /// Set to `sizeof(SturdyBindGroupLayoutDesc)` by the caller.
    uint32_t struct_size;
    uint32_t entry_count;
    const SturdyBindGroupLayoutEntry *entries;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyBindGroupLayoutDesc;

/// Creates a bind group layout. `desc->entries` is read synchronously and need not outlive the
/// call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_bind_group_layout(
    SturdyEngine engine, const SturdyBindGroupLayoutDesc *desc, SturdyBindGroupLayout *out_layout);

/// Destroys a bind group layout created by `sturdy_rhi_create_bind_group_layout`. A null/zero
/// handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_bind_group_layout(SturdyEngine engine,
                                                                              SturdyBindGroupLayout layout);

/// One resource binding within a bind group. Set the field matching `binding`'s type in the
/// layout it targets; unused fields are ignored.
typedef struct SturdyBindGroupEntry {
    uint32_t binding;
    /// Array element within the binding; 0 for a non-array binding.
    uint32_t array_element;
    SturdyBuffer buffer;
    uint64_t offset;
    /// Zero means the buffer's remaining size from `offset`.
    uint64_t size;
    /// Structured-buffer element stride. Only meaningful for a storage-buffer binding on
    /// backends that need it (D3D12 structured buffers); leave 0 otherwise.
    uint32_t structure_stride;
    uint32_t reserved;
    SturdyTextureView texture_view;
    SturdySampler sampler;
} SturdyBindGroupEntry;

/// Handle to a bind group. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyBindGroup {
    uint64_t id;
} SturdyBindGroup;

/// Describes a bind group to create.
typedef struct SturdyBindGroupDesc {
    /// Set to `sizeof(SturdyBindGroupDesc)` by the caller.
    uint32_t struct_size;
    SturdyBindGroupLayout layout;
    uint32_t entry_count;
    const SturdyBindGroupEntry *entries;
    /// Element count to reserve for the layout's trailing variable-count binding, if any.
    /// Zero when the layout has none.
    uint32_t variable_descriptor_count;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyBindGroupDesc;

/// Creates a bind group. `desc->entries` is read synchronously and need not outlive the call.
/// The group is created persistent (not frame-transient); destroy it explicitly when done.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_bind_group(SturdyEngine engine,
                                                                      const SturdyBindGroupDesc *desc,
                                                                      SturdyBindGroup *out_group);

/// Destroys a bind group created by `sturdy_rhi_create_bind_group`. A null/zero handle is a
/// no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_bind_group(SturdyEngine engine, SturdyBindGroup group);

/// One push-constant range within a pipeline layout.
typedef struct SturdyPushConstantRange {
    /// Bitwise OR of `STURDY_SHADER_STAGE_*`.
    SturdyShaderStage stages;
    uint32_t offset;
    uint32_t size;
    /// D3D-style register/space this range compiles to. Ignored on Vulkan.
    uint32_t shader_register;
    uint32_t register_space;
} SturdyPushConstantRange;

/// Handle to a pipeline layout. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyPipelineLayout {
    uint64_t id;
} SturdyPipelineLayout;

/// Describes a pipeline layout to create.
typedef struct SturdyPipelineLayoutDesc {
    /// Set to `sizeof(SturdyPipelineLayoutDesc)` by the caller.
    uint32_t struct_size;
    uint32_t bind_group_layout_count;
    const SturdyBindGroupLayout *bind_group_layouts;
    uint32_t push_constant_range_count;
    uint32_t reserved;
    const SturdyPushConstantRange *push_constant_ranges;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyPipelineLayoutDesc;

/// Creates a pipeline layout. `desc->bind_group_layouts`/`push_constant_ranges` are read
/// synchronously and need not outlive the call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_pipeline_layout(
    SturdyEngine engine, const SturdyPipelineLayoutDesc *desc, SturdyPipelineLayout *out_layout);

/// Destroys a pipeline layout created by `sturdy_rhi_create_pipeline_layout`. A null/zero handle
/// is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_pipeline_layout(SturdyEngine engine,
                                                                            SturdyPipelineLayout layout);

/// Per-vertex-attribute data layout. Ordinal values are pinned to match the engine's internal
/// `RHI::VertexFormat` and must stay in this exact order.
typedef enum SturdyVertexFormat {
    STURDY_VERTEX_FORMAT_FLOAT32 = 0,
    STURDY_VERTEX_FORMAT_FLOAT32X2,
    STURDY_VERTEX_FORMAT_FLOAT32X3,
    STURDY_VERTEX_FORMAT_FLOAT32X4,
    STURDY_VERTEX_FORMAT_UINT32,
    STURDY_VERTEX_FORMAT_UINT32X2,
    STURDY_VERTEX_FORMAT_UINT32X3,
    STURDY_VERTEX_FORMAT_UINT32X4,
    STURDY_VERTEX_FORMAT_SINT32,
    STURDY_VERTEX_FORMAT_SINT32X2,
    STURDY_VERTEX_FORMAT_SINT32X3,
    STURDY_VERTEX_FORMAT_SINT32X4,
    STURDY_VERTEX_FORMAT_UINT8X4_UNORM,
    STURDY_VERTEX_FORMAT_SINT8X4_NORM,
    STURDY_VERTEX_FORMAT_UINT16X2_UNORM,
    STURDY_VERTEX_FORMAT_UINT16X4_UNORM,
    STURDY_VERTEX_FORMAT_FLOAT16X2,
    STURDY_VERTEX_FORMAT_FLOAT16X4,
    STURDY_VERTEX_FORMAT_FORCE_U32 = 0x7fffffff
} SturdyVertexFormat;

typedef enum SturdyVertexStepMode {
    STURDY_VERTEX_STEP_MODE_VERTEX = 0,
    STURDY_VERTEX_STEP_MODE_INSTANCE = 1,
    STURDY_VERTEX_STEP_MODE_FORCE_U32 = 0x7fffffff
} SturdyVertexStepMode;

/// One attribute within a vertex buffer layout.
typedef struct SturdyVertexAttribute {
    SturdyVertexFormat format;
    uint32_t offset;
    uint32_t shader_location;
    /// D3D-style input semantic name matching the shader's declared input (e.g. "NORMAL").
    /// Ignored on Vulkan, which matches by `shader_location` alone. Defaults to "TEXCOORD" if
    /// left null.
    const char *semantic_name;
    uint32_t semantic_index;
} SturdyVertexAttribute;

/// Describes one vertex buffer's layout within a render pipeline.
typedef struct SturdyVertexBufferLayout {
    uint64_t stride;
    SturdyVertexStepMode step_mode;
    uint32_t attribute_count;
    const SturdyVertexAttribute *attributes;
} SturdyVertexBufferLayout;

/// A shader module plus the stage and entry point a pipeline invokes it at.
typedef struct SturdyShaderEntry {
    SturdyShaderModule module;
    /// Defaults to "main" if left null.
    const char *entry_point;
    SturdyShaderStage stage;
} SturdyShaderEntry;

typedef enum SturdyPrimitiveTopology {
    STURDY_PRIMITIVE_TOPOLOGY_POINT_LIST = 0,
    STURDY_PRIMITIVE_TOPOLOGY_LINE_LIST = 1,
    STURDY_PRIMITIVE_TOPOLOGY_LINE_STRIP = 2,
    STURDY_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
    STURDY_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4,
    STURDY_PRIMITIVE_TOPOLOGY_FORCE_U32 = 0x7fffffff
} SturdyPrimitiveTopology;

typedef enum SturdyPolygonMode {
    STURDY_POLYGON_MODE_FILL = 0,
    /// Wireframe. A device feature on some backends; check before relying on it.
    STURDY_POLYGON_MODE_LINE = 1,
    STURDY_POLYGON_MODE_POINT = 2,
    STURDY_POLYGON_MODE_FORCE_U32 = 0x7fffffff
} SturdyPolygonMode;

typedef enum SturdyCullMode {
    STURDY_CULL_MODE_NONE = 0,
    STURDY_CULL_MODE_FRONT = 1,
    STURDY_CULL_MODE_BACK = 2,
    STURDY_CULL_MODE_FORCE_U32 = 0x7fffffff
} SturdyCullMode;

/// Winding considered front-facing by the rasterizer.
typedef enum SturdyFrontFace {
    STURDY_FRONT_FACE_COUNTER_CLOCKWISE = 0,
    STURDY_FRONT_FACE_CLOCKWISE = 1,
    STURDY_FRONT_FACE_FORCE_U32 = 0x7fffffff
} SturdyFrontFace;

/// Rasterizer fixed-function state.
typedef struct SturdyRasterizationState {
    SturdyPolygonMode polygon_mode;
    SturdyCullMode cull_mode;
    SturdyFrontFace front_face;
    SturdyBool depth_clamp_enable;
    uint8_t reserved[3];
    float depth_bias_constant;
    float depth_bias_slope_scale;
    float depth_bias_clamp;
    float line_width;
} SturdyRasterizationState;

typedef enum SturdyStencilOp {
    STURDY_STENCIL_OP_KEEP = 0,
    STURDY_STENCIL_OP_ZERO = 1,
    STURDY_STENCIL_OP_REPLACE = 2,
    STURDY_STENCIL_OP_INCREMENT_CLAMP = 3,
    STURDY_STENCIL_OP_DECREMENT_CLAMP = 4,
    STURDY_STENCIL_OP_INVERT = 5,
    STURDY_STENCIL_OP_INCREMENT_WRAP = 6,
    STURDY_STENCIL_OP_DECREMENT_WRAP = 7,
    STURDY_STENCIL_OP_FORCE_U32 = 0x7fffffff
} SturdyStencilOp;

/// Stencil behavior for one face (front or back).
typedef struct SturdyStencilFaceState {
    SturdyStencilOp fail_op;
    SturdyStencilOp depth_fail_op;
    SturdyStencilOp pass_op;
    SturdyCompareOp compare;
} SturdyStencilFaceState;

/// Depth/stencil fixed-function state. `format` names the attachment this pipeline targets;
/// `STURDY_FORMAT_UNDEFINED` means the pipeline has no depth/stencil attachment.
typedef struct SturdyDepthStencilState {
    SturdyFormat format;
    SturdyBool depth_test_enable;
    SturdyBool depth_write_enable;
    uint8_t reserved[2];
    SturdyCompareOp depth_compare;
    SturdyBool stencil_test_enable;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    uint8_t reserved2;
    SturdyStencilFaceState stencil_front;
    SturdyStencilFaceState stencil_back;
} SturdyDepthStencilState;

/// Multisample fixed-function state.
typedef struct SturdyMultisampleState {
    uint32_t samples;
    uint32_t sample_mask;
    SturdyBool alpha_to_coverage_enable;
    uint8_t reserved[3];
} SturdyMultisampleState;

typedef enum SturdyBlendFactor {
    STURDY_BLEND_FACTOR_ZERO = 0,
    STURDY_BLEND_FACTOR_ONE = 1,
    STURDY_BLEND_FACTOR_SRC_COLOR = 2,
    STURDY_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3,
    STURDY_BLEND_FACTOR_DST_COLOR = 4,
    STURDY_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 5,
    STURDY_BLEND_FACTOR_SRC_ALPHA = 6,
    STURDY_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
    STURDY_BLEND_FACTOR_DST_ALPHA = 8,
    STURDY_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9,
    STURDY_BLEND_FACTOR_CONSTANT_COLOR = 10,
    STURDY_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR = 11,
    STURDY_BLEND_FACTOR_SRC_ALPHA_SATURATED = 12,
    STURDY_BLEND_FACTOR_FORCE_U32 = 0x7fffffff
} SturdyBlendFactor;

typedef enum SturdyBlendOp {
    STURDY_BLEND_OP_ADD = 0,
    STURDY_BLEND_OP_SUBTRACT = 1,
    STURDY_BLEND_OP_REVERSE_SUBTRACT = 2,
    STURDY_BLEND_OP_MIN = 3,
    STURDY_BLEND_OP_MAX = 4,
    STURDY_BLEND_OP_FORCE_U32 = 0x7fffffff
} SturdyBlendOp;

/// Bitwise-OR'd color channel mask for `SturdyColorTargetState::write_mask`.
typedef uint32_t SturdyColorWriteMask;
#define STURDY_COLOR_WRITE_MASK_NONE ((SturdyColorWriteMask)0)
#define STURDY_COLOR_WRITE_MASK_RED ((SturdyColorWriteMask)(1u << 0))
#define STURDY_COLOR_WRITE_MASK_GREEN ((SturdyColorWriteMask)(1u << 1))
#define STURDY_COLOR_WRITE_MASK_BLUE ((SturdyColorWriteMask)(1u << 2))
#define STURDY_COLOR_WRITE_MASK_ALPHA ((SturdyColorWriteMask)(1u << 3))
#define STURDY_COLOR_WRITE_MASK_ALL \
    ((SturdyColorWriteMask)(STURDY_COLOR_WRITE_MASK_RED | STURDY_COLOR_WRITE_MASK_GREEN | \
                             STURDY_COLOR_WRITE_MASK_BLUE | STURDY_COLOR_WRITE_MASK_ALPHA))

/// One color attachment's format and blend/write behavior. `blend_enable` false means a
/// straight overwrite; the blend fields are then ignored.
typedef struct SturdyColorTargetState {
    SturdyFormat format;
    SturdyBool blend_enable;
    uint8_t reserved[3];
    SturdyBlendFactor color_src_factor;
    SturdyBlendFactor color_dst_factor;
    SturdyBlendOp color_op;
    SturdyBlendFactor alpha_src_factor;
    SturdyBlendFactor alpha_dst_factor;
    SturdyBlendOp alpha_op;
    /// Bitwise OR of `STURDY_COLOR_WRITE_MASK_*`.
    SturdyColorWriteMask write_mask;
} SturdyColorTargetState;

/// Handle to a render pipeline. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyRenderPipeline {
    uint64_t id;
} SturdyRenderPipeline;

/// Everything needed to build a raster pipeline against dynamic rendering (no render-pass
/// object). Viewport/scissor are always dynamic, set per-draw on the render pass encoder, so
/// they are not part of this description. All array fields are read synchronously during
/// `sturdy_rhi_create_render_pipeline` and need not outlive the call.
///
/// Set `mesh` (and optionally `task`) for a mesh-shader pipeline, or set `vertex` for a
/// traditional vertex-input pipeline — not both. A mesh pipeline requires the `"mesh shaders"`
/// feature (check with `sturdy_rhi_feature_index`/`sturdy_rhi_feature_enabled`) and ignores
/// `vertex_buffers`/`topology`; a task stage additionally requires `"task/amplification shaders"`.
typedef struct SturdyRenderPipelineDesc {
    /// Set to `sizeof(SturdyRenderPipelineDesc)` by `sturdy_rhi_render_pipeline_desc_init`.
    uint32_t struct_size;
    SturdyPipelineLayout layout;
    SturdyShaderEntry vertex;
    /// Module may be the zero handle for a depth-only pipeline.
    SturdyShaderEntry fragment;
    uint32_t vertex_buffer_count;
    uint32_t reserved;
    const SturdyVertexBufferLayout *vertex_buffers;
    SturdyPrimitiveTopology topology;
    SturdyRasterizationState rasterization;
    SturdyMultisampleState multisample;
    /// `format` field `STURDY_FORMAT_UNDEFINED` means no depth/stencil attachment.
    SturdyDepthStencilState depth_stencil;
    uint32_t color_target_count;
    uint32_t reserved2;
    const SturdyColorTargetState *color_targets;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
    /// Amplification/task stage for a mesh-shader pipeline. Zero handle means no task stage.
    SturdyShaderEntry task;
    /// Mesh stage. A valid (nonzero) module here selects the mesh-shader pipeline path instead
    /// of `vertex`.
    SturdyShaderEntry mesh;
} SturdyRenderPipelineDesc;

/// Fills `desc` with `struct_size` and engine defaults (triangle list, fill/back-cull/CCW
/// rasterization, no multisampling, no depth/stencil, no color targets, zeroed shader entries).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pipeline_desc_init(SturdyRenderPipelineDesc *desc);

/// Creates a render pipeline on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_render_pipeline(
    SturdyEngine engine, const SturdyRenderPipelineDesc *desc, SturdyRenderPipeline *out_pipeline);

/// Destroys a render pipeline created by `sturdy_rhi_create_render_pipeline`. A null/zero handle
/// is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_render_pipeline(SturdyEngine engine,
                                                                            SturdyRenderPipeline pipeline);

/// Handle to a compute pipeline. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyComputePipeline {
    uint64_t id;
} SturdyComputePipeline;

/// Describes a compute pipeline to create.
typedef struct SturdyComputePipelineDesc {
    /// Set to `sizeof(SturdyComputePipelineDesc)` by the caller.
    uint32_t struct_size;
    SturdyPipelineLayout layout;
    SturdyShaderEntry compute;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyComputePipelineDesc;

/// Creates a compute pipeline on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_compute_pipeline(
    SturdyEngine engine, const SturdyComputePipelineDesc *desc, SturdyComputePipeline *out_pipeline);

/// Destroys a compute pipeline created by `sturdy_rhi_create_compute_pipeline`. A null/zero
/// handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_compute_pipeline(SturdyEngine engine,
                                                                             SturdyComputePipeline pipeline);

/// Load/store behavior for a render pass attachment.
typedef enum SturdyLoadOp {
    STURDY_LOAD_OP_LOAD = 0,
    STURDY_LOAD_OP_CLEAR = 1,
    STURDY_LOAD_OP_DONT_CARE = 2,
    STURDY_LOAD_OP_FORCE_U32 = 0x7fffffff
} SturdyLoadOp;

typedef enum SturdyStoreOp {
    STURDY_STORE_OP_STORE = 0,
    STURDY_STORE_OP_DONT_CARE = 1,
    STURDY_STORE_OP_FORCE_U32 = 0x7fffffff
} SturdyStoreOp;

typedef enum SturdyIndexFormat {
    STURDY_INDEX_FORMAT_UINT16 = 0,
    STURDY_INDEX_FORMAT_UINT32 = 1,
    STURDY_INDEX_FORMAT_FORCE_U32 = 0x7fffffff
} SturdyIndexFormat;

/// Rectangle in pixel space, top-left origin — used for both scissor rects and render-pass
/// render areas.
typedef struct SturdyRect2D {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} SturdyRect2D;

/// Floating-point viewport, top-left origin. See `RHI::Viewport`'s engine-side documentation for
/// the clip-space convention this maps into — it is identical on every backend from a shader's
/// point of view, so nothing here needs backend-specific handling.
typedef struct SturdyViewport {
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} SturdyViewport;

/// One color attachment within a render pass.
typedef struct SturdyColorAttachment {
    SturdyTextureView view;
    /// Zero handle disables MSAA resolve for this attachment.
    SturdyTextureView resolve_view;
    SturdyLoadOp load_op;
    SturdyStoreOp store_op;
    float clear_color[4];
} SturdyColorAttachment;

/// The depth/stencil attachment within a render pass. `has_view` false means the pass has no
/// depth/stencil attachment and the rest of this struct is ignored.
typedef struct SturdyDepthStencilAttachment {
    SturdyBool has_view;
    uint8_t reserved[3];
    SturdyTextureView view;
    SturdyTextureView resolve_view;
    SturdyLoadOp depth_load_op;
    SturdyStoreOp depth_store_op;
    SturdyLoadOp stencil_load_op;
    SturdyStoreOp stencil_store_op;
    float clear_depth;
    uint32_t clear_stencil;
} SturdyDepthStencilAttachment;

/// Describes a render pass to begin.
typedef struct SturdyRenderPassDesc {
    /// Set to `sizeof(SturdyRenderPassDesc)` by `sturdy_rhi_render_pass_desc_init`.
    uint32_t struct_size;
    uint32_t color_attachment_count;
    const SturdyColorAttachment *color_attachments;
    SturdyDepthStencilAttachment depth_stencil;
    SturdyRect2D render_area;
    /// Must be true to call `sturdy_rhi_render_pass_execute_bundles` on the pass this creates.
    SturdyBool allow_bundles;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyRenderPassDesc;

/// Fills `desc` with `struct_size` and zeroed contents (no color attachments, no depth/stencil
/// attachment, zero render area — set `color_attachments`/`render_area` before use).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_desc_init(SturdyRenderPassDesc *desc);

/// Owned handle to an in-progress command recording, borrowed from the engine's active device.
/// Lives until `sturdy_rhi_command_encoder_finish` or `sturdy_rhi_command_encoder_release`.
typedef struct SturdyCommandEncoder {
    uint64_t token;
} SturdyCommandEncoder;

/// Describes a command encoder to create.
typedef struct SturdyCommandEncoderDesc {
    /// Set to `sizeof(SturdyCommandEncoderDesc)` by `sturdy_rhi_command_encoder_desc_init`.
    uint32_t struct_size;
    SturdyQueueClass queue_class;
    uint32_t queue_lane_index;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyCommandEncoderDesc;

/// Fills `desc` with `struct_size` and engine defaults (graphics queue, lane 0).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_desc_init(SturdyCommandEncoderDesc *desc);

/// Begins recording a new command buffer.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_command_encoder(
    SturdyEngine engine, const SturdyCommandEncoderDesc *desc, SturdyCommandEncoder *out_encoder);

/// Abandons an in-progress recording without submitting it. Use `sturdy_rhi_command_encoder_finish`
/// instead when the recorded commands should actually run.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_release(SturdyCommandEncoder encoder);

/// Handle to a finished, submittable command buffer. See the "RHI resources" section header for
/// its lifetime rules.
typedef struct SturdyCommandBuffer {
    uint64_t id;
} SturdyCommandBuffer;

/// Ends recording and produces a submittable command buffer. `encoder` is consumed: its token is
/// invalid after this call, whether it succeeds or fails.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_finish(SturdyCommandEncoder encoder,
                                                                           SturdyCommandBuffer *out_buffer);

/// Destroys a command buffer produced by `sturdy_rhi_command_encoder_finish`, whether or not it
/// was submitted. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_command_buffer(SturdyEngine engine,
                                                                           SturdyCommandBuffer buffer);

/// One buffer-to-buffer copy region.
typedef struct SturdyBufferCopy {
    uint64_t src_offset;
    uint64_t dst_offset;
    uint64_t size;
} SturdyBufferCopy;

/// Copies bytes between two buffers.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_buffer_to_buffer(
    SturdyCommandEncoder encoder, SturdyBuffer src, SturdyBuffer dst, const SturdyBufferCopy *region);

/// Describes one buffer<->texture copy region.
///
/// @note The buffer-side row pitch in bytes (the tightly-packed value is `extent_width` times the
///       format's texel size, or `buffer_row_length` times it when nonzero) must be a multiple of
///       256 on D3D12 (`D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`) — Vulkan has no such requirement, but
///       this is required for portability across both backends. Verified empirically: a 4-texel
///       (16-byte) row succeeds on Vulkan and reports failure on D3D12.
typedef struct SturdyBufferTextureCopy {
    uint64_t buffer_offset;
    /// Row length in texels; 0 means tightly packed.
    uint32_t buffer_row_length;
    /// Image height in rows; 0 means tightly packed.
    uint32_t buffer_image_height;
    uint32_t mip_level;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    int32_t texture_x;
    int32_t texture_y;
    int32_t texture_z;
    uint32_t extent_width;
    uint32_t extent_height;
    uint32_t extent_depth_or_layers;
} SturdyBufferTextureCopy;

/// Copies bytes from a buffer into a texture's subresources.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_buffer_to_texture(
    SturdyCommandEncoder encoder, SturdyBuffer src, SturdyTexture dst, const SturdyBufferTextureCopy *region);

/// Copies texels from a texture's subresources into a buffer.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_texture_to_buffer(
    SturdyCommandEncoder encoder, SturdyTexture src, SturdyBuffer dst, const SturdyBufferTextureCopy *region);

/// One texture-to-texture copy region. Source and destination subresources must have the same
/// dimensions.
typedef struct SturdyTextureCopy {
    uint32_t src_mip_level;
    uint32_t src_base_array_layer;
    uint32_t src_array_layer_count;
    int32_t src_x;
    int32_t src_y;
    int32_t src_z;
    uint32_t dst_mip_level;
    uint32_t dst_base_array_layer;
    uint32_t dst_array_layer_count;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_z;
    uint32_t extent_width;
    uint32_t extent_height;
    uint32_t extent_depth_or_layers;
} SturdyTextureCopy;

/// Copies texels between two textures' subresources.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_texture_to_texture(
    SturdyCommandEncoder encoder, SturdyTexture src, SturdyTexture dst, const SturdyTextureCopy *region);

/// Fills a byte range of `buffer` with repeated copies of `value`, outside any render/compute
/// pass.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_fill_buffer(
    SturdyCommandEncoder encoder, SturdyBuffer buffer, uint64_t offset, uint64_t size, uint32_t value);

/// Records a GPU-timed write of `data` into `buffer` at `offset`, outside any render/compute
/// pass. Prefer this over `sturdy_rhi_write_buffer` for a write that must be ordered against
/// other commands in the same encoder.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_update_buffer(
    SturdyCommandEncoder encoder, SturdyBuffer buffer, uint64_t offset, const void *data, size_t data_size);

/// Identifies the mip/array subrange a clear or barrier applies to.
typedef struct SturdyTextureSubresourceRange {
    uint32_t base_mip_level;
    /// `STURDY_ALL_REMAINING` covers every remaining mip level.
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    /// `STURDY_ALL_REMAINING` covers every remaining array layer.
    uint32_t array_layer_count;
} SturdyTextureSubresourceRange;

/// Clears a color texture's subresources to a solid color, outside any render pass.
///
/// @note `texture` must have been created with `STURDY_TEXTURE_USAGE_COLOR_ATTACHMENT`. Vulkan's
///       `vkCmdClearColorImage` has no such requirement, but D3D12 clears through a render-target
///       view, so this is required for portability across both backends even though only one
///       enforces it — verified empirically against both.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_clear_color_texture(
    SturdyCommandEncoder encoder, SturdyTexture texture, const float color[4],
    const SturdyTextureSubresourceRange *range);

/// Clears a depth/stencil texture's subresources, outside any render pass.
///
/// @note `texture` should be created with `STURDY_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT`; by
///       symmetry with `sturdy_rhi_command_encoder_clear_color_texture` above, D3D12 likely clears
///       through a depth-stencil view the same way, though this has not been independently
///       verified against both backends the way the color case was.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_clear_depth_stencil_texture(
    SturdyCommandEncoder encoder, SturdyTexture texture, float depth, uint32_t stencil,
    const SturdyTextureSubresourceRange *range);

/// Pipeline stages a barrier waits on/blocks. Bitwise OR of `STURDY_PIPELINE_STAGE_*`; wider than
/// 32 bits, so these are macros rather than a C enum.
typedef uint64_t SturdyPipelineStage;
#define STURDY_PIPELINE_STAGE_NONE ((SturdyPipelineStage)0)
#define STURDY_PIPELINE_STAGE_DRAW_INDIRECT ((SturdyPipelineStage)(1ull << 0))
#define STURDY_PIPELINE_STAGE_VERTEX_INPUT ((SturdyPipelineStage)(1ull << 1))
#define STURDY_PIPELINE_STAGE_VERTEX_SHADER ((SturdyPipelineStage)(1ull << 2))
#define STURDY_PIPELINE_STAGE_FRAGMENT_SHADER ((SturdyPipelineStage)(1ull << 6))
#define STURDY_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS ((SturdyPipelineStage)(1ull << 7))
#define STURDY_PIPELINE_STAGE_LATE_FRAGMENT_TESTS ((SturdyPipelineStage)(1ull << 8))
#define STURDY_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT ((SturdyPipelineStage)(1ull << 9))
#define STURDY_PIPELINE_STAGE_COMPUTE_SHADER ((SturdyPipelineStage)(1ull << 10))
#define STURDY_PIPELINE_STAGE_TRANSFER ((SturdyPipelineStage)(1ull << 11))
#define STURDY_PIPELINE_STAGE_HOST ((SturdyPipelineStage)(1ull << 12))
#define STURDY_PIPELINE_STAGE_ALL_COMMANDS ((SturdyPipelineStage)~0ull)

/// Memory access kinds a barrier synchronizes. Bitwise OR of `STURDY_ACCESS_*`; wider than 32
/// bits, so these are macros rather than a C enum.
typedef uint64_t SturdyAccessFlags;
#define STURDY_ACCESS_NONE ((SturdyAccessFlags)0)
#define STURDY_ACCESS_INDIRECT_COMMAND_READ ((SturdyAccessFlags)(1ull << 0))
#define STURDY_ACCESS_INDEX_READ ((SturdyAccessFlags)(1ull << 1))
#define STURDY_ACCESS_VERTEX_ATTRIBUTE_READ ((SturdyAccessFlags)(1ull << 2))
#define STURDY_ACCESS_UNIFORM_READ ((SturdyAccessFlags)(1ull << 3))
#define STURDY_ACCESS_SHADER_READ ((SturdyAccessFlags)(1ull << 4))
#define STURDY_ACCESS_SHADER_WRITE ((SturdyAccessFlags)(1ull << 5))
#define STURDY_ACCESS_COLOR_ATTACHMENT_READ ((SturdyAccessFlags)(1ull << 6))
#define STURDY_ACCESS_COLOR_ATTACHMENT_WRITE ((SturdyAccessFlags)(1ull << 7))
#define STURDY_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ ((SturdyAccessFlags)(1ull << 8))
#define STURDY_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE ((SturdyAccessFlags)(1ull << 9))
#define STURDY_ACCESS_TRANSFER_READ ((SturdyAccessFlags)(1ull << 10))
#define STURDY_ACCESS_TRANSFER_WRITE ((SturdyAccessFlags)(1ull << 11))
#define STURDY_ACCESS_HOST_READ ((SturdyAccessFlags)(1ull << 12))
#define STURDY_ACCESS_HOST_WRITE ((SturdyAccessFlags)(1ull << 13))
#define STURDY_ACCESS_MEMORY_READ ((SturdyAccessFlags)(1ull << 16))
#define STURDY_ACCESS_MEMORY_WRITE ((SturdyAccessFlags)(1ull << 17))

/// A pipeline barrier with no specific resource — orders every matching access before/after it.
typedef struct SturdyGlobalBarrier {
    SturdyPipelineStage src_stage;
    SturdyAccessFlags src_access;
    SturdyPipelineStage dst_stage;
    SturdyAccessFlags dst_access;
} SturdyGlobalBarrier;

/// A pipeline barrier scoped to one buffer range.
typedef struct SturdyBufferBarrier {
    SturdyBuffer buffer;
    SturdyPipelineStage src_stage;
    SturdyAccessFlags src_access;
    SturdyPipelineStage dst_stage;
    SturdyAccessFlags dst_access;
    uint64_t offset;
    /// Zero means the buffer's remaining size from `offset`.
    uint64_t size;
} SturdyBufferBarrier;

/// Layout a texture is transitioned to/from by a barrier.
typedef enum SturdyTextureLayout {
    STURDY_TEXTURE_LAYOUT_UNDEFINED = 0,
    STURDY_TEXTURE_LAYOUT_GENERAL = 1,
    STURDY_TEXTURE_LAYOUT_COLOR_ATTACHMENT = 2,
    STURDY_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT = 3,
    STURDY_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY = 4,
    STURDY_TEXTURE_LAYOUT_SHADER_READ_ONLY = 5,
    STURDY_TEXTURE_LAYOUT_TRANSFER_SRC = 6,
    STURDY_TEXTURE_LAYOUT_TRANSFER_DST = 7,
    STURDY_TEXTURE_LAYOUT_PRESENT = 8,
    STURDY_TEXTURE_LAYOUT_FORCE_U32 = 0x7fffffff
} SturdyTextureLayout;

/// A pipeline barrier scoped to one texture's subresources, optionally also transitioning its
/// layout.
typedef struct SturdyTextureBarrier {
    SturdyTexture texture;
    SturdyPipelineStage src_stage;
    SturdyAccessFlags src_access;
    SturdyPipelineStage dst_stage;
    SturdyAccessFlags dst_access;
    SturdyTextureLayout old_layout;
    SturdyTextureLayout new_layout;
    SturdyTextureSubresourceRange range;
} SturdyTextureBarrier;

/// Inserts a pipeline barrier. Any of the three arrays may be empty (count 0, pointer null).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_barrier(
    SturdyCommandEncoder encoder, uint32_t global_barrier_count, const SturdyGlobalBarrier *global_barriers,
    uint32_t buffer_barrier_count, const SturdyBufferBarrier *buffer_barriers, uint32_t texture_barrier_count,
    const SturdyTextureBarrier *texture_barriers);

/// Pushes a labeled debug group, for graphics-debugger capture readability.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_push_debug_group(SturdyCommandEncoder encoder,
                                                                                     const char *label);

/// Pops the most recently pushed debug group.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_pop_debug_group(SturdyCommandEncoder encoder);

/// Owned handle to an in-progress render pass, borrowed from its command encoder. Lives until
/// `sturdy_rhi_render_pass_end`. The parent `SturdyCommandEncoder` may not be used for anything
/// else while a pass is open.
typedef struct SturdyRenderPassEncoder {
    uint64_t token;
} SturdyRenderPassEncoder;

/// Begins a render pass. `desc->color_attachments` is read synchronously and need not outlive
/// the call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_render_pass(
    SturdyCommandEncoder encoder, const SturdyRenderPassDesc *desc, SturdyRenderPassEncoder *out_pass);

/// Sets the pipeline used by subsequent draws.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_pipeline(SturdyRenderPassEncoder pass,
                                                                             SturdyRenderPipeline pipeline);

/// Binds a bind group at `index`. `dynamic_offsets` supplies one offset per dynamic-offset
/// binding in the group's layout, in declaration order; pass null/0 when the layout has none.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_bind_group(
    SturdyRenderPassEncoder pass, uint32_t index, SturdyBindGroup bind_group, uint32_t dynamic_offset_count,
    const uint32_t *dynamic_offsets);

/// Binds a vertex buffer at `slot`, matching the pipeline's `vertex_buffers` layout array index.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_vertex_buffer(SturdyRenderPassEncoder pass,
                                                                                  uint32_t slot,
                                                                                  SturdyBuffer buffer,
                                                                                  uint64_t offset);

/// Binds the index buffer used by subsequent indexed draws.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_index_buffer(SturdyRenderPassEncoder pass,
                                                                                 SturdyBuffer buffer,
                                                                                 SturdyIndexFormat format,
                                                                                 uint64_t offset);

/// Writes push-constant bytes visible to `stages`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_push_constants(
    SturdyRenderPassEncoder pass, SturdyShaderStage stages, uint32_t offset, const void *data, size_t data_size);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_viewport(SturdyRenderPassEncoder pass,
                                                                             const SturdyViewport *viewport);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_scissor(SturdyRenderPassEncoder pass,
                                                                            const SturdyRect2D *scissor);

/// Sets the constant blend color used by `STURDY_BLEND_FACTOR_CONSTANT_COLOR`/
/// `STURDY_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_blend_constant(SturdyRenderPassEncoder pass,
                                                                                   const float color[4]);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_set_stencil_reference(SturdyRenderPassEncoder pass,
                                                                                      uint32_t reference);

typedef struct SturdyDrawArgs {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} SturdyDrawArgs;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw(SturdyRenderPassEncoder pass,
                                                                     const SturdyDrawArgs *args);

typedef struct SturdyDrawIndexedArgs {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t base_vertex;
    uint32_t first_instance;
} SturdyDrawIndexedArgs;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed(SturdyRenderPassEncoder pass,
                                                                             const SturdyDrawIndexedArgs *args);

/// Draws using arguments read from `indirect_buffer` at `offset` (a single `SturdyDrawArgs`-shaped
/// record, engine-native layout matching the active backend's indirect-draw structure).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect(SturdyRenderPassEncoder pass,
                                                                              SturdyBuffer indirect_buffer,
                                                                              uint64_t offset);

/// Issues `draw_count` draws read from consecutive `stride`-byte records in `indirect_buffer`
/// starting at `offset`. `draw_count` is fixed at record time; use
/// `sturdy_rhi_render_pass_draw_indirect_count` when the count itself comes from the GPU.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect_multi(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

/// Issues up to `max_draws` draws read from `indirect_buffer`, with the actual count read from a
/// 32-bit value in `count_buffer` at `count_offset` (clamped to `max_draws`). Backed by
/// `vkCmdDrawIndirectCount` on Vulkan and `ExecuteIndirect` with a counter resource on D3D12.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indirect_count(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

/// Indexed counterpart to `sturdy_rhi_render_pass_draw_indirect`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t offset);

/// Indexed counterpart to `sturdy_rhi_render_pass_draw_indirect_multi`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect_multi(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

/// Indexed counterpart to `sturdy_rhi_render_pass_draw_indirect_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_indexed_indirect_count(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

/// Dispatch dimensions for `sturdy_rhi_render_pass_draw_mesh_tasks`, matching
/// `RHI::DrawMeshTasksArgs`.
typedef struct SturdyDrawMeshTasksArgs {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
} SturdyDrawMeshTasksArgs;

/// Dispatches the mesh (and, if present, task) shader stages of the currently bound pipeline,
/// which must have been created with `SturdyRenderPipelineDesc::mesh` set. Neither backend
/// clamps `group_count_*` against a device maximum here — an out-of-range value is a backend-
/// defined failure, the same as a C++ caller would see.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks(
    SturdyRenderPassEncoder pass, const SturdyDrawMeshTasksArgs *args);

/// Mesh-shader counterpart to `sturdy_rhi_render_pass_draw_indirect`: dispatch dimensions are
/// read from a single `SturdyDrawMeshTasksArgs`-shaped record in `indirect_buffer` at `offset`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks_indirect(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t offset);

/// Mesh-shader counterpart to `sturdy_rhi_render_pass_draw_indirect_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_draw_mesh_tasks_indirect_count(
    SturdyRenderPassEncoder pass, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

/// Ends the render pass. `pass`'s token is invalid after this call, whether it succeeds or
/// fails. The parent command encoder may be used again afterward.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_end(SturdyRenderPassEncoder pass);

// ---------------------------------------------------------------------------------------------
// Render bundles
// ---------------------------------------------------------------------------------------------
//
// A render bundle is a reusable, pre-recorded sequence of pipeline/bind-group/draw commands
// (Vulkan secondary command buffers, D3D12 bundles under the hood) that a render pass can replay
// with `sturdy_rhi_render_pass_execute_bundles` without re-encoding it every frame. Recording
// mirrors the render-pass command surface above minus what bundles cannot legally do: no
// occlusion queries, no shading-rate control, and no nesting another bundle.

/// Describes a render bundle to create. Bundles are format-only — they record against whatever
/// render pass later executes them, matching it by attachment formats/sample count/view mask
/// rather than by binding real attachments.
typedef struct SturdyRenderBundleDesc {
    /// Set to `sizeof(SturdyRenderBundleDesc)` by `sturdy_rhi_render_bundle_desc_init`.
    uint32_t struct_size;
    uint32_t color_format_count;
    const SturdyFormat *color_formats;
    SturdyFormat depth_stencil_format;
    /// Sample count as an integer (1, 2, 4, 8, or 16), matching `RHI::SampleCount`'s values.
    uint32_t samples;
    uint32_t view_mask;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyRenderBundleDesc;

/// Fills `desc` with `struct_size` and zeroed contents (no color formats, no depth/stencil
/// format, `samples = 1`) — set `color_formats` before use.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_desc_init(SturdyRenderBundleDesc *desc);

/// Owned handle to an in-progress bundle recording, borrowed from the engine's active device.
/// Lives until `sturdy_rhi_render_bundle_encoder_finish` or `sturdy_rhi_render_bundle_encoder_release`.
typedef struct SturdyRenderBundleEncoder {
    uint64_t token;
} SturdyRenderBundleEncoder;

/// Handle to a finished, replayable render bundle. Unlike the encoder that produced it, this
/// carries no scope of its own — like `SturdyBuffer`, it is an informational identifier the RHI
/// device owns and validates until `sturdy_rhi_destroy_render_bundle`.
typedef struct SturdyRenderBundle {
    uint64_t id;
} SturdyRenderBundle;

/// Begins recording a render bundle.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_render_bundle_encoder(
    SturdyEngine engine, const SturdyRenderBundleDesc *desc, SturdyRenderBundleEncoder *out_encoder);

/// Abandons an in-progress bundle recording without finishing it. `encoder`'s token is invalid
/// after this call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_encoder_release(
    SturdyRenderBundleEncoder encoder);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_pipeline(SturdyRenderBundleEncoder encoder,
                                                                               SturdyRenderPipeline pipeline);

/// Binds a bind group at `index`. Same `dynamic_offsets` convention as
/// `sturdy_rhi_render_pass_set_bind_group`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_bind_group(
    SturdyRenderBundleEncoder encoder, uint32_t index, SturdyBindGroup bind_group,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_vertex_buffer(
    SturdyRenderBundleEncoder encoder, uint32_t slot, SturdyBuffer buffer, uint64_t offset);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_index_buffer(
    SturdyRenderBundleEncoder encoder, SturdyBuffer buffer, SturdyIndexFormat format, uint64_t offset);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_push_constants(
    SturdyRenderBundleEncoder encoder, SturdyShaderStage stages, uint32_t offset, const void *data,
    size_t data_size);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_viewport(SturdyRenderBundleEncoder encoder,
                                                                               const SturdyViewport *viewport);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_scissor(SturdyRenderBundleEncoder encoder,
                                                                              const SturdyRect2D *scissor);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_blend_constant(
    SturdyRenderBundleEncoder encoder, const float color[4]);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_set_stencil_reference(
    SturdyRenderBundleEncoder encoder, uint32_t reference);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw(SturdyRenderBundleEncoder encoder,
                                                                       const SturdyDrawArgs *args);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed(
    SturdyRenderBundleEncoder encoder, const SturdyDrawIndexedArgs *args);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect_multi(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect_multi(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset, uint32_t draw_count,
    uint32_t stride);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_indexed_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks(
    SturdyRenderBundleEncoder encoder, const SturdyDrawMeshTasksArgs *args);

/// D3D12 does not allow indirect draws of any kind — including mesh-tasks-indirect — inside a
/// bundle; this reports a backend failure there rather than an ABI-level rejection, matching how
/// the ordinary indirect-draw bundle restriction behaves.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks_indirect(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t offset);

/// See `sturdy_rhi_render_bundle_draw_mesh_tasks_indirect`'s D3D12 caveat.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_draw_mesh_tasks_indirect_count(
    SturdyRenderBundleEncoder encoder, SturdyBuffer indirect_buffer, uint64_t indirect_offset,
    SturdyBuffer count_buffer, uint64_t count_offset, uint32_t max_draws, uint32_t stride);

/// Finishes recording, consuming `encoder` (its token is invalid after this call, whether it
/// succeeds or fails) and producing a replayable `SturdyRenderBundle`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_bundle_encoder_finish(
    SturdyRenderBundleEncoder encoder, SturdyRenderBundle *out_bundle);

/// Destroys a bundle created by `sturdy_rhi_render_bundle_encoder_finish`. A null/zero handle is
/// a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_render_bundle(SturdyEngine engine,
                                                                          SturdyRenderBundle bundle);

/// Replays `bundles` into `pass`, which must have been created with
/// `SturdyRenderPassDesc::allow_bundles` set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_execute_bundles(
    SturdyRenderPassEncoder pass, uint32_t bundle_count, const SturdyRenderBundle *bundles);

/// Owned handle to an in-progress compute pass, borrowed from its command encoder. Lives until
/// `sturdy_rhi_compute_pass_end`. The parent `SturdyCommandEncoder` may not be used for anything
/// else while a pass is open.
typedef struct SturdyComputePassEncoder {
    uint64_t token;
} SturdyComputePassEncoder;

/// Begins a compute pass.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_compute_pass(
    SturdyCommandEncoder encoder, const char *label, SturdyComputePassEncoder *out_pass);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_pipeline(SturdyComputePassEncoder pass,
                                                                              SturdyComputePipeline pipeline);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_bind_group(
    SturdyComputePassEncoder pass, uint32_t index, SturdyBindGroup bind_group, uint32_t dynamic_offset_count,
    const uint32_t *dynamic_offsets);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_set_push_constants(
    SturdyComputePassEncoder pass, SturdyShaderStage stages, uint32_t offset, const void *data, size_t data_size);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_dispatch(SturdyComputePassEncoder pass,
                                                                          uint32_t group_count_x,
                                                                          uint32_t group_count_y,
                                                                          uint32_t group_count_z);

/// Dispatches using group counts read from `indirect_buffer` at `offset` (three consecutive
/// `uint32_t`s), written by an earlier pass on the GPU.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_dispatch_indirect(SturdyComputePassEncoder pass,
                                                                                   SturdyBuffer indirect_buffer,
                                                                                   uint64_t offset);

/// Ends the compute pass. `pass`'s token is invalid after this call, whether it succeeds or
/// fails. The parent command encoder may be used again afterward.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_compute_pass_end(SturdyComputePassEncoder pass);

/// Handle to a timeline semaphore: a GPU-to-GPU (and GPU-to-host, via
/// `sturdy_rhi_wait_semaphore`) synchronization primitive that counts up through a sequence of
/// `u64` values rather than toggling once like a fence. Reach for this to order work across two
/// queues (e.g. an upload on the transfer queue that a graphics-queue submission must wait on);
/// use `SturdyFence` instead for the common "wait for this one submission to finish on the host"
/// case. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdySemaphore {
    uint64_t id;
} SturdySemaphore;

/// Describes a semaphore to create.
typedef struct SturdySemaphoreDesc {
    /// Set to `sizeof(SturdySemaphoreDesc)` by the caller.
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t initial_value;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdySemaphoreDesc;

/// Creates a semaphore on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_semaphore(SturdyEngine engine,
                                                                     const SturdySemaphoreDesc *desc,
                                                                     SturdySemaphore *out_semaphore);

/// Destroys a semaphore created by `sturdy_rhi_create_semaphore`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_semaphore(SturdyEngine engine, SturdySemaphore semaphore);

/// Reads the semaphore's current counter value.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_semaphore_value(SturdyEngine engine, SturdySemaphore semaphore,
                                                                    uint64_t *out_value);

/// Blocks the calling host thread until `semaphore`'s counter reaches `value`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_semaphore(SturdyEngine engine, SturdySemaphore semaphore,
                                                                   uint64_t value, uint64_t timeout_ns);

/// Advances `semaphore`'s counter to `value` from the host. Rarely needed directly — a submission
/// with a signal entry (see `sturdy_rhi_submit`) is the usual way a semaphore advances.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_signal_semaphore(SturdyEngine engine, SturdySemaphore semaphore,
                                                                     uint64_t value);

/// One semaphore a submission waits on before its work begins.
typedef struct SturdySemaphoreWait {
    SturdySemaphore semaphore;
    uint64_t value;
    /// Pipeline stages of this submission that must wait; other stages may start immediately.
    /// Bitwise OR of `STURDY_PIPELINE_STAGE_*`.
    SturdyPipelineStage stages;
} SturdySemaphoreWait;

/// One semaphore a submission signals once its work completes.
typedef struct SturdySemaphoreSignal {
    SturdySemaphore semaphore;
    uint64_t value;
    /// Pipeline stages of this submission that must complete before signaling. Bitwise OR of
    /// `STURDY_PIPELINE_STAGE_*`.
    SturdyPipelineStage stages;
} SturdySemaphoreSignal;

/// Kind of work a query set records.
typedef enum SturdyQueryType {
    STURDY_QUERY_TYPE_OCCLUSION = 0,
    STURDY_QUERY_TYPE_TIMESTAMP = 1,
    STURDY_QUERY_TYPE_PIPELINE_STATISTICS = 2,
    STURDY_QUERY_TYPE_FORCE_U32 = 0x7fffffff
} SturdyQueryType;

/// Bitwise-OR'd pipeline-statistic counters. Only meaningful for
/// `STURDY_QUERY_TYPE_PIPELINE_STATISTICS`.
typedef uint32_t SturdyPipelineStatistic;
#define STURDY_PIPELINE_STATISTIC_NONE ((SturdyPipelineStatistic)0)
#define STURDY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES ((SturdyPipelineStatistic)(1u << 0))
#define STURDY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES ((SturdyPipelineStatistic)(1u << 1))
#define STURDY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS ((SturdyPipelineStatistic)(1u << 2))
#define STURDY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS ((SturdyPipelineStatistic)(1u << 3))
#define STURDY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES ((SturdyPipelineStatistic)(1u << 4))
#define STURDY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS ((SturdyPipelineStatistic)(1u << 5))
#define STURDY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES ((SturdyPipelineStatistic)(1u << 6))
#define STURDY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS ((SturdyPipelineStatistic)(1u << 7))
#define STURDY_PIPELINE_STATISTIC_TESS_CONTROL_SHADER_PATCHES ((SturdyPipelineStatistic)(1u << 8))
#define STURDY_PIPELINE_STATISTIC_TESS_EVALUATION_SHADER_INVOCATIONS ((SturdyPipelineStatistic)(1u << 9))
#define STURDY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS ((SturdyPipelineStatistic)(1u << 10))

/// Bitwise-OR'd flags controlling how `sturdy_rhi_get_query_set_results` reads results back.
typedef uint32_t SturdyQueryResultFlags;
#define STURDY_QUERY_RESULT_FLAGS_NONE ((SturdyQueryResultFlags)0)
/// Each result is a 64-bit value rather than 32-bit.
#define STURDY_QUERY_RESULT_FLAGS_RESULT_64_BIT ((SturdyQueryResultFlags)(1u << 0))
/// Block until every requested query is available rather than returning partial/stale data.
#define STURDY_QUERY_RESULT_FLAGS_WAIT ((SturdyQueryResultFlags)(1u << 1))
/// Append one extra value per query reporting whether it was available.
#define STURDY_QUERY_RESULT_FLAGS_WITH_AVAILABILITY ((SturdyQueryResultFlags)(1u << 2))
/// Accept whatever subset of queries is currently available instead of failing outright.
#define STURDY_QUERY_RESULT_FLAGS_PARTIAL ((SturdyQueryResultFlags)(1u << 3))

/// Handle to a query set. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyQuerySet {
    uint64_t id;
} SturdyQuerySet;

/// Describes a query set to create.
typedef struct SturdyQuerySetDesc {
    /// Set to `sizeof(SturdyQuerySetDesc)` by the caller.
    uint32_t struct_size;
    SturdyQueryType type;
    uint32_t count;
    /// Only meaningful for `STURDY_QUERY_TYPE_PIPELINE_STATISTICS`: bitwise OR of
    /// `STURDY_PIPELINE_STATISTIC_*`.
    SturdyPipelineStatistic statistics;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyQuerySetDesc;

/// Creates a query set on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_query_set(SturdyEngine engine,
                                                                     const SturdyQuerySetDesc *desc,
                                                                     SturdyQuerySet *out_query_set);

/// Destroys a query set created by `sturdy_rhi_create_query_set`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_query_set(SturdyEngine engine, SturdyQuerySet query_set);

/// Reads back `count` results starting at `first`, into `dst` (which must be at least
/// `count * stride` bytes). Blocks the host if `STURDY_QUERY_RESULT_FLAGS_WAIT` is set;
/// otherwise a query whose result is not yet available reads as undefined unless
/// `STURDY_QUERY_RESULT_FLAGS_PARTIAL` is also set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_get_query_set_results(
    SturdyEngine engine, SturdyQuerySet query_set, uint32_t first, uint32_t count, void *dst, size_t dst_size,
    uint64_t stride, SturdyQueryResultFlags flags);

/// Resets `count` queries starting at `first` to the unavailable state, outside any command
/// encoder. Every query must be reset (via this or
/// `sturdy_rhi_command_encoder_reset_query_set`) before its first use.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_reset_query_set(SturdyEngine engine, SturdyQuerySet query_set,
                                                                    uint32_t first, uint32_t count);

/// Resets `count` queries starting at `first` to the unavailable state, recorded into the command
/// encoder.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_reset_query_set(SturdyCommandEncoder encoder,
                                                                                    SturdyQuerySet query_set,
                                                                                    uint32_t first, uint32_t count);

/// Writes a GPU timestamp for `query_set[index]` once every command before this point in the
/// encoder has reached `stage`. Only valid for a `STURDY_QUERY_TYPE_TIMESTAMP` query set.
///
/// @param stage A single `STURDY_PIPELINE_STAGE_*` value (not a combination) naming the point in
///        the pipeline to timestamp.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_write_timestamp(
    SturdyCommandEncoder encoder, SturdyPipelineStage stage, SturdyQuerySet query_set, uint32_t index);

/// Begins a pipeline-statistics query at `query_set[index]`, ended by
/// `sturdy_rhi_command_encoder_end_pipeline_statistics_query`. Only valid for a
/// `STURDY_QUERY_TYPE_PIPELINE_STATISTICS` query set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_begin_pipeline_statistics_query(
    SturdyCommandEncoder encoder, SturdyQuerySet query_set, uint32_t index);

/// Ends the most recently begun pipeline-statistics query on this encoder.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_end_pipeline_statistics_query(
    SturdyCommandEncoder encoder);

/// Copies `count` query results starting at `first` from `query_set` into `dst` at `dst_offset`,
/// entirely on the GPU. Prefer this over `sturdy_rhi_get_query_set_results` when the results feed
/// a later GPU pass rather than the host (e.g. GPU-driven culling statistics).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_resolve_query_set(
    SturdyCommandEncoder encoder, SturdyQuerySet query_set, uint32_t first, uint32_t count, SturdyBuffer dst,
    uint64_t dst_offset, uint64_t stride, SturdyQueryResultFlags flags);

/// Begins an occlusion query at `query_set[index]`, ended by
/// `sturdy_rhi_render_pass_end_occlusion_query`. Only valid for a `STURDY_QUERY_TYPE_OCCLUSION`
/// query set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_begin_occlusion_query(SturdyRenderPassEncoder pass,
                                                                                      SturdyQuerySet query_set,
                                                                                      uint32_t index);

/// Ends the most recently begun occlusion query on this render pass.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_render_pass_end_occlusion_query(SturdyRenderPassEncoder pass);

/// Pass to `sturdy_rhi_wait_fences`' `timeout_ns` to block with no timeout.
#define STURDY_WAIT_FOREVER (~(uint64_t)0)

/// Handle to a fence: a GPU-to-host synchronization primitive signaled when a submission
/// completes. See the "RHI resources" section header for its lifetime rules.
typedef struct SturdyFence {
    uint64_t id;
} SturdyFence;

/// Describes a fence to create.
typedef struct SturdyFenceDesc {
    /// Set to `sizeof(SturdyFenceDesc)` by the caller.
    uint32_t struct_size;
    /// Whether the fence starts already signaled.
    SturdyBool signaled;
    uint8_t reserved[3];
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyFenceDesc;

/// Creates a fence on the engine's active device.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_fence(SturdyEngine engine, const SturdyFenceDesc *desc,
                                                                 SturdyFence *out_fence);

/// Destroys a fence created by `sturdy_rhi_create_fence`. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_fence(SturdyEngine engine, SturdyFence fence);

/// Waits for one or more fences to become signaled — the correct way to know a specific
/// submission has finished. Prefer this over `sturdy_rhi_wait_idle`, which is a device-wide
/// barrier and unsafe to interleave with any other GPU work in flight, including the engine's own.
///
/// @param wait_all `STURDY_TRUE` to wait for every fence; `STURDY_FALSE` to return once any one is
///        signaled.
/// @param timeout_ns Maximum time to wait, in nanoseconds. `STURDY_WAIT_FOREVER` for no timeout.
/// @param out_signaled Receives whether the wait condition was met (`STURDY_FALSE` on timeout).
///        May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_fences(SturdyEngine engine, uint32_t fence_count,
                                                                const SturdyFence *fences, SturdyBool wait_all,
                                                                uint64_t timeout_ns, SturdyBool *out_signaled);

/// Resets one or more fences to unsignaled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_reset_fences(SturdyEngine engine, uint32_t fence_count,
                                                                 const SturdyFence *fences);

/// Submits finished command buffers to a device queue for execution.
///
/// @param wait_count/waits Semaphores this submission's work waits on before starting. May be
///        0/null.
/// @param signal_count/signals Semaphores this submission signals once its work completes. May
///        be 0/null.
/// @param fence Optional; the zero handle submits with no fence. When supplied, it becomes
///        signaled once this submission's work completes — wait on it with
///        `sturdy_rhi_wait_fences` rather than `sturdy_rhi_wait_idle` to know when it is safe to
///        reuse or destroy the submitted command buffers. This is the one-shot GPU-work pattern:
///        create a fence, submit with it, wait on it, destroy the fence and command buffer.
/// @param one_shot Hints that these command buffers will not be resubmitted, letting the backend
///        release their recording resources as soon as this submission completes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_submit(
    SturdyEngine engine, SturdyQueueClass queue_class, uint32_t queue_lane_index, uint32_t command_buffer_count,
    const SturdyCommandBuffer *command_buffers, uint32_t wait_count, const SturdySemaphoreWait *waits,
    uint32_t signal_count, const SturdySemaphoreSignal *signals, SturdyFence fence, SturdyBool one_shot);

/// Blocks until every queue on the active device has finished all submitted work. Expensive, and
/// unsafe to call while any other GPU work — including the engine's own internal one-shot
/// uploads — may still be in flight on this device. Intended for shutdown/resource-teardown
/// synchronization; wait on a specific submission with `sturdy_rhi_wait_fences` instead.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_wait_idle(SturdyEngine engine);

// ---------------------------------------------------------------------------------------------
// Presentation and HDR
// ---------------------------------------------------------------------------------------------
//
// Live control over a surface's swapchain policy: vsync/VRR/latency, transparent composition,
// image count, and real display HDR (distinct from `SturdyToneMappingSettings`'s
// `hdr_paper_white_nits`/`hdr_peak_nits`, which only shape the tone-mapping curve and never touch
// the swapchain's color space). A change here is queued the same way as a window request — it is
// picked up the next time that surface renders, not applied synchronously.

/// How the engine should schedule presentation relative to the display's refresh cycle, once
/// vsync/VRR/latency are taken into account. Matches `RHI::PresentStrategy`.
typedef enum SturdyPresentStrategy {
    STURDY_PRESENT_STRATEGY_UNSYNCHRONIZED = 0,
    STURDY_PRESENT_STRATEGY_TEAR_FREE_ORDERED,
    STURDY_PRESENT_STRATEGY_TEAR_FREE_LATEST,
    STURDY_PRESENT_STRATEGY_ADAPTIVE_TEARING,
    STURDY_PRESENT_STRATEGY_TEAR_FREE_LATEST_READY,
    STURDY_PRESENT_STRATEGY_VARIABLE_REFRESH,
    STURDY_PRESENT_STRATEGY_FORCE_U32 = 0x7fffffff
} SturdyPresentStrategy;

/// Concrete swapchain present mode a `SturdyPresentStrategy` resolved to on this backend. Matches
/// `RHI::PresentMode`.
typedef enum SturdyPresentMode {
    STURDY_PRESENT_MODE_FIFO = 0,
    STURDY_PRESENT_MODE_FIFO_RELAXED,
    STURDY_PRESENT_MODE_MAILBOX,
    STURDY_PRESENT_MODE_IMMEDIATE,
    STURDY_PRESENT_MODE_FIFO_LATEST_READY,
    STURDY_PRESENT_MODE_FORCE_U32 = 0x7fffffff
} SturdyPresentMode;

/// How the swapchain's alpha channel composites with what is behind the window. Matches
/// `RHI::CompositeAlphaMode`.
typedef enum SturdyCompositeAlphaMode {
    STURDY_COMPOSITE_ALPHA_MODE_AUTO = 0,
    STURDY_COMPOSITE_ALPHA_MODE_OPAQUE,
    STURDY_COMPOSITE_ALPHA_MODE_PREMULTIPLIED,
    STURDY_COMPOSITE_ALPHA_MODE_POST_MULTIPLIED,
    STURDY_COMPOSITE_ALPHA_MODE_INHERIT,
    STURDY_COMPOSITE_ALPHA_MODE_FORCE_U32 = 0x7fffffff
} SturdyCompositeAlphaMode;

/// Swapchain color space, including real display HDR transfer functions. Matches `RHI::ColorSpace`.
/// `STURDY_COLOR_SPACE_DOLBY_VISION` is declared for completeness but not implemented by either
/// backend yet; requesting it will not succeed.
typedef enum SturdyColorSpace {
    STURDY_COLOR_SPACE_SRGB_NONLINEAR = 0,
    STURDY_COLOR_SPACE_HDR10_ST2084,
    STURDY_COLOR_SPACE_SCRGB_LINEAR,
    STURDY_COLOR_SPACE_HDR10_HLG,
    STURDY_COLOR_SPACE_DOLBY_VISION,
    STURDY_COLOR_SPACE_ADOBE_RGB_LINEAR,
    STURDY_COLOR_SPACE_ADOBE_RGB_NONLINEAR,
    STURDY_COLOR_SPACE_DISPLAY_P3_LINEAR,
    STURDY_COLOR_SPACE_DISPLAY_P3_NONLINEAR,
    STURDY_COLOR_SPACE_BT2020_LINEAR,
    STURDY_COLOR_SPACE_FORCE_U32 = 0x7fffffff
} SturdyColorSpace;

/// Variable refresh rate policy. Matches `Core::VariableRefreshMode`.
typedef enum SturdyVariableRefreshMode {
    STURDY_VARIABLE_REFRESH_MODE_DISABLED = 0,
    STURDY_VARIABLE_REFRESH_MODE_AUTOMATIC,
    STURDY_VARIABLE_REFRESH_MODE_PREFERRED,
    STURDY_VARIABLE_REFRESH_MODE_FORCE_U32 = 0x7fffffff
} SturdyVariableRefreshMode;

/// Presentation latency policy. Matches `Core::LatencyMode`.
typedef enum SturdyLatencyMode {
    STURDY_LATENCY_MODE_NORMAL = 0,
    STURDY_LATENCY_MODE_LOW,
    STURDY_LATENCY_MODE_ULTRA,
    STURDY_LATENCY_MODE_FORCE_U32 = 0x7fffffff
} SturdyLatencyMode;

/// Overall presentation tuning goal. Matches `Core::PresentationPreference`.
typedef enum SturdyPresentationPreference {
    STURDY_PRESENTATION_PREFERENCE_AUTOMATIC = 0,
    STURDY_PRESENTATION_PREFERENCE_LOWEST_LATENCY,
    STURDY_PRESENTATION_PREFERENCE_SMOOTHEST,
    STURDY_PRESENTATION_PREFERENCE_POWER_EFFICIENT,
    STURDY_PRESENTATION_PREFERENCE_FORCE_U32 = 0x7fffffff
} SturdyPresentationPreference;

/// Real display HDR transfer function/color-space combination to present in, when
/// `SturdyPresentationSettings::hdr_enabled` is set. Matches `Core::HdrColorSpaceMode`.
typedef enum SturdyHdrColorSpaceMode {
    STURDY_HDR_COLOR_SPACE_MODE_HDR10_ST2084 = 0,
    STURDY_HDR_COLOR_SPACE_MODE_SCRGB_LINEAR,
    STURDY_HDR_COLOR_SPACE_MODE_HDR10_HLG,
    STURDY_HDR_COLOR_SPACE_MODE_DOLBY_VISION,
    STURDY_HDR_COLOR_SPACE_MODE_FORCE_U32 = 0x7fffffff
} SturdyHdrColorSpaceMode;

/// A display's reported HDR transfer function. Matches `RHI::HdrTransferFunction`.
typedef enum SturdyHdrTransferFunction {
    STURDY_HDR_TRANSFER_FUNCTION_UNKNOWN = 0,
    STURDY_HDR_TRANSFER_FUNCTION_SDR,
    STURDY_HDR_TRANSFER_FUNCTION_PQ_ST2084,
    STURDY_HDR_TRANSFER_FUNCTION_HLG,
    STURDY_HDR_TRANSFER_FUNCTION_LINEAR_EXTENDED,
    STURDY_HDR_TRANSFER_FUNCTION_FORCE_U32 = 0x7fffffff
} SturdyHdrTransferFunction;

/// A display's reported color gamut. Matches `RHI::HdrColorGamut`.
typedef enum SturdyHdrColorGamut {
    STURDY_HDR_COLOR_GAMUT_UNKNOWN = 0,
    STURDY_HDR_COLOR_GAMUT_REC709,
    STURDY_HDR_COLOR_GAMUT_DISPLAY_P3,
    STURDY_HDR_COLOR_GAMUT_REC2020,
    STURDY_HDR_COLOR_GAMUT_FORCE_U32 = 0x7fffffff
} SturdyHdrColorGamut;

/// Where reported HDR display metadata came from. Matches `RHI::HdrMetadataSource`.
typedef enum SturdyHdrMetadataSource {
    STURDY_HDR_METADATA_SOURCE_UNKNOWN = 0,
    STURDY_HDR_METADATA_SOURCE_GRAPHICS_API,
    STURDY_HDR_METADATA_SOURCE_OPERATING_SYSTEM,
    STURDY_HDR_METADATA_SOURCE_WINDOW_SYSTEM,
    STURDY_HDR_METADATA_SOURCE_EDID,
    STURDY_HDR_METADATA_SOURCE_USER_CALIBRATION,
    STURDY_HDR_METADATA_SOURCE_ENGINE_DEFAULT,
    STURDY_HDR_METADATA_SOURCE_FORCE_U32 = 0x7fffffff
} SturdyHdrMetadataSource;

/// How much to trust reported HDR display metadata. Matches `RHI::HdrMetadataConfidence`.
typedef enum SturdyHdrMetadataConfidence {
    STURDY_HDR_METADATA_CONFIDENCE_UNKNOWN = 0,
    STURDY_HDR_METADATA_CONFIDENCE_ESTIMATED,
    STURDY_HDR_METADATA_CONFIDENCE_REPORTED,
    STURDY_HDR_METADATA_CONFIDENCE_CALIBRATED,
    STURDY_HDR_METADATA_CONFIDENCE_MEASURED,
    STURDY_HDR_METADATA_CONFIDENCE_FORCE_U32 = 0x7fffffff
} SturdyHdrMetadataConfidence;

/// Outcome of a platform-level display/HDR query, distinct from `SturdyResult`: the call itself
/// succeeded (a device and surface exist), but the platform may still have nothing useful to
/// report. Matches `RHI::PlatformQueryStatus`.
typedef enum SturdyPlatformQueryStatus {
    STURDY_PLATFORM_QUERY_STATUS_OK = 0,
    STURDY_PLATFORM_QUERY_STATUS_UNSUPPORTED,
    STURDY_PLATFORM_QUERY_STATUS_NOT_AVAILABLE,
    STURDY_PLATFORM_QUERY_STATUS_INVALID_ARGUMENT,
    STURDY_PLATFORM_QUERY_STATUS_PLATFORM_ERROR,
    STURDY_PLATFORM_QUERY_STATUS_FORCE_U32 = 0x7fffffff
} SturdyPlatformQueryStatus;

/// A surface's presentation policy. Mirrors `Core::PresentationSettings` field-for-field.
typedef struct SturdyPresentationSettings {
    /// Set to `sizeof(SturdyPresentationSettings)` by `sturdy_presentation_settings_init`.
    uint32_t struct_size;
    uint32_t reserved;
    SturdyVSync vsync;
    SturdyVariableRefreshMode variable_refresh;
    SturdyLatencyMode latency;
    SturdyPresentationPreference preference;
    /// Enables real display HDR output (not tone-mapping). Fails to take effect if the active
    /// backend/display cannot support it; check `sturdy_surface_presentation_resolution` after the
    /// next frame to see what was actually negotiated.
    SturdyBool hdr_enabled;
    SturdyHdrColorSpaceMode hdr_color_space;
    SturdyBool transparent_composition;
    /// Requested swapchain image count. Zero uses the engine's default.
    uint32_t swapchain_image_count;
    SturdyBool allow_present_from_compute;
} SturdyPresentationSettings;

/// Fills `settings` with `struct_size` and the engine's defaults (vsync on, no HDR, opaque
/// composition, automatic image count).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_presentation_settings_init(SturdyPresentationSettings *settings);

/// Requests a change to `surface`'s presentation policy. Queued like a window request: applied the
/// next time `surface` renders, not synchronously.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_surface_set_presentation_settings(
    SturdyEngine engine,
    SturdySurface surface,
    const SturdyPresentationSettings *settings);

/// Reads `surface`'s current presentation policy: the last value
/// `sturdy_surface_set_presentation_settings` accepted, or the app-wide default if never
/// overridden.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_surface_presentation_settings(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyPresentationSettings *out_settings);

/// What a `SturdyPresentationSettings` request actually resolved to on the active backend and
/// display, including whether the requested strategy, composite alpha, or full-screen-exclusive
/// state degraded to something else. Mirrors `RHI::PresentationResolution` field-for-field.
typedef struct SturdyPresentationResolution {
    uint32_t struct_size;
    uint32_t reserved;
    SturdyPresentStrategy strategy;
    SturdyPresentMode effective_mode;
    SturdyBool degraded;
    SturdyBool present_queue_is_compute;
    SturdyCompositeAlphaMode effective_composite_alpha;
    SturdyBool composite_alpha_degraded;
    SturdyBool via_composition_present;
    SturdyBool supports_completion_fence;
    SturdyBool full_screen_exclusive_active;
    SturdyFormat effective_format;
    SturdyColorSpace effective_color_space;
} SturdyPresentationResolution;

/// Reads what `surface`'s presentation policy actually resolved to.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when `surface` has not rendered a frame yet (no swapchain
///         exists to report on).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_surface_presentation_resolution(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyPresentationResolution *out_resolution);

/// A 1931 CIE xy chromaticity coordinate.
typedef struct SturdyChromaticity {
    float x;
    float y;
} SturdyChromaticity;

/// Real HDR display metadata (EDID-reported primaries/white point/luminance range, or a platform
/// fallback), as opposed to `SturdyToneMappingSettings`'s software tone-curve parameters.
typedef struct SturdyHdrDisplayMetadata {
    uint32_t struct_size;
    uint32_t reserved;
    SturdyChromaticity red_primary;
    SturdyChromaticity green_primary;
    SturdyChromaticity blue_primary;
    SturdyChromaticity white_point;
    float min_luminance_nits;
    float max_luminance_nits;
    float max_full_frame_luminance_nits;
    SturdyHdrMetadataSource source;
    SturdyHdrMetadataConfidence confidence;
} SturdyHdrDisplayMetadata;

/// One transfer-function/gamut combination a display can present in.
typedef struct SturdyHdrPresentationMode {
    SturdyHdrTransferFunction transfer;
    SturdyHdrColorGamut gamut;
    /// Whether the OS-level HDR toggle must be on for this mode to work, independent of what the
    /// application requests.
    SturdyBool requires_os_hdr_mode;
} SturdyHdrPresentationMode;

/// Real display HDR capabilities for a surface, as reported by the platform. Mirrors
/// `RHI::SurfaceHdrCapabilities` plus the outer query's `status`; diagnostic message text is not
/// surfaced here.
typedef struct SturdyHdrCapabilities {
    uint32_t struct_size;
    /// `STURDY_PLATFORM_QUERY_STATUS_OK` when the platform answered normally. A non-OK status
    /// means the rest of this struct (aside from `status` itself) is default-initialized, not
    /// meaningful.
    SturdyPlatformQueryStatus status;
    SturdyBool hdr_supported;
    SturdyBool hdr_enabled_by_os;
    SturdyBool hdr_metadata_output_supported;
    float sdr_white_nits;
    float edr_headroom;
    float max_edr_headroom;
    SturdyBool has_display_metadata;
    SturdyHdrDisplayMetadata display_metadata;
} SturdyHdrCapabilities;

/// Queries `surface`'s real HDR display capabilities and supported presentation modes.
///
/// @param out_capabilities Receives the capability summary. Must not be null.
/// @param out_modes Receives up to `modes_capacity` supported transfer-function/gamut
///        combinations. May be null if `modes_capacity` is 0.
/// @param out_mode_count Receives the number of modes the display actually supports, which may
///        exceed `modes_capacity`. May be null.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when `surface` has not rendered a frame yet (no RHI
///         surface exists to query). A supported-but-empty answer is not an ABI error — see
///         `SturdyHdrCapabilities::status` for that.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_surface_query_hdr_capabilities(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyHdrCapabilities *out_capabilities,
    SturdyHdrPresentationMode *out_modes,
    uint32_t modes_capacity,
    uint32_t *out_mode_count);

/// Updates HDR10 static content light level metadata (MaxCLL/MaxFALL) on `surface`'s live
/// swapchain, without recreating it.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when `surface` has no live swapchain yet, or when the
///         active backend/driver cannot update this metadata live (e.g. Vulkan without
///         `VK_EXT_hdr_metadata`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_surface_update_hdr_content_light_level(
    SturdyEngine engine,
    SturdySurface surface,
    float max_content_light_level_nits,
    float max_frame_average_light_level_nits);

// ---------------------------------------------------------------------------------------------
// Ray tracing
// ---------------------------------------------------------------------------------------------
//
// Acceleration structures, ray tracing pipelines, shader binding tables, and `trace_rays`.
// Opacity micromaps are declared here too, but D3D12 does not implement them
// (`create_opacity_micromap`/`opacity_micromap_build_sizes`/`sturdy_rhi_command_encoder_
// build_opacity_micromaps` report `STURDY_ERROR_NOT_AVAILABLE` there) — Vulkan implements the
// full surface via `VK_EXT_opacity_micromap`. Check `STURDY_FEATURE`-style support first with
// `sturdy_rhi_feature_index("opacity micromaps", ...)`/`sturdy_rhi_feature_enabled` if targeting
// both backends.
//
// Acceleration structures, ray tracing pipelines, and opacity micromaps are plain resource
// handles like `SturdyBuffer` — informational identifiers the device owns and validates until
// the matching `sturdy_rhi_destroy_*` call, not scope-bound tokens.

/// Ray tracing stages, added to `SturdyShaderStage`'s bitmask. Bit positions match
/// `RHI::ShaderStage`.
#define STURDY_SHADER_STAGE_RAY_GENERATION ((SturdyShaderStage)(1u << 8))
#define STURDY_SHADER_STAGE_ANY_HIT ((SturdyShaderStage)(1u << 9))
#define STURDY_SHADER_STAGE_CLOSEST_HIT ((SturdyShaderStage)(1u << 10))
#define STURDY_SHADER_STAGE_MISS ((SturdyShaderStage)(1u << 11))
#define STURDY_SHADER_STAGE_INTERSECTION ((SturdyShaderStage)(1u << 12))
#define STURDY_SHADER_STAGE_CALLABLE ((SturdyShaderStage)(1u << 13))

/// Whether an acceleration structure holds geometry (bottom-level) or instances of bottom-level
/// structures (top-level). Matches `RHI::AccelerationStructureType`.
typedef enum SturdyAccelerationStructureType {
    STURDY_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL = 0,
    STURDY_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL = 1,
    STURDY_ACCELERATION_STRUCTURE_TYPE_FORCE_U32 = 0x7fffffff
} SturdyAccelerationStructureType;

/// Bitwise-OR'd acceleration structure build flags. Matches `RHI::AccelerationStructureBuildFlags`.
typedef uint32_t SturdyAccelerationStructureBuildFlags;
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_NONE ((SturdyAccelerationStructureBuildFlags)0)
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_ALLOW_UPDATE ((SturdyAccelerationStructureBuildFlags)(1u << 0))
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_ALLOW_COMPACTION ((SturdyAccelerationStructureBuildFlags)(1u << 1))
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_PREFER_FAST_TRACE ((SturdyAccelerationStructureBuildFlags)(1u << 2))
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_PREFER_FAST_BUILD ((SturdyAccelerationStructureBuildFlags)(1u << 3))
#define STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_MINIMIZE_MEMORY ((SturdyAccelerationStructureBuildFlags)(1u << 4))

/// Bitwise-OR'd per-geometry flags. Matches `RHI::AccelerationStructureGeometryFlags`.
typedef uint32_t SturdyAccelerationStructureGeometryFlags;
#define STURDY_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS_NONE ((SturdyAccelerationStructureGeometryFlags)0)
#define STURDY_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS_OPAQUE ((SturdyAccelerationStructureGeometryFlags)(1u << 0))
#define STURDY_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS_NO_DUPLICATE_ANY_HIT_INVOCATION \
    ((SturdyAccelerationStructureGeometryFlags)(1u << 1))

/// Which member of `SturdyAccelerationStructureGeometryDesc` is meaningful. Matches
/// `RHI::AccelerationStructureGeometryType`.
typedef enum SturdyAccelerationStructureGeometryType {
    STURDY_ACCELERATION_STRUCTURE_GEOMETRY_TYPE_TRIANGLES = 0,
    STURDY_ACCELERATION_STRUCTURE_GEOMETRY_TYPE_AABBS = 1,
    STURDY_ACCELERATION_STRUCTURE_GEOMETRY_TYPE_INSTANCES = 2,
    STURDY_ACCELERATION_STRUCTURE_GEOMETRY_TYPE_FORCE_U32 = 0x7fffffff
} SturdyAccelerationStructureGeometryType;

/// Matches `RHI::AccelerationStructureCopyMode`.
typedef enum SturdyAccelerationStructureCopyMode {
    STURDY_ACCELERATION_STRUCTURE_COPY_MODE_CLONE = 0,
    STURDY_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT = 1,
    STURDY_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE = 2,
    STURDY_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE = 3,
    STURDY_ACCELERATION_STRUCTURE_COPY_MODE_FORCE_U32 = 0x7fffffff
} SturdyAccelerationStructureCopyMode;

/// Matches `RHI::RayTracingShaderGroupType`.
typedef enum SturdyRayTracingShaderGroupType {
    STURDY_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL = 0,
    STURDY_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP = 1,
    STURDY_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP = 2,
    STURDY_RAY_TRACING_SHADER_GROUP_TYPE_FORCE_U32 = 0x7fffffff
} SturdyRayTracingShaderGroupType;

/// Matches `RHI::OpacityMicromapFormat`.
typedef enum SturdyOpacityMicromapFormat {
    STURDY_OPACITY_MICROMAP_FORMAT_TWO_STATE = 0,
    STURDY_OPACITY_MICROMAP_FORMAT_FOUR_STATE = 1,
    STURDY_OPACITY_MICROMAP_FORMAT_FORCE_U32 = 0x7fffffff
} SturdyOpacityMicromapFormat;

/// Handle to an acceleration structure. See the "Ray tracing" section header for its lifetime
/// rules.
typedef struct SturdyAccelerationStructure {
    uint64_t id;
} SturdyAccelerationStructure;

/// Handle to an opacity micromap. Not supported on D3D12 — see the "Ray tracing" section header.
typedef struct SturdyOpacityMicromap {
    uint64_t id;
} SturdyOpacityMicromap;

/// Handle to a ray tracing pipeline.
typedef struct SturdyRayTracingPipeline {
    uint64_t id;
} SturdyRayTracingPipeline;

/// Describes an acceleration structure to create. `size` must come from
/// `sturdy_rhi_acceleration_structure_build_sizes`.
typedef struct SturdyAccelerationStructureDesc {
    uint32_t struct_size;
    SturdyAccelerationStructureType type;
    uint64_t size;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyAccelerationStructureDesc;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_desc_init(
    SturdyAccelerationStructureDesc *desc);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_acceleration_structure(
    SturdyEngine engine, const SturdyAccelerationStructureDesc *desc,
    SturdyAccelerationStructure *out_structure);

/// Destroys an acceleration structure. A null/zero handle is a no-op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_acceleration_structure(
    SturdyEngine engine, SturdyAccelerationStructure structure);

/// Reads the GPU virtual address of `structure`, for writing into
/// `SturdyAccelerationStructureInstance::acceleration_structure_device_address`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_device_address(
    SturdyEngine engine, SturdyAccelerationStructure structure, uint64_t *out_address);

/// Triangle geometry within an acceleration structure build. `opacity_micromap` is a zero handle
/// unless `Feature`-gated opacity micromaps are used (Vulkan only).
typedef struct SturdyAccelerationStructureTrianglesDesc {
    SturdyBuffer vertex_buffer;
    uint64_t vertex_offset;
    SturdyVertexFormat vertex_format;
    uint64_t vertex_stride;
    uint32_t max_vertex;
    SturdyBuffer index_buffer;
    uint64_t index_offset;
    SturdyIndexFormat index_format;
    /// Zero handle means no per-instance transform.
    SturdyBuffer transform_buffer;
    uint64_t transform_offset;
    SturdyOpacityMicromap opacity_micromap;
    SturdyBuffer opacity_micromap_index_buffer;
    uint64_t opacity_micromap_index_offset;
    SturdyIndexFormat opacity_micromap_index_format;
} SturdyAccelerationStructureTrianglesDesc;

/// Procedural (AABB) geometry within an acceleration structure build.
typedef struct SturdyAccelerationStructureAabbsDesc {
    SturdyBuffer buffer;
    uint64_t offset;
    uint64_t stride;
} SturdyAccelerationStructureAabbsDesc;

/// Top-level instance geometry within an acceleration structure build.
typedef struct SturdyAccelerationStructureInstancesDesc {
    /// A buffer of `SturdyAccelerationStructureInstance` records, or of pointers to them if
    /// `array_of_pointers` is set.
    SturdyBuffer buffer;
    uint64_t offset;
    SturdyBool array_of_pointers;
} SturdyAccelerationStructureInstancesDesc;

/// One geometry entry in an acceleration structure build. `type` selects which of
/// `triangles`/`aabbs`/`instances` is meaningful; the other two are ignored. Mirrors
/// `RHI::AccelerationStructureGeometryDesc`, which carries all three as plain members rather than
/// a tagged union.
typedef struct SturdyAccelerationStructureGeometryDesc {
    SturdyAccelerationStructureGeometryType type;
    SturdyAccelerationStructureGeometryFlags flags;
    SturdyAccelerationStructureTrianglesDesc triangles;
    SturdyAccelerationStructureAabbsDesc aabbs;
    SturdyAccelerationStructureInstancesDesc instances;
} SturdyAccelerationStructureGeometryDesc;

/// How many primitives (and at what offsets) each geometry in a build contributes. Index-aligned
/// with the build's `geometries` array.
typedef struct SturdyAccelerationStructureBuildRangeInfo {
    uint32_t primitive_count;
    uint32_t primitive_offset;
    uint32_t first_vertex;
    uint32_t transform_offset;
} SturdyAccelerationStructureBuildRangeInfo;

/// Buffer sizes needed to build an acceleration structure, returned by
/// `sturdy_rhi_acceleration_structure_build_sizes`.
typedef struct SturdyAccelerationStructureBuildSizes {
    /// Pass to `SturdyAccelerationStructureDesc::size`.
    uint64_t acceleration_structure_size;
    /// Scratch buffer size needed for an initial build.
    uint64_t build_scratch_size;
    /// Scratch buffer size needed for an in-place update (only meaningful with
    /// `STURDY_ACCELERATION_STRUCTURE_BUILD_FLAGS_ALLOW_UPDATE`).
    uint64_t update_scratch_size;
} SturdyAccelerationStructureBuildSizes;

/// Describes one acceleration structure build (or update, if `src` is nonzero). `geometries`
/// must match what was passed to `sturdy_rhi_acceleration_structure_build_sizes` when `dst` was
/// sized.
typedef struct SturdyAccelerationStructureBuildDesc {
    SturdyAccelerationStructureType type;
    SturdyAccelerationStructureBuildFlags flags;
    SturdyAccelerationStructure dst;
    /// Zero handle for an initial build; the structure being updated for an in-place update.
    SturdyAccelerationStructure src;
    SturdyBuffer scratch_buffer;
    uint64_t scratch_offset;
    uint32_t geometry_count;
    const SturdyAccelerationStructureGeometryDesc *geometries;
    uint32_t range_count;
    const SturdyAccelerationStructureBuildRangeInfo *ranges;
} SturdyAccelerationStructureBuildDesc;

/// Queries the buffer sizes needed to build the acceleration structure described by `desc`.
/// `desc->dst`/`src` are ignored; only `type`, `flags`, and `geometries` matter for sizing.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_acceleration_structure_build_sizes(
    SturdyEngine engine, const SturdyAccelerationStructureBuildDesc *desc,
    SturdyAccelerationStructureBuildSizes *out_sizes);

/// Builds or updates `build_count` acceleration structures, recorded into the command encoder
/// outside any render/compute pass.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_build_acceleration_structures(
    SturdyCommandEncoder encoder, uint32_t build_count, const SturdyAccelerationStructureBuildDesc *builds);

typedef struct SturdyAccelerationStructureCopyDesc {
    SturdyAccelerationStructure src;
    SturdyAccelerationStructure dst;
    SturdyAccelerationStructureCopyMode mode;
} SturdyAccelerationStructureCopyDesc;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_copy_acceleration_structure(
    SturdyCommandEncoder encoder, const SturdyAccelerationStructureCopyDesc *copy);

/// One row of a top-level acceleration structure's instance buffer. Fixed 64-byte layout matching
/// `VkAccelerationStructureInstanceKHR`/`D3D12_RAYTRACING_INSTANCE_DESC` exactly (and
/// `RHI::AccelerationStructureInstance`) — write an array of these directly into a mapped/staged
/// buffer bound as `SturdyAccelerationStructureInstancesDesc::buffer`.
typedef struct SturdyAccelerationStructureInstance {
    /// Row-major 3x4 object-to-world transform (3 rows of 4 floats each).
    float transform[12];
    /// Packed via `sturdy_rhi_pack_instance_custom_index_and_mask`.
    uint32_t custom_index_and_mask;
    /// Packed via `sturdy_rhi_pack_instance_sbt_offset_and_flags`.
    uint32_t shader_binding_table_offset_and_flags;
    /// From `sturdy_rhi_acceleration_structure_device_address` on the referenced bottom-level
    /// structure.
    uint64_t acceleration_structure_device_address;
} SturdyAccelerationStructureInstance;

/// Packs a 24-bit custom index and 8-bit visibility mask into
/// `SturdyAccelerationStructureInstance::custom_index_and_mask`. Pure host-side bit packing —
/// takes no engine handle and cannot fail.
STURDY_ABI uint32_t STURDY_ABI_CALL sturdy_rhi_pack_instance_custom_index_and_mask(
    uint32_t custom_index, uint8_t mask);

/// Packs a 24-bit shader binding table offset and 8-bit instance flags into
/// `SturdyAccelerationStructureInstance::shader_binding_table_offset_and_flags`.
STURDY_ABI uint32_t STURDY_ABI_CALL sturdy_rhi_pack_instance_sbt_offset_and_flags(
    uint32_t sbt_offset, uint8_t flags);

/// One usage count entry within an opacity micromap build. Not supported on D3D12.
typedef struct SturdyOpacityMicromapUsageCount {
    uint32_t count;
    uint32_t subdivision_level;
    SturdyOpacityMicromapFormat format;
} SturdyOpacityMicromapUsageCount;

typedef struct SturdyOpacityMicromapDesc {
    uint32_t struct_size;
    uint32_t usage_count_count;
    const SturdyOpacityMicromapUsageCount *usage_counts;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyOpacityMicromapDesc;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_opacity_micromap_desc_init(SturdyOpacityMicromapDesc *desc);

typedef struct SturdyOpacityMicromapBuildSizes {
    /// Pass to `sturdy_rhi_create_opacity_micromap`'s `size` parameter.
    uint64_t micromap_size;
    uint64_t build_scratch_size;
} SturdyOpacityMicromapBuildSizes;

/// @return `STURDY_ERROR_NOT_AVAILABLE` on D3D12.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_opacity_micromap_build_sizes(
    SturdyEngine engine, const SturdyOpacityMicromapDesc *desc, SturdyOpacityMicromapBuildSizes *out_sizes);

/// @return `STURDY_ERROR_NOT_AVAILABLE` on D3D12.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_opacity_micromap(
    SturdyEngine engine, const SturdyOpacityMicromapDesc *desc, uint64_t size, SturdyOpacityMicromap *out_micromap);

/// Destroys an opacity micromap. A null/zero handle is a no-op; always safe to call even on
/// D3D12, where it is a no-op regardless of the handle.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_opacity_micromap(
    SturdyEngine engine, SturdyOpacityMicromap micromap);

typedef struct SturdyOpacityMicromapBuildDesc {
    SturdyOpacityMicromap dst;
    SturdyBuffer scratch_buffer;
    uint64_t scratch_offset;
    SturdyBuffer data_buffer;
    uint64_t data_buffer_offset;
    uint32_t usage_count_count;
    const SturdyOpacityMicromapUsageCount *usage_counts;
} SturdyOpacityMicromapBuildDesc;

/// @return `STURDY_ERROR_NOT_AVAILABLE` on D3D12.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_build_opacity_micromaps(
    SturdyCommandEncoder encoder, uint32_t build_count, const SturdyOpacityMicromapBuildDesc *builds);

/// One shader group within a ray tracing pipeline. `type` selects which entries are meaningful:
/// `general` for a raygen/miss/callable group, `closest_hit`/`any_hit`/`intersection` for a hit
/// group (a triangles hit group leaves `intersection` zeroed).
typedef struct SturdyRayTracingShaderGroupDesc {
    SturdyRayTracingShaderGroupType type;
    SturdyShaderEntry general;
    SturdyShaderEntry closest_hit;
    SturdyShaderEntry any_hit;
    SturdyShaderEntry intersection;
} SturdyRayTracingShaderGroupDesc;

typedef struct SturdyRayTracingPipelineDesc {
    uint32_t struct_size;
    SturdyPipelineLayout layout;
    uint32_t group_count;
    const SturdyRayTracingShaderGroupDesc *groups;
    uint32_t max_ray_recursion_depth;
    /// Optional debug label, copied at creation time. May be null.
    const char *label;
} SturdyRayTracingPipelineDesc;

/// Fills `desc` with `struct_size` and engine defaults (`max_ray_recursion_depth = 1`, no groups).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_ray_tracing_pipeline_desc_init(
    SturdyRayTracingPipelineDesc *desc);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_create_ray_tracing_pipeline(
    SturdyEngine engine, const SturdyRayTracingPipelineDesc *desc, SturdyRayTracingPipeline *out_pipeline);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_destroy_ray_tracing_pipeline(
    SturdyEngine engine, SturdyRayTracingPipeline pipeline);

/// Reads `group_count` shader group handles starting at `first_group`, into `dst` (which must be
/// at least `group_count * sturdy_rhi_ray_tracing_properties`'s `shader_group_handle_size` bytes).
/// Write these into a shader binding table buffer at `shader_group_base_alignment`-aligned
/// offsets to build `SturdyShaderBindingTableRegion`s for `sturdy_rhi_command_encoder_trace_rays`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_write_ray_tracing_shader_group_handles(
    SturdyEngine engine, SturdyRayTracingPipeline pipeline, uint32_t first_group, uint32_t group_count,
    void *dst, size_t dst_size);

/// Ray tracing device properties needed to size and align a shader binding table. Mirrors
/// `RHI::RayTracingProperties`.
typedef struct SturdyRayTracingProperties {
    uint32_t struct_size;
    uint32_t max_ray_recursion_depth;
    uint32_t shader_group_handle_size;
    uint32_t shader_group_base_alignment;
    uint32_t max_ray_hit_attribute_size;
    uint32_t max_acceleration_structure_geometry_count;
    uint32_t max_acceleration_structure_instance_count;
    uint64_t min_acceleration_structure_scratch_offset_alignment;
} SturdyRayTracingProperties;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_ray_tracing_properties(
    SturdyEngine engine, SturdyRayTracingProperties *out_properties);

/// Binds the ray tracing pipeline used by a subsequent `sturdy_rhi_command_encoder_trace_rays`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_set_ray_tracing_pipeline(
    SturdyCommandEncoder encoder, SturdyRayTracingPipeline pipeline);

/// One shader binding table region: a byte range in a buffer holding shader group handles written
/// by `sturdy_rhi_write_ray_tracing_shader_group_handles`. `stride` is ignored for `raygen` (a
/// single record); `miss`/`hit`/`callable` are strided tables.
typedef struct SturdyShaderBindingTableRegion {
    SturdyBuffer buffer;
    uint64_t offset;
    uint64_t size;
    uint64_t stride;
} SturdyShaderBindingTableRegion;

typedef struct SturdyTraceRaysDesc {
    SturdyShaderBindingTableRegion raygen;
    SturdyShaderBindingTableRegion miss;
    SturdyShaderBindingTableRegion hit;
    SturdyShaderBindingTableRegion callable;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} SturdyTraceRaysDesc;

/// Dispatches rays against the pipeline bound by `sturdy_rhi_command_encoder_set_ray_tracing_pipeline`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rhi_command_encoder_trace_rays(
    SturdyCommandEncoder encoder, const SturdyTraceRaysDesc *desc);

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
