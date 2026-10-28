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
#define STURDY_ABI_VERSION_MINOR 40u

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

// ---------------------------------------------------------------------------------------------
// Stack traces
// ---------------------------------------------------------------------------------------------
//
// When a call fails, `sturdy_last_error_message()` says what went wrong; the trace says WHERE, across languages. A trace is a list of
// frames, innermost first, drawn from three layers:
//
//   FOREIGN  the call site the calling language announced with `sturdy_trace_set_foreign_call_site` ("src/game/inventory.rs:118");
//   SCOPE    named engine scopes ("Renderer::submit_frame"): the engine's own logical stack. These need no symbols, survive stripped
//            builds and exist on WebAssembly, where a native stack cannot be walked;
//   UNWOUND  engine scopes an exception travelled through before it was caught at the ABI boundary: the logical stack at the throw;
//   NATIVE   machine frames as module + offset (+ symbol and file/line when the platform provides them). The offset is stable across
//            runs, so a foreign debugger can symbolize it offline against the module's debug info even when this process could not.
//
// Reading a trace never changes the error state, and a trace belongs to the thread that made the failing call. Strings inside a
// `SturdyTraceFrame` stay valid until the next trace call on the same thread.
//
// How much is captured is `SturdyTraceDetail`; the environment variable `STURDY_TRACE=off|scopes|native` sets the initial value.
// Failures that are part of ordinary use (`STURDY_ERROR_BUFFER_TOO_SMALL`, `_OUT_OF_RANGE`, `_ENTITY_NOT_ALIVE`, ...) record only the
// foreign call site, so size-query loops do not pay for a native stack walk.

typedef enum SturdyTraceDetail {
    /// Nothing is captured.
    STURDY_TRACE_DETAIL_OFF = 0,
    /// Foreign call site and engine scopes only. Cheap enough to leave on in release builds. The release-build default.
    STURDY_TRACE_DETAIL_SCOPES = 1,
    /// Scopes plus the native call stack. The debug-build default.
    STURDY_TRACE_DETAIL_NATIVE = 2,
    STURDY_TRACE_DETAIL_FORCE_U32 = 0x7fffffff
} SturdyTraceDetail;

typedef enum SturdyTraceFrameKind {
    /// A machine frame: `address`, `module`, `module_offset`, and `function`/`file`/`line` when known.
    STURDY_TRACE_FRAME_NATIVE = 0,
    /// A named engine scope: `function` is its name.
    STURDY_TRACE_FRAME_SCOPE = 1,
    /// The announced foreign call site: `file`, `line`, `column`, `function`; `module` holds the language tag.
    STURDY_TRACE_FRAME_FOREIGN = 2,
    /// An engine scope an exception unwound through: `function` is its name.
    STURDY_TRACE_FRAME_UNWOUND = 3,
    STURDY_TRACE_FRAME_KIND_FORCE_U32 = 0x7fffffff
} SturdyTraceFrameKind;

typedef struct SturdyTraceFrame {
    /// Set to `sizeof(SturdyTraceFrame)` before the call.
    uint32_t struct_size;
    SturdyTraceFrameKind kind;
    /// Absolute instruction address in this process (native frames), else 0.
    uint64_t address;
    /// Address relative to the load base of `module` (native frames), else 0.
    uint64_t module_offset;
    uint32_t line;
    uint32_t column;
    /// Never null; empty when unknown.
    const char *function;
    /// Never null; empty when unknown.
    const char *file;
    /// Never null. Native: the executable or library path. Foreign: the language tag.
    const char *module;
} SturdyTraceFrame;

/// Chooses how much future failures capture, for the whole process.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_set_detail(SturdyTraceDetail detail);

/// Reads the current capture detail.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_get_detail(SturdyTraceDetail *out_detail);

/// Announces where the calling thread's foreign code is, so a failure inside the engine can be traced back to a line of it. The site
/// stays in force until replaced or cleared, so announcing once per frame or per widget is as valid as before every call. The engine
/// copies the strings (they need not be null-terminated or outlive the call), and re-announcing an identical site allocates nothing.
///
/// @param language Short tag: "rust", "csharp", "java", "python", ...
/// @param file Source file; may be empty.
/// @param line 1-based line, or 0 if unknown.
/// @param column 1-based column, or 0 if unknown.
/// @param function Enclosing function; may be empty.
/// @note Never changes the error state.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_set_foreign_call_site(const char *language,
                                                                          uint32_t language_length,
                                                                          const char *file,
                                                                          uint32_t file_length,
                                                                          uint32_t line,
                                                                          uint32_t column,
                                                                          const char *function,
                                                                          uint32_t function_length);

/// Forgets the calling thread's foreign call site.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_clear_foreign_call_site(void);

/// Replaces the calling thread's trace with the stack at this call (foreign site, engine scopes, native stack per the detail setting).
/// For a foreign language that wants the engine's side of a stack at a moment of its choosing, such as a panic hook.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_capture(void);

/// Number of frames in the calling thread's most recent trace (the last failure's, or the last `sturdy_trace_capture`). Zero if none.
/// Call this before `sturdy_last_error_trace_frame`: it resolves the trace once so the frames can be read cheaply.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_frame_count(uint32_t *out_count);

/// Reads one frame of the trace counted by `sturdy_last_error_trace_frame_count`, innermost first.
///
/// @return `STURDY_ERROR_OUT_OF_RANGE` when `index` is past the end.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_frame(uint32_t index, SturdyTraceFrame *out_frame);

/// The whole trace as readable text, one frame per line, with the usual size-query convention (see `copy_string_out` in the ABI notes:
/// pass a null buffer to learn the size; a short buffer gets a truncated, terminated result and `STURDY_ERROR_BUFFER_TOO_SMALL`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_last_error_trace_format(char *buffer, uint64_t capacity, uint64_t *out_length);

/// Installs process-wide crash handlers (fatal signals on POSIX, the unhandled-exception filter on Windows) that print the crashing
/// thread's engine scopes and native stack before the process dies. Does nothing useful on WebAssembly (returns
/// `STURDY_ERROR_NOT_AVAILABLE`). Safe to call repeatedly.
///
/// @param log_path Also append the report to this file; null or empty for stderr only.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_trace_install_crash_handler(const char *log_path);

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
    /// Collect frame timings for `Renderer::last_frame_timings` (an application's own diagnostics overlay reads them).
    STURDY_RENDER_FEATURE_FRAME_TIMINGS = 6,
    /// Deprecated alias: the engine no longer draws a debug overlay; this now only enables frame timings.
    STURDY_RENDER_FEATURE_DEBUG_OVERLAY = STURDY_RENDER_FEATURE_FRAME_TIMINGS,
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

    /// Primary window client-area size in physical pixels.
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

/// One game-logic module of a game written in several languages (since 0.35): your callbacks plus where it sits in the
/// start order. A Rust module, a C# module and a C++ module can each pass their own `SturdyGameLogic` here.
typedef struct SturdyGameModule {
    /// Set to `sizeof(SturdyGameModule)`.
    uint32_t struct_size;
    /// Number of entries in `after`.
    uint32_t after_count;
    /// Unique, non-empty, null-terminated UTF-8 name that other modules' `after` lists refer to. Copied.
    const char *name;
    /// Names of modules that must start before this one and shut down after it. May be null when `after_count` is 0.
    /// Unknown names and cycles are reported when the modules start (see `sturdy_runtime_run_modules`).
    const char *const *after;
    /// Nonzero: this module may supply the scene (camera, lighting, render graph) for a frame. Zero: overlay-only, so its
    /// overlay passes are drawn but it never replaces the scene.
    SturdyBool contributes_scene;
    uint8_t reserved[7];
    /// The module's callbacks, with `struct_size` set as for `sturdy_runtime_run`.
    SturdyGameLogic logic;
} SturdyGameModule;

/// Like `sturdy_runtime_run`, for a game made of several modules (since 0.35).
///
/// Modules start in the order you list them, adjusted so each comes after the modules in its `after` list; they shut down in
/// the reverse order. If a module's `on_engine_initialized` returns false (or a dependency is unknown or cyclic), every module
/// that had started is shut down and the call returns `STURDY_ERROR_CALLBACK_FAILED` (or
/// `STURDY_ERROR_INITIALIZATION_FAILED` for a bad dependency, with the cause in the engine log).
///
/// For each frame every module's `request_render_frame` runs in start order. Among modules with `contributes_scene`, the
/// last one that returns true supplies the camera, lighting and render graph; every module's overlay passes are kept, in
/// start order. If no scene module returns true the frame is skipped.
///
/// Threading is as for `sturdy_runtime_run`: `request_render_frame` may run on per-window render threads, so a module's
/// own state must be safe for that. The module list never changes after startup.
///
/// @param modules `module_count` modules. Checked before anything starts: a bad struct size, an empty or duplicate name, a
///        module that lists itself in `after`, or no modules at all returns `STURDY_ERROR_INVALID_ARGUMENT` (or
///        `STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE`) and no `destroy` hook is run, so you still own every `user_data`. Once the
///        runtime starts, each module's `destroy` is invoked exactly once on every path.
/// @param config, argc, argv, out_exit_code As for `sturdy_runtime_run`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_runtime_run_modules(const SturdyRuntimeConfig *config,
                                                                   const SturdyGameModule *modules,
                                                                   uint32_t module_count,
                                                                   int32_t argc,
                                                                   const char *const *argv,
                                                                   int32_t *out_exit_code);

// ---------------------------------------------------------------------------------------------
// Live module changes (since 0.35)
// ---------------------------------------------------------------------------------------------
//
// While a game started with `sturdy_runtime_run_modules` runs, its modules can be unloaded, replaced (hot reload) or added. Every
// request returns immediately and is carried out at the start of the next engine update, the one moment nothing is running, so no
// system or frame callback is still inside the module being removed; frames wait for a change in progress. They may be called from
// any thread, including from inside the module being replaced.
//
// What a module registered while it started (systems, resources and event channels created through this ABI) goes away with it.
// Components cannot be unregistered and stay. A request that fails when it is carried out (a dependent module blocks the unload, a
// dependency is missing, the new module's `on_engine_initialized` returns false) is reported through
// `sturdy_module_take_failure`; the module's `destroy` hook runs for every module that is destroyed, as always.
//
// Ownership: a request that returns anything but `STURDY_OK` took nothing, so you still own the `user_data`. After `STURDY_OK` the
// engine owns it and calls `destroy` exactly once, whether or not the change later fails.

/// Unloads a module: its `on_shutdown` runs, what it registered is removed, and it is destroyed. Refused when another module names
/// it in `after`. Returns `STURDY_ERROR_NOT_AVAILABLE` when the game was not started with `sturdy_runtime_run_modules`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_request_unload(SturdyEngine engine, const char *name);

/// Replaces the module called `name` with `logic`, in the same place in the start order and with the same options. The old module
/// is shut down and its registrations removed first. If `logic`'s `on_engine_initialized` returns false the module stays unloaded
/// and the failure is reported.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_request_reload(SturdyEngine engine,
                                                                     const char *name,
                                                                     const SturdyGameLogic *logic);

/// Adds and starts a module in the running game, after the modules in `module->after` (which must already be loaded).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_request_load(SturdyEngine engine, const SturdyGameModule *module);

/// How many modules are loaded.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_count(SturdyEngine engine, uint32_t *out_count);

/// The name of the module at `index` in start order. See the string-output convention above.
/// @return `STURDY_ERROR_OUT_OF_RANGE` when `index` is not below `sturdy_module_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_name(SturdyEngine engine,
                                                           uint32_t index,
                                                           char *buffer,
                                                           size_t capacity,
                                                           size_t *out_length);

/// Takes the oldest message of a live change that failed, copying it out (see the string-output convention above); it is removed
/// only when the copy succeeded, so a too-small buffer can be retried.
/// @return `STURDY_ERROR_NOT_AVAILABLE` when no change has failed since the last call drained them.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_module_take_failure(SturdyEngine engine,
                                                                   char *buffer,
                                                                   size_t capacity,
                                                                   size_t *out_length);

// ---------------------------------------------------------------------------------------------
// Native plugins (since 0.36)
// ---------------------------------------------------------------------------------------------
//
// A plugin is a shared library built by its own language's toolchain (a Rust `cdylib`, C++, Zig, Go `c-shared`, .NET NativeAOT, ...)
// that holds one or more game modules. The engine loads it; the plugin does not own `main`, so any number of plugins written in any
// number of languages share one game, one frame and one ECS. Nothing is interpreted or sandboxed: a module is native code, so its
// systems read and write component columns directly and run in the engine's parallel schedule like any other system.
//
// A plugin exports one function, `sturdy_plugin_entry` (typedef `SturdyPluginEntryFn`), and links the engine's shared library the
// way every other binding does (so there is one copy of the engine's state in the process). The engine calls it twice in a plugin's
// life: with `STURDY_PLUGIN_LOAD`, during which the plugin describes itself and adds its modules through the host table, and with
// `STURDY_PLUGIN_UNLOAD`, once, after every module has been destroyed and just before the library is closed.
//
// Loading and starting are separate. `sturdy_plugin_load` runs `LOAD` and only collects the modules. Start them with the game
// (`sturdy_plugin_take_modules`, then `sturdy_runtime_run_modules`) or into a running one (`sturdy_plugin_request_load`).
//
// Reloading: build the new library, `sturdy_plugin_load` it with the old plugin as `previous`, then `sturdy_plugin_request_reload`.
// Modules with the same name are replaced in place, new names are added, names the new plugin lacks are unloaded. With
// `STURDY_PLUGIN_SHADOW_COPY` the file is copied before it is opened, so the build can overwrite the original while it runs (the
// loader would otherwise hand back the already-open image for the same path).
//
// Lifetime: the library stays open until the plugin handle has been released *and* every module it handed out has been destroyed,
// so a module's code is never unmapped while the engine may still call it. Threads the plugin started itself must be stopped by
// the plugin before its last module's `destroy` returns (or in `UNLOAD`); the engine cannot know about them.

/// A loaded plugin. Owned, not borrowed: valid until `sturdy_plugin_release`.
typedef struct SturdyPlugin {
    uint64_t token;
} SturdyPlugin;

/// Why the engine is calling a plugin's entry point.
typedef enum SturdyPluginReason {
    /// The library was just opened. Describe the plugin and add its modules through the host table.
    STURDY_PLUGIN_LOAD = 1,
    /// Every module has been destroyed and the library is about to be closed. Release whatever `set_state` stored.
    STURDY_PLUGIN_UNLOAD = 2,
    STURDY_PLUGIN_REASON_FORCE_32 = 0x7fffffff
} SturdyPluginReason;

/// What the engine offers a plugin from inside its entry point. Valid only for the duration of that call. Grows by appending; a
/// plugin checks `struct_size` covers the members it uses.
typedef struct SturdyPluginHost {
    /// `sizeof(SturdyPluginHost)` as the engine built it.
    uint32_t struct_size;
    /// `STURDY_ABI_VERSION_MAJOR` / `MINOR` the engine was built with. A plugin built for a different major refuses to load.
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;
    /// Pass back as the first argument of the functions below.
    void *context;
    /// Path of the library being loaded (the shadow copy when one was made). Null-terminated UTF-8.
    const char *path;
    /// The state the previous generation of this plugin stored with `set_state`, or null. Ownership moves to this generation: the
    /// previous generation's `UNLOAD` sees null. It must be plain data, with no pointers into the previous library's code or
    /// statics (that library is closed once the reload finishes).
    void *prior_state;
    /// Adds a module: `module` and everything it points to is copied, except `logic.user_data`, which the engine owns from here on
    /// (it calls `logic.destroy` exactly once, even if the plugin is never started). Names must be unique within the plugin.
    /// Returns `STURDY_ERROR_INVALID_ARGUMENT` or `STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE` for a bad module, and then took nothing.
    SturdyResult(STURDY_ABI_CALL *add_module)(void *context, const SturdyGameModule *module);
    /// Names the plugin and the language it is written in (for logs and `sturdy_plugin_language`), and a free-form version string.
    /// Any argument may be null. Copied.
    SturdyResult(STURDY_ABI_CALL *set_info)(void *context, const char *language, const char *plugin_name, const char *version);
    /// Stores a pointer that survives a reload (see `prior_state`) and is handed back through `state` at `UNLOAD`.
    void(STURDY_ABI_CALL *set_state)(void *context, void *state);
} SturdyPluginHost;

/// The one function a plugin exports, under the name `sturdy_plugin_entry`. Return `STURDY_OK` to accept the load; anything else
/// aborts it, destroys any modules already added, and `UNLOAD` is *not* called. For `UNLOAD`, `host->context` is valid but
/// `add_module` is refused, and `state` carries what the plugin stored.
typedef SturdyResult(STURDY_ABI_CALL *SturdyPluginEntryFn)(const SturdyPluginHost *host, SturdyPluginReason reason, void *state);

/// Flags for `SturdyPluginLoadDesc::flags`.
typedef enum SturdyPluginLoadFlags {
    /// Copy the library to a unique temporary file and open the copy, so the original can be rebuilt while the plugin runs.
    STURDY_PLUGIN_SHADOW_COPY = 1,
    STURDY_PLUGIN_LOAD_FLAGS_FORCE_32 = 0x7fffffff
} SturdyPluginLoadFlags;

/// How to load a plugin.
typedef struct SturdyPluginLoadDesc {
    /// Set to `sizeof(SturdyPluginLoadDesc)`.
    uint32_t struct_size;
    /// `SturdyPluginLoadFlags` bits.
    uint32_t flags;
    /// Path of the shared library, null-terminated UTF-8.
    const char *path;
    /// The plugin this one replaces, or a zero token. Its stored state is handed to this one as `prior_state`.
    SturdyPlugin previous;
} SturdyPluginLoadDesc;

/// Opens a plugin library and runs its entry point with `STURDY_PLUGIN_LOAD`, collecting its modules (nothing is started).
/// @return `STURDY_ERROR_NOT_AVAILABLE` when the library cannot be opened or exports no `sturdy_plugin_entry` (the cause is in the
///         error message); `STURDY_ERROR_INITIALIZATION_FAILED` when the entry point refuses; the entry point's own error otherwise.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_load(const SturdyPluginLoadDesc *desc, SturdyPlugin *out_plugin);

/// Releases the caller's handle. The library closes once every module that was handed out has been destroyed (immediately when
/// none was). Modules that were never handed out are destroyed now.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_release(SturdyPlugin plugin);

/// The language the plugin reported through `set_info`, empty when it did not. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_language(SturdyPlugin plugin, char *buffer, size_t capacity, size_t *out_length);
/// The plugin's reported name (falls back to the file name).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_name(SturdyPlugin plugin, char *buffer, size_t capacity, size_t *out_length);

/// How many modules the plugin added.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_module_count(SturdyPlugin plugin, uint32_t *out_count);

/// The name of module `index`, in the order the plugin added them.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_module_name(SturdyPlugin plugin, uint32_t index, char *buffer, size_t capacity,
                                                                  size_t *out_length);

/// Hands the plugin's modules to the caller, to pass to `sturdy_runtime_run_modules` (the returned strings and arrays stay valid
/// until `sturdy_plugin_release`). Each module's ownership moves to whatever consumes it, so this works once per plugin: a second
/// call returns `STURDY_ERROR_BUSY`. If `capacity` is smaller than the module count, `STURDY_ERROR_BUFFER_TOO_SMALL` is returned and
/// nothing is handed out; `out_count` always receives the count.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_take_modules(SturdyPlugin plugin, SturdyGameModule *out_modules, uint32_t capacity,
                                                                   uint32_t *out_count);

/// Adds every module of the plugin to the running game (as `sturdy_module_request_load` for each, in the order added). Consumes
/// the plugin's modules like `sturdy_plugin_take_modules`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_request_load(SturdyEngine engine, SturdyPlugin plugin);

/// Replaces `previous`'s modules with `next`'s in the running game: same-named modules are reloaded in place, new ones are loaded,
/// and names `previous` had that `next` lacks are unloaded. Consumes `next`'s modules. Release `previous` afterwards; its library
/// closes when its modules are gone.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_request_reload(SturdyEngine engine, SturdyPlugin previous, SturdyPlugin next);

/// Unloads every module of `plugin` from the running game. Release the handle afterwards.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_plugin_request_unload(SturdyEngine engine, SturdyPlugin plugin);

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

/// Clustered local lighting (since 0.32). Every spot and point light (nearest/brightest first, up to `max_lights`) is binned
/// into screen tiles x depth slices so each pixel evaluates only the lights that reach it; with `clustered` off the
/// deferred pass falls back to the first 8 spot + 8 point lights.
typedef struct SturdyLightingSettings {
    /// Set to `sizeof(SturdyLightingSettings)` by `sturdy_lighting_settings_init`.
    uint32_t struct_size;
    SturdyBool clustered;
    uint8_t reserved[3];
    uint32_t cluster_tile_px;
    uint32_t cluster_depth_slices;
    float cluster_max_distance;
    uint32_t max_lights;
    uint32_t max_lights_per_cluster;
} SturdyLightingSettings;

/// Fills `settings` with `struct_size` and engine defaults (clustered on).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_lighting_settings_init(SturdyLightingSettings *settings);

/// Replaces the clustered-lighting settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_lighting_settings(
    SturdyFrame frame, const SturdyLightingSettings *settings);

/// Froxel volumetric fog (since 0.32): height fog with optional wind-driven noise, lit by the sun (cascaded shadows), the
/// ambient term and every clustered local light (spot/point shadows), so flashlight and lamp beams show in the air.
typedef struct SturdyVolumetricFogSettings {
    /// Set to `sizeof(SturdyVolumetricFogSettings)` by `sturdy_volumetric_fog_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    SturdyBool sun_shadows;
    SturdyBool local_light_shadows;
    uint8_t reserved;
    /// Extinction per world unit at `base_height`, and its exponential falloff above it (0 = uniform).
    float density;
    float height_falloff;
    float base_height;
    /// Scattering fraction of extinction per channel, Henyey-Greenstein anisotropy, self-emission per unit length.
    float albedo[3];
    float anisotropy;
    float emissive[3];
    float sun_intensity;
    float ambient_intensity;
    float local_light_intensity;
    /// Fog is computed this far from the camera, in `slice_count` slices of `tile_px`-pixel froxels.
    float max_distance;
    uint32_t tile_px;
    uint32_t slice_count;
    /// 1 = linear slices, larger = more slices near the camera.
    float slice_distribution;
    /// Weight of the new frame in the temporal blend (lower = smoother, slower).
    float temporal_blend;
    float noise_strength;
    float noise_scale;
    float wind[3];
} SturdyVolumetricFogSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled — fog is opt-in).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_volumetric_fog_settings_init(SturdyVolumetricFogSettings *settings);

/// Replaces the volumetric-fog settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_volumetric_fog_settings(
    SturdyFrame frame, const SturdyVolumetricFogSettings *settings);

/// Indirect specular light (since 0.33): the sky prefiltered per roughness (so every surface reflects something and metals are
/// never black) plus deterministic (noise-free) screen-space reflections with cone-traced glossiness, which replace it where
/// the reflected surface is on screen. On by default.
typedef struct SturdyReflectionSettings {
    /// Set to `sizeof(SturdyReflectionSettings)` by `sturdy_reflection_settings_init`.
    uint32_t struct_size;
    SturdyBool environment;
    SturdyBool specular_occlusion;
    SturdyBool screen_space;
    uint8_t reserved;
    float environment_intensity;
    float screen_space_intensity;
    /// Surfaces rougher than this (perceptual roughness) use only the environment.
    float max_roughness;
    uint32_t max_steps;
    /// How far behind the depth buffer a surface is assumed to extend, as a fraction of its view depth.
    float thickness;
    /// Scales how fast the specular cone widens with distance (1 = the GGX lobe).
    float glossy_blur;
} SturdyReflectionSettings;

/// Fills `settings` with `struct_size` and engine defaults.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_settings_init(SturdyReflectionSettings *settings);

/// Replaces the reflection settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_reflection_settings(
    SturdyFrame frame, const SturdyReflectionSettings *settings);

/// Depth of field (since 0.34): the thin-lens blur of everything away from the focal plane, with the aperture's polygon as the
/// bokeh. With `from_camera` set the engine overwrites the lens fields from the camera every frame.
typedef struct SturdyDepthOfFieldSettings {
    /// Set to `sizeof(SturdyDepthOfFieldSettings)` by `sturdy_depth_of_field_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    SturdyBool from_camera;
    SturdyBool near_field;
    uint8_t reserved;
    /// Distance (metres) that is perfectly sharp, f-number, focal length and sensor height in millimetres.
    float focus_distance;
    float aperture_f_stop;
    float focal_length_mm;
    float sensor_height_mm;
    /// Aperture blades (3..32) and their rotation.
    uint32_t blades;
    float blade_rotation_degrees;
    /// Cap on the blur diameter in pixels, and a multiplier on the blur.
    float max_blur_px;
    float strength;
    /// Gather samples per pixel: 0 = 24, 1 = 40, 2 = 64.
    uint32_t quality;
} SturdyDepthOfFieldSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_depth_of_field_settings_init(SturdyDepthOfFieldSettings *settings);

/// Replaces the depth-of-field settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_depth_of_field_settings(
    SturdyFrame frame, const SturdyDepthOfFieldSettings *settings);

/// Colour grading (since 0.34), on the scene-linear image just before tone mapping: white balance, exposure offset, contrast,
/// lift / gamma / gain, split toning, saturation and an optional log-domain 3D LUT. Every control is neutral at its default.
typedef struct SturdyColorGradeSettings {
    /// Set to `sizeof(SturdyColorGradeSettings)` by `sturdy_color_grade_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    /// White balance, -1..1 (cool..warm, green..magenta).
    float temperature;
    float tint;
    float exposure_ev;
    float contrast;
    float saturation;
    float lift[3];
    float gamma[3];
    float gain[3];
    float shadow_tint[3];
    float highlight_tint[3];
    float split_strength;
    /// Renderer texture handle value of a LUT made by the C++ `Engine::create_color_lut`; 0 = none.
    uint64_t lut_texture;
    float lut_intensity;
} SturdyColorGradeSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled, neutral).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_color_grade_settings_init(SturdyColorGradeSettings *settings);

/// Replaces the colour-grade settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_color_grade_settings(
    SturdyFrame frame, const SturdyColorGradeSettings *settings);

typedef struct SturdyAutoExposureSettings {
    /// Set to `sizeof(SturdyAutoExposureSettings)` by `sturdy_auto_exposure_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    /// Luminance range the histogram meters, as log2(scene-linear luminance).
    float min_log2_luminance;
    float max_log2_luminance;
    /// Fractions of the metered pixel mass ignored at the dark / bright end (`0 <= low < high <= 1`).
    float low_percent;
    float high_percent;
    /// Luminance the metered average is mapped to (middle grey).
    float key_value;
    /// Exposure compensation in stops.
    float compensation_ev;
    float min_exposure;
    float max_exposure;
    /// Adaptation rates per second when the scene darkens (exposure rises) / brightens.
    float adapt_up_speed;
    float adapt_down_speed;
    /// 0 = every pixel counts equally, 1 = the frame edges count for nothing.
    float center_weight;
} SturdyAutoExposureSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled — auto exposure is opt-in).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_auto_exposure_settings_init(SturdyAutoExposureSettings *settings);

/// Replaces the histogram auto-exposure stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_auto_exposure_settings(
    SturdyFrame frame, const SturdyAutoExposureSettings *settings);

typedef struct SturdyCameraEmulationSettings {
    /// Set to `sizeof(SturdyCameraEmulationSettings)` by `sturdy_camera_emulation_settings_init`.
    uint32_t struct_size;
    SturdyBool enabled;
    uint8_t reserved[3];
    /// Barrel distortion, 0 = none; the image is zoomed so the corners still reach the frame edge.
    float fisheye_strength;
    /// Radial red/blue separation as a fraction of the frame half-size at the corners.
    float chromatic_aberration;
    float vignette_strength;
    /// Brightness-dependent temporal sensor noise, 0 = none.
    float sensor_noise;
    float sharpen;
    float saturation;
    /// Contrast about middle grey (1 = unchanged).
    float contrast;
    /// White-balance multiplier (1, 1, 1 = neutral).
    float tint_r;
    float tint_g;
    float tint_b;
    /// Darkens a rounded-rectangle "camera housing" frame around the edges, 0 = none.
    float housing;
} SturdyCameraEmulationSettings;

typedef struct SturdyScreenSpaceGiSettings {
    /// Set to `sizeof(SturdyScreenSpaceGiSettings)` by `sturdy_screen_space_gi_settings_init`.
    uint32_t struct_size;
    /// Screen-space indirect lighting (no ray tracing needed, every backend). Used when ReSTIR GI is off.
    SturdyBool enabled;
    uint8_t reserved[3];
    float intensity;
    /// How far, in world units, a surface can light its neighbours.
    float radius;
    /// Assumed thickness of what the depth buffer shows, in world units.
    float thickness;
    /// Search directions per pixel and depth taps per direction (at half resolution).
    uint32_t slice_count;
    uint32_t step_count;
    /// Weight of the newest frame in the temporal accumulation.
    float temporal_alpha;
    /// Brightest scene-linear luminance a texel may contribute.
    float max_radiance;
} SturdyScreenSpaceGiSettings;

/// Fills `settings` with `struct_size` and engine defaults (disabled).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_screen_space_gi_settings_init(SturdyScreenSpaceGiSettings *settings);

/// Replaces the screen-space GI stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_screen_space_gi_settings(
    SturdyFrame frame, const SturdyScreenSpaceGiSettings *settings);

/// Fills `settings` with `struct_size` and engine defaults (disabled — the body-camera look is opt-in).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_camera_emulation_settings_init(SturdyCameraEmulationSettings *settings);

/// Replaces the camera-emulation stage's settings for the frame being built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_emulation_settings(
    SturdyFrame frame, const SturdyCameraEmulationSettings *settings);

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

/// The scalar kinds a component field can have (since 0.35). These are what two languages must agree on to share a
/// component's memory: a C `float`, a Rust `f32` and a C# `float` are all `STURDY_FIELD_F32`. Values never change.
typedef enum SturdyFieldKind {
    STURDY_FIELD_BOOL = 0,
    STURDY_FIELD_I8 = 1,
    STURDY_FIELD_I16 = 2,
    STURDY_FIELD_I32 = 3,
    STURDY_FIELD_I64 = 4,
    STURDY_FIELD_U8 = 5,
    STURDY_FIELD_U16 = 6,
    STURDY_FIELD_U32 = 7,
    STURDY_FIELD_U64 = 8,
    STURDY_FIELD_F32 = 9,
    STURDY_FIELD_F64 = 10,
    STURDY_FIELD_KIND_FORCE_U32 = 0x7fffffff
} SturdyFieldKind;

/// One field of a component you are registering: `count` consecutive values of `kind`, `offset` bytes into the component
/// (`float position[3]` is one field with `count == 3`).
typedef struct SturdyComponentField {
    /// Set to `sizeof(SturdyComponentField)`.
    uint32_t struct_size;
    /// A `SturdyFieldKind`.
    uint32_t kind;
    uint32_t offset;
    /// Number of consecutive values; must be nonzero.
    uint32_t count;
    /// For diagnostics only: null-terminated UTF-8, copied, may be null. It is NOT part of the layout comparison, because
    /// naming conventions differ per language while the memory is the same.
    const char *name;
} SturdyComponentField;

/// A component to register with its field layout (since 0.35).
typedef struct SturdyComponentDesc {
    /// Set to `sizeof(SturdyComponentDesc)`.
    uint32_t struct_size;
    uint32_t size;
    uint32_t align;
    /// Number of entries in `fields`; 0 registers without describing the layout, as `sturdy_ecs_register_component` does.
    uint32_t field_count;
    /// Canonical, globally unique name, as for `sturdy_ecs_register_component`.
    const char *name;
    /// `field_count` fields, in any order. May be null when `field_count` is 0.
    const SturdyComponentField *fields;
} SturdyComponentDesc;

/// Registers a component and its field layout (since 0.35), so languages sharing it can be checked against each other.
///
/// Behaves like `sturdy_ecs_register_component` (same name and layout returns the existing id; a different size or alignment is
/// rejected), and additionally: the fields must fit inside `size` and not overlap, and when this registration and an earlier
/// one of the same name both describe their fields, they must describe the same layout (kinds, offsets, counts; field names
/// are ignored). A mismatch fails with `STURDY_ERROR_INVALID_ARGUMENT` and a message naming the first field that differs, so a
/// Rust `{f32, f32}` and a C# `{u32, u32}` cannot silently share an 8-byte component. A registration that gives no fields is
/// still accepted against any layout.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_register_component_ex(SturdyEngine engine,
                                                                         const SturdyComponentDesc *desc,
                                                                         SturdyComponentId *out_component);

/// One field of a registered component, as read back.
typedef struct SturdyComponentFieldInfo {
    /// Set by the engine to `sizeof(SturdyComponentFieldInfo)`.
    uint32_t struct_size;
    SturdyFieldKind kind;
    uint32_t offset;
    uint32_t count;
} SturdyComponentFieldInfo;

/// How many fields a component was registered with (since 0.35); 0 when its registrant did not describe them.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_field_count(SturdyEngine engine,
                                                                         SturdyComponentId component,
                                                                         uint32_t *out_count);

/// Reads field `index`, in the order the fields were registered in.
///
/// @return `STURDY_ERROR_OUT_OF_RANGE` when `index` is not below `sturdy_ecs_component_field_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_field(SturdyEngine engine,
                                                                   SturdyComponentId component,
                                                                   uint32_t index,
                                                                   SturdyComponentFieldInfo *out_field);

/// Reads field `index`'s name. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_field_name(SturdyEngine engine,
                                                                        SturdyComponentId component,
                                                                        uint32_t index,
                                                                        char *buffer,
                                                                        size_t capacity,
                                                                        size_t *out_length);

/// The 128-bit fingerprint of a component's registered layout (size, alignment, every field's kind, offset and count), as two
/// words; both zero when its registrant did not describe the fields. Lets a binding compare against a value baked in at build
/// time without re-implementing the hash.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_component_layout_fingerprint(SturdyEngine engine,
                                                                                SturdyComponentId component,
                                                                                uint64_t *out_high,
                                                                                uint64_t *out_low);

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

/// Handle to a system registered through `sturdy_ecs_add_system_ex`.
typedef struct SturdySystem {
    uint64_t token;
} SturdySystem;

/// Like `sturdy_ecs_add_system_with_resources`, but returns a handle so the system can later be
/// removed, disabled, or ordered. Use this when the system's `user_data` is owned by something
/// that can be destroyed (a script VM, a closure): remove the system first, then free the data.
STURDY_ABI SturdyResult STURDY_ABI_CALL
sturdy_ecs_add_system_ex(SturdyEngine engine,
                         const SturdySystemAccess *access,
                         uint32_t access_count,
                         const SturdySystemResourceAccess *resource_access,
                         uint32_t resource_access_count,
                         SturdySystemFn system,
                         void *user_data,
                         SturdySystem *out_system);

/// Body of a system registered with `sturdy_ecs_add_chunk_system_ex` (since 0.35): called once per matching archetype with
/// that archetype's contiguous arrays, instead of once per entity.
///
/// `entities` holds `count` entities. `columns[i]` is the first element of an array of `count` components, one array per entry
/// of the access list in the order it was given, each element as large as that component's size; element `r` of every array
/// belongs to `entities[r]`. Write through the arrays declared `STURDY_ACCESS_WRITE`. All pointers are valid only for this
/// call. `commands` is as for `SturdySystemFn`.
///
/// One call crosses the language boundary for a whole archetype, so a system over 10,000 entities costs a few calls instead of
/// 10,000: use this for anything per-frame over many entities.
typedef void(STURDY_ABI_CALL *SturdyChunkSystemFn)(const SturdyEntity *entities,
                                                   uint32_t count,
                                                   void *const *columns,
                                                   SturdyCommands commands,
                                                   void *user_data);

/// Registers a chunk system (since 0.35) with a removable handle. Declaration, ordering, conflict detection, `commands` and
/// removal behave exactly as for `sturdy_ecs_add_system_ex`; only the body differs.
///
/// @param access Components to iterate. At least one is required: a chunk system always iterates, and a system with nothing to
///        iterate belongs on `sturdy_ecs_add_system_ex` as a global system.
/// @param system Body to run. Must not unwind.
STURDY_ABI SturdyResult STURDY_ABI_CALL
sturdy_ecs_add_chunk_system_ex(SturdyEngine engine,
                               const SturdySystemAccess *access,
                               uint32_t access_count,
                               const SturdySystemResourceAccess *resource_access,
                               uint32_t resource_access_count,
                               SturdyChunkSystemFn system,
                               void *user_data,
                               SturdySystem *out_system);

/// Unregisters a system. After this returns the engine no longer references its `user_data`.
/// Fails with `STURDY_ERROR_BUSY` semantics (a contract violation) if called while a schedule is
/// running, including from inside a system.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_remove_system(SturdyEngine engine, SturdySystem system);

/// Skips (or resumes) a system without unregistering it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_set_system_enabled(SturdyEngine engine, SturdySystem system, SturdyBool enabled);

/// Forces `system` to run in a later stage than `dependency`, in addition to whatever ordering the
/// declared access already implies. A cycle aborts when the schedule is next built.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_order_system_after(SturdyEngine engine, SturdySystem system, SturdySystem dependency);

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

/// Like `sturdy_ecs_create_resource`, additionally describing the value's fields (since 0.35).
///
/// @param desc Name, `size` and fields, as for `sturdy_ecs_register_component_ex`; `align` is ignored (the engine allocates
///        the storage). Re-creating a resource under the same name must give the same size and, when both calls describe their
///        fields, the same layout: two languages that share a resource must agree on what is in it, not just how big it is.
///        A mismatch fails with `STURDY_ERROR_INVALID_ARGUMENT` and names the first field that differs.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_create_resource_ex(SturdyEngine engine,
                                                                      const SturdyComponentDesc *desc,
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

/// Like `sturdy_ecs_create_event_channel`, additionally describing the event payload's fields (since 0.35).
///
/// An event channel is how two modules written in different languages talk to each other, so they must agree on the payload.
/// `desc->size` is the event size and `align` is ignored. Creating the channel again under the same name must give the same
/// size and, when both calls describe their fields, the same layout (kinds, offsets, counts; names are ignored); a mismatch
/// fails with `STURDY_ERROR_INVALID_ARGUMENT` and names the first field that differs.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ecs_create_event_channel_ex(SturdyEngine engine,
                                                                           const SturdyComponentDesc *desc,
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
// Audio
// ---------------------------------------------------------------------------------------------
//
// Mixing, spatialisation (2D, 3D, any speaker layout up to 32 channels), streamed sources (voice chat, video soundtracks),
// microphones, custom sinks and effects. Sounds, voices, streams, sinks and captures are owned handles: they outlive the
// callback that made them and live until their `_release`/`_stop` call. Release every audio handle before the engine shuts
// down. Handles of voices that finished are inert, not invalid.

typedef struct SturdySound { uint64_t token; } SturdySound;
typedef struct SturdyVoice { uint64_t token; } SturdyVoice;
typedef struct SturdyAudioStream { uint64_t token; } SturdyAudioStream;
typedef struct SturdyAudioSink { uint64_t token; } SturdyAudioSink;
typedef struct SturdyAudioCapture { uint64_t token; } SturdyAudioCapture;

typedef enum SturdyAudioLayout {
    STURDY_AUDIO_LAYOUT_STEREO = 0,
    STURDY_AUDIO_LAYOUT_MONO = 1,
    STURDY_AUDIO_LAYOUT_QUAD = 2,
    STURDY_AUDIO_LAYOUT_5_1 = 3,
    STURDY_AUDIO_LAYOUT_7_1 = 4,
    STURDY_AUDIO_LAYOUT_7_1_4 = 5,
    STURDY_AUDIO_LAYOUT_9_1_6 = 6,
    STURDY_AUDIO_LAYOUT_22_2 = 7,
    /// Whatever the conventional layout is for `channel_count` speakers (3, 5, 7, 9, 11 ... any count up to 32).
    STURDY_AUDIO_LAYOUT_CHANNEL_COUNT = 8,
    STURDY_AUDIO_LAYOUT_FORCE_32_BIT = 0x7fffffff
} SturdyAudioLayout;

typedef struct SturdyAudioConfig {
    /// Set to `sizeof(SturdyAudioConfig)`.
    uint32_t struct_size;
    /// 0 selects 48000.
    uint32_t sample_rate;
    SturdyAudioLayout layout;
    /// Used with `STURDY_AUDIO_LAYOUT_CHANNEL_COUNT`.
    uint32_t channel_count;
    /// 0 selects the defaults (8192 voices, 4096 audible at once, helper threads chosen by the engine).
    uint32_t max_voices;
    uint32_t max_physical_voices;
    uint32_t mix_threads;
    /// Open the default playback device. False mixes silently (headless tools, tests, or when a custom sink takes the output).
    SturdyBool open_device;
} SturdyAudioConfig;

/// Turns audio on (replacing any previous setup). Returns `STURDY_ERROR_NOT_AVAILABLE` when the playback device would not open;
/// the mixer is then running silently anyway, so sinks, streams and offline use keep working.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_enable(SturdyEngine engine, const SturdyAudioConfig *config);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_disable(SturdyEngine engine);

/// Makes `entity` the listener (the highest `priority` wins when several exist). The entity needs a world transform.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_listener_attach(SturdyEngine engine, SturdyEntity entity, float priority);

typedef struct SturdyAudioStats {
    uint32_t voices;
    uint32_t audible;
    uint32_t virtualised;
    float master_peak;
} SturdyAudioStats;
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stats(SturdyEngine engine, SturdyAudioStats *out_stats);

/// Decodes a file (any supported codec; cached by path) without playing it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sound_load(SturdyEngine engine, const char *path, SturdySound *out_sound);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sound_release(SturdySound sound);

typedef struct SturdyPlayOptions {
    /// Set to `sizeof(SturdyPlayOptions)`.
    uint32_t struct_size;
    /// 0 is treated as 1.
    float volume;
    /// 0 is treated as 1; 2 is an octave up.
    float pitch;
    /// False plays un-positioned (UI, music).
    SturdyBool spatial;
    SturdyBool loop;
    /// World position when `spatial`.
    float position[3];
    /// Seconds to wait before starting.
    float delay_seconds;
    float fade_in_seconds;
} SturdyPlayOptions;

/// Plays a loaded sound. `out_voice` may be null for fire-and-forget; a returned voice must still be released.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sound_play(SturdyEngine engine, SturdySound sound, const SturdyPlayOptions *options, SturdyVoice *out_voice);
/// Streams a long file (music, ambience) from a decode thread instead of loading it whole.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_file(SturdyEngine engine, const char *path, const SturdyPlayOptions *options, SturdyVoice *out_voice);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_release(SturdyVoice voice);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_set_volume(SturdyVoice voice, float volume);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_fade_volume(SturdyVoice voice, float target, float seconds);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_set_pitch(SturdyVoice voice, float pitch);
/// Moves a spatial voice; `velocity` (may be null) feeds Doppler shift.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_set_position(SturdyVoice voice, const float position[3], const float *velocity);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_stop(SturdyVoice voice, float fade_seconds);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_pause(SturdyVoice voice);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_resume(SturdyVoice voice);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_seek(SturdyVoice voice, double seconds);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_voice_state(SturdyVoice voice, SturdyBool *out_playing, SturdyBool *out_finished, double *out_position_seconds);

/// How a sound attached to an entity is anchored.
typedef enum SturdyAudioAttachment {
    /// Fixed at `offset`, read as a world position.
    STURDY_AUDIO_ATTACH_WORLD = 0,
    /// Follows the entity's transform plus `offset` in its local space (parenting to a moving object).
    STURDY_AUDIO_ATTACH_ENTITY = 1,
    /// Stays at `offset` from the listener's head.
    STURDY_AUDIO_ATTACH_LISTENER = 2,
    STURDY_AUDIO_ATTACH_FORCE_32_BIT = 0x7fffffff
} SturdyAudioAttachment;

/// Gives an entity a sound source that follows it (the engine's audio systems keep it positioned).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_source_attach(SturdyEngine engine, SturdyEntity entity, SturdySound sound,
                                                                   SturdyAudioAttachment attachment, const float offset[3],
                                                                   const SturdyPlayOptions *options);

/// A source you feed yourself: voice chat, a video soundtrack, procedural audio. Interleaved float frames in, any channel count
/// up to 256; sample-rate differences and clock drift are corrected on playback.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_create(SturdyEngine engine, uint32_t channels, uint32_t sample_rate,
                                                                   float buffer_seconds, SturdyAudioStream *out_stream);
/// Appends frames; `out_accepted` (may be null) receives how many fit.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_push(SturdyAudioStream stream, const float *interleaved, uint32_t frames, uint32_t *out_accepted);
/// Marks the end of the stream; the voice finishes when it has drained.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_close(SturdyAudioStream stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_buffered_seconds(SturdyAudioStream stream, double *out_seconds);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_play(SturdyEngine engine, SturdyAudioStream stream, const SturdyPlayOptions *options, SturdyVoice *out_voice);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_stream_release(SturdyAudioStream stream);

/// Opens a microphone or audio interface at any channel count it offers. `device_name` null or empty selects the default input.
/// The capture is playable and readable through the stream it exposes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_capture_start(const char *device_name, uint32_t sample_rate, uint32_t channels,
                                                                   SturdyAudioCapture *out_capture);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_capture_info(SturdyAudioCapture capture, uint32_t *out_channels, uint32_t *out_sample_rate);
/// Highest absolute sample of a channel since the last call (for level meters).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_capture_peak(SturdyAudioCapture capture, uint32_t channel, float *out_peak);
/// Plays the capture through the engine (voice chat monitoring, effects chains).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_capture_play(SturdyEngine engine, SturdyAudioCapture capture, const SturdyPlayOptions *options, SturdyVoice *out_voice);
/// Closes the device. Voices playing the capture end.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_capture_stop(SturdyAudioCapture capture);

/// Receives mixed audio from a custom sink. Runs on a thread of the engine's, never the audio thread; `interleaved` holds
/// `frames * channels` floats and is only valid during the call.
typedef void(STURDY_ABI_CALL *SturdyAudioSinkCallback)(void *user_data, const float *interleaved, uint32_t frames, uint32_t channels, uint32_t sample_rate);

/// Sends one engine output to `callback`. Output 0 is the primary output: the sink then drives the mix itself (real-time, or as
/// fast as possible when `realtime` is false), so combine it with `open_device = false`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_sink_start(SturdyEngine engine, uint32_t output, SturdyAudioSinkCallback callback, void *user_data,
                                                                uint32_t block_frames, SturdyBool realtime, SturdyAudioSink *out_sink);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_sink_stop(SturdyAudioSink sink);

/// Appends an effect to the master bus by catalogue name ("filter", "equalizer", "noise_reduction", "compressor", "limiter", ...;
/// see `sturdy_audio_effect_name`) with named parameters (`count` pairs of `names[i]`/`values[i]`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_master_effect_add(SturdyEngine engine, const char *kind, const char *const *names, const float *values, uint32_t count);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_effect_count(uint32_t *out_count);
/// Reads the catalogue name of effect `index`. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_audio_effect_name(uint32_t index, char *buffer, size_t capacity, size_t *out_length);

// ---------------------------------------------------------------------------------------------
// Model import, animation and animation graphs
// ---------------------------------------------------------------------------------------------
//
// `sturdy_model_import` loads glTF/GLB, FBX and OBJ into the same `SturdyGltfScene` the glTF functions above use,
// including skeletons, blend shapes and every animation the file carries. `sturdy_scene_spawn` then creates the
// entities with animation already wired up (skinned characters play, animated props move). Control playback per
// entity with the `sturdy_animation_*` calls; animation can also come from separate files (Mixamo, BVH mocap) and be
// retargeted onto a character.

/// Imports a model file of any supported format, chosen from its extension: `.gltf`/`.glb`, `.fbx`, `.obj`.
/// Same ownership and release rules as `sturdy_gltf_import`; the handle works with every `sturdy_gltf_*` call.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_model_import(SturdyEngine engine,
                                                            const char *source,
                                                            SturdyAsset shader,
                                                            SturdyGltfScene *out_scene);

/// Returns how many animation clips the imported scene carries.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scene_clip_count(SturdyGltfScene scene, uint32_t *out_count);

/// Reads the name of clip `index`. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scene_clip_name(SturdyGltfScene scene,
                                                               uint32_t index,
                                                               char *buffer,
                                                               size_t capacity,
                                                               size_t *out_length);

/// Reads the length of clip `index` in seconds.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scene_clip_duration(SturdyGltfScene scene,
                                                                   uint32_t index,
                                                                   float *out_seconds);

/// How `sturdy_scene_spawn` places and starts a scene.
typedef struct SturdySpawnOptions {
    /// Set to `sizeof(SturdySpawnOptions)`.
    uint32_t struct_size;
    /// Start playing as soon as the entities exist.
    SturdyBool auto_play;
    /// Loop the starting clip.
    SturdyBool loop;
    uint16_t reserved;
    /// Playback speed multiplier.
    float speed;
    /// Clip to start with, or null for the file's first.
    const char *initial_clip;
    /// Column-major 4x4 transform applied on top of the file's own placement (identity to keep it as authored).
    float transform[16];
} SturdySpawnOptions;

/// Fills `options` with the defaults: auto-play, looping, speed 1, first clip, identity transform.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_spawn_options_init(SturdySpawnOptions *options);

/// Spawns every model and light of the scene with its animation wired up: skinned characters get a private
/// deformable copy and an animator, blend-shape meshes likewise, and plain animated nodes follow the file's node
/// animation. `sturdy_gltf_spawn_all` is this with default options.
///
/// @param options Null for the defaults.
/// @param out_entities Optional array receiving the created entities (renderers first, then lights).
/// @param capacity Length of `out_entities`.
/// @param out_count Receives how many entities were created (may exceed `capacity`). May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scene_spawn(SturdyEngine engine,
                                                           SturdyGltfScene scene,
                                                           const SturdySpawnOptions *options,
                                                           SturdyEntity *out_entities,
                                                           uint32_t capacity,
                                                           uint32_t *out_count);

/// Starts the named clip from its beginning on an animated entity (a character or a node of an animated prop).
/// Fails with `STURDY_ERROR_NOT_AVAILABLE` when the entity has no clip of that name or no animation.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_play(SturdyEngine engine,
                                                              SturdyEntity entity,
                                                              const char *clip_name,
                                                              SturdyBool loop,
                                                              float speed);

/// Scales playback speed (1 = authored speed, 0 freezes, negative runs backwards where supported).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_speed(SturdyEngine engine,
                                                                   SturdyEntity entity,
                                                                   float speed);

/// Pauses or resumes the entity's animation.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_playing(SturdyEngine engine,
                                                                     SturdyEntity entity,
                                                                     SturdyBool playing);

/// Returns how many clips the entity can play by name.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_clip_count(SturdyEngine engine,
                                                                    SturdyEntity entity,
                                                                    uint32_t *out_count);

/// Reads the name of the entity's clip `index`. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_clip_name(SturdyEngine engine,
                                                                   SturdyEntity entity,
                                                                   uint32_t index,
                                                                   char *buffer,
                                                                   size_t capacity,
                                                                   size_t *out_length);

/// Makes the entity's animation graph-driven. `graph_json` (`graph_length` bytes, or 0 for null-terminated) is an
/// animation graph document: parameters, clip/blend/additive/state-machine nodes, layers with joint masks and root
/// motion. Clips are referenced by name from the entity's own clips (add more first with
/// `sturdy_animation_adopt`). On failure the error message says what is wrong with the document.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_graph_attach(SturdyEngine engine,
                                                                      SturdyEntity entity,
                                                                      const char *graph_json,
                                                                      size_t graph_length);

/// Sets a float/int graph parameter. Unknown names are ignored.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_graph_set_float(SturdyEngine engine,
                                                                         SturdyEntity entity,
                                                                         const char *name,
                                                                         float value);

/// Sets a bool graph parameter.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_graph_set_bool(SturdyEngine engine,
                                                                        SturdyEntity entity,
                                                                        const char *name,
                                                                        SturdyBool value);

/// Fires a trigger parameter (it resets once a transition consumes it).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_graph_trigger(SturdyEngine engine,
                                                                       SturdyEntity entity,
                                                                       const char *name);

/// Reads the name of the graph's current state-machine state. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_graph_state(SturdyEngine engine,
                                                                     SturdyEntity entity,
                                                                     char *buffer,
                                                                     size_t capacity,
                                                                     size_t *out_length);

/// Animation loaded from a file on its own (BVH mocap, or glTF/FBX with no mesh such as Mixamo downloads).
/// Owned: valid until `sturdy_animation_set_release`.
typedef struct SturdyAnimationSet {
    uint64_t token;
} SturdyAnimationSet;

/// Loads only the animation from `.bvh`, `.gltf`/`.glb` or `.fbx`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_import(SturdyEngine engine,
                                                                    const char *source,
                                                                    SturdyAnimationSet *out_set);

/// Releases an animation set. Clips already adopted by entities keep working.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_release(SturdyAnimationSet set);

/// Returns how many clips the set holds.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_clip_count(SturdyAnimationSet set, uint32_t *out_count);

/// Reads the name of clip `index` of the set. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_set_clip_name(SturdyAnimationSet set,
                                                                       uint32_t index,
                                                                       char *buffer,
                                                                       size_t capacity,
                                                                       size_t *out_length);

/// Retargets every clip of the set onto the entity's skeleton (by bone name when the rigs match, through a humanoid
/// mapping otherwise) and adds them to the entity so `sturdy_animation_play` can play them.
///
/// @param out_added Receives how many clips were added. May be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_adopt(SturdyEngine engine,
                                                               SturdyEntity entity,
                                                               SturdyAnimationSet set,
                                                               uint32_t *out_added);

/// Adds a two-bone IK constraint (a leg onto the ground, a hand onto a prop), solved every frame after the
/// animation. Joints are named as in the skeleton; `pole` is the model-space direction the knee/elbow bends toward
/// (zero = keep the animated bend).
///
/// @param out_index Receives the constraint index for `sturdy_animation_ik_set_target`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_ik_add_two_bone(SturdyEngine engine,
                                                                         SturdyEntity entity,
                                                                         const char *root_joint,
                                                                         const char *mid_joint,
                                                                         const char *tip_joint,
                                                                         const float pole[3],
                                                                         uint32_t *out_index);

/// Moves a two-bone IK constraint's target (model space) and sets its blend weight (0 = off, 1 = fully solved).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_ik_set_target(SturdyEngine engine,
                                                                       SturdyEntity entity,
                                                                       uint32_t index,
                                                                       const float target[3],
                                                                       float weight);

/// Adds a look-at constraint (head tracking): the joint's `forward_axis` (local) turns toward the target, limited to
/// `max_angle_degrees` from the animated direction.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_look_at_add(SturdyEngine engine,
                                                                     SturdyEntity entity,
                                                                     const char *joint,
                                                                     const float forward_axis[3],
                                                                     float max_angle_degrees,
                                                                     uint32_t *out_index);

/// Moves a look-at constraint's target (model space) and sets its blend weight.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_animation_look_at_set_target(SturdyEngine engine,
                                                                            SturdyEntity entity,
                                                                            uint32_t index,
                                                                            const float target[3],
                                                                            float weight);

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

/// How UI units map onto the framebuffer.
typedef enum SturdyUiScaleMode {
    /// One UI unit covers `sturdy_window_content_scale` pixels, so the UI keeps its physical size on
    /// every display and text stays sharp. The default.
    STURDY_UI_SCALE_MODE_CONTENT_SCALE = 0,
    /// One UI unit is one framebuffer pixel, whatever the display scale.
    STURDY_UI_SCALE_MODE_RAW_PIXELS = 1,
    STURDY_UI_SCALE_MODE_FORCE_U32 = 0x7fffffff
} SturdyUiScaleMode;

/// Chooses how UI units map onto the framebuffer. Takes effect at the next `sturdy_ui_begin`.
///
/// @return `STURDY_ERROR_INVALID_ARGUMENT` for an unknown mode.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ui_set_scale_mode(SturdyEngine engine, SturdyUiScaleMode mode);

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

/// Reads the pointer position in UI coordinates: the same units as element sizes and positions,
/// which are framebuffer pixels divided by the content scale unless
/// `STURDY_UI_SCALE_MODE_RAW_PIXELS` is set.
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

/// Screen-space hint for where an IME should draw its candidate/composition window, in window
/// coordinates (the same units as `SturdyWindowSnapshot::width`/`height` and mouse positions). `cursor_offset_x` is the caret's
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
/// `width`/`height` are window coordinates (the space mouse positions and `position_x/y` use) and
/// `framebuffer_width/height` are physical pixels. They are equal on Windows/X11 and differ on
/// macOS, scaled Wayland and the Web. Render with the framebuffer values. The display's UI scale is
/// separate; read it with `sturdy_window_content_scale`.
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

/// Reads the UI scale the OS asks for on the display the window is on (1.0 at 100%, 2.0 at 200%).
/// Updated when the window moves to a display with a different scale.
///
/// @param surface Surface identifier, as handed to `request_render_frame`.
/// @return `STURDY_ERROR_NOT_AVAILABLE` when no managed window matches.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_window_content_scale(SturdyEngine engine,
                                                                    SturdySurface surface,
                                                                    float *out_scale);

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
    /// Client-area size in physical pixels on every platform.
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

/// Hints the IME where to draw its candidate/composition window, in window coordinates.
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

/// The "FPS Limit" dial: how the render loop caps its own dispatch rate, independent of and
/// composing with whatever the resolved present mode is already doing. Matches
/// `Core::FrameRateLimitMode`.
typedef enum SturdyFrameRateLimitMode {
    /// No engine-imposed cap. Correct with a vsync-blocking present mode (Fifo/FifoLatestReady);
    /// with a non-blocking one (Mailbox/Immediate/FifoRelaxed) this really is uncapped dispatch,
    /// an explicit choice rather than something the engine silently overrides.
    STURDY_FRAME_RATE_LIMIT_MODE_UNLIMITED = 0,
    /// Cap dispatch to `SturdyPresentationSettings::frame_rate_limit_fps`, regardless of present mode.
    STURDY_FRAME_RATE_LIMIT_MODE_CUSTOM,
    /// Cap dispatch to the display's current refresh rate (queried per frame; a window can move between
    /// displays with different rates). Falls back to unlimited if the platform/window can't report one.
    STURDY_FRAME_RATE_LIMIT_MODE_MATCH_DISPLAY_REFRESH,
    STURDY_FRAME_RATE_LIMIT_MODE_FORCE_U32 = 0x7fffffff
} SturdyFrameRateLimitMode;

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
    SturdyFrameRateLimitMode frame_rate_limit_mode;
    /// Target frames per second; meaningful only when `frame_rate_limit_mode ==
    /// STURDY_FRAME_RATE_LIMIT_MODE_CUSTOM`. A value `<= 0` behaves the same as
    /// `STURDY_FRAME_RATE_LIMIT_MODE_UNLIMITED`.
    double frame_rate_limit_fps;
    /// Headroom kept below the display's refresh rate while `variable_refresh` is engaged, in fps.
    /// Applied automatically whenever the display's current refresh rate can be queried, on top of
    /// (never loosening) whatever `frame_rate_limit_mode` otherwise picked -- including
    /// `STURDY_FRAME_RATE_LIMIT_MODE_UNLIMITED`, so enabling variable refresh alone is enough to get a
    /// safe cap. Defaults to 3.0 in `sturdy_presentation_settings_init`.
    double variable_refresh_margin_fps;
    /// A separate, usually lower cap applied while the window is not focused, to save power/heat.
    /// `<= 0` disables this. Only ever tightens an existing cap, never loosens one.
    double unfocused_frame_rate_limit_fps;
    /// On a fixed-refresh display, round a frame-rate limit below the refresh rate down to the nearest
    /// `refresh / n` so every frame is on screen for the same number of refreshes (144 Hz with a 60 fps
    /// limit -> 48 fps). Ignored under variable refresh. Defaults to true.
    SturdyBool snap_frame_rate_limit_to_refresh;
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

/// The engine's raw WebGPU objects.
///
/// Fields are `void *` for the same reason as `SturdyVulkanHandles`/`SturdyD3D12Handles`, and carry
/// the same borrowing rules. Unlike those two, this deliberately stops at the WebGPU objects
/// themselves rather than reaching beneath them to whatever native API Dawn chose (Vulkan, Metal, or
/// D3D12) -- that is fundamentally unavailable on the Web target (a browser's WebGPU implementation
/// exposes no native handles to any embedder) and would defeat the point of the backend even on
/// native Dawn. Cast each field to its documented `webgpu.h` type. There is exactly one queue:
/// WebGPU has no separate graphics/compute/transfer queues to distinguish, so there is no
/// `sturdy_native_webgpu_queue` counterpart to `sturdy_native_vulkan_queue`.
typedef struct SturdyWebGpuHandles {
    /// Set by the engine to `sizeof(SturdyWebGpuHandles)` as this build sees it.
    uint32_t struct_size;
    uint32_t reserved;
    /// `WGPUInstance`.
    void *instance;
    /// `WGPUAdapter`.
    void *adapter;
    /// `WGPUDevice`.
    void *device;
    /// `WGPUQueue`, the one queue every WebGPU device exposes.
    void *queue;
} SturdyWebGpuHandles;

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

/// Reads the engine's raw WebGPU objects.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when the active backend is not WebGPU, or when native
///         access was not enabled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_native_webgpu(SturdyEngine engine,
                                                             SturdyWebGpuHandles *out_handles);

// ---------------------------------------------------------------------------------------------
// Reflection
// ---------------------------------------------------------------------------------------------
//
// Runtime introspection over C++ types reflected via SFT_REFLECT_TYPE/FIELD/METHOD/EVENT — the
// actual modding surface: a foreign-language mod finds a type by name, reads/writes its fields,
// invokes its methods, and installs overrides/hooks/event listeners entirely through this
// section, without ever needing to know the type's real C++ definition.
//
// Independent of any running engine: `SFT::Reflection::TypeRegistry` is a process-wide singleton,
// not something owned by a `SturdyEngine` session, so every function below may be called before
// `sturdy_runtime_run`, after it returns, or concurrently with it, from any thread — the same
// shape as the logging section above. IDs are correspondingly **not** scope-bound handles like
// `SturdyEngine`/`SturdyFrame`: once a type/field/method/event is registered, its id is stable for
// the process lifetime (or until explicitly unregistered) and there is nothing to "expire".
//
// **Object pointers are the caller's responsibility.** `sturdy_reflection_get_field` and friends
// take a raw `void *object` — this ABI has no entity/handle system for arbitrary reflected C++
// objects (unlike ECS components, which live in archetype storage `sturdy_ecs_get_component`
// copies out of). A mod is expected to reach an object through some other part of this ABI that
// legitimately hands out a pointer (or to own the memory itself, for a type it registered via
// `sturdy_reflection_type_builder_*`). Passing a dangling or wrongly-typed pointer is exactly as
// unsafe here as it would be in C++ — this boundary cannot detect it.
//
// **A macro-reflected C++ type must already be registered before `sturdy_reflection_find_type`
// can see it.** `SFT_REFLECT_TYPE` only specializes a C++ template at compile time — by design,
// it performs no registration of its own (see the type's own header for why). Registration
// happens lazily, the first time C++ code calls `TypeRegistry::instance().type<T>()`/
// `try_register<T>()` — and *only* C++ code can do that, since it is a template requiring
// compile-time knowledge of `T` that a foreign caller fundamentally does not have. In practice
// this means: the game/engine's own C++ startup code must explicitly "touch" every type it wants
// moddable at least once (however briefly) before any mod can find it through this section — a
// type nobody ever asked about in C++ is invisible here, not because of a permissions boundary,
// but because it was never registered. Types built entirely at runtime through
// `sturdy_reflection_type_builder_*` have no such requirement — `_finish` registers them itself.

/// Identifies a reflected type, field, method, event, or enum. Derived from its canonical name
/// and stable for the process lifetime (or until unregistered) — the same shape as
/// `SturdyResourceId`, since both wrap the engine's underlying 128-bit name hash identically.
typedef struct SturdyReflectionId {
    uint64_t high;
    uint64_t low;
} SturdyReflectionId;

/// A reflected id known to be invalid — `{0, 0}` can never be produced by hashing a real name.
/// Useful for initializing an `out_*` parameter before a call that might fail.
///
/// @note Deliberately spelled as a bare brace-init-list, not a C99 compound-literal cast: this
///       form is valid as a declaration's initializer (`SturdyReflectionId id =
///       STURDY_REFLECTION_ID_NONE;`) in both C99 and C++, whereas a compound literal
///       (`(SturdyReflectionId){0, 0}`) is C-only and would not compile in a C++ translation
///       unit — this header must work as both. It is *not* valid as a bare function argument in
///       C (only in C++); pass a named variable instead if you need that.
#define STURDY_REFLECTION_ID_NONE {0, 0}

/// What the engine knows about a reflected field.
typedef struct SturdyReflectionFieldInfo {
    /// Set by the engine to `sizeof(SturdyReflectionFieldInfo)` as this build sees it.
    uint32_t struct_size;
    SturdyReflectionId field;
    /// Identifies the field's declared type. If this also resolves via
    /// `sturdy_reflection_find_type`/`_find_enum`, the field is a nested reflected struct/enum;
    /// otherwise it is a primitive or an opaque type this ABI does not further describe.
    SturdyReflectionId field_type;
    uint32_t size;
    uint32_t align;
    /// True for a static data member — access it with `sturdy_reflection_get_static_field`/
    /// `_set_static_field` instead of the instance versions.
    SturdyBool is_static;
    /// True when the field rejects writes (`sturdy_reflection_set_field` returns
    /// `STURDY_ERROR_INVALID_ARGUMENT`).
    SturdyBool is_read_only;
    /// True when the field is a recognized container (`std::vector<T>` with a trivially-copyable
    /// `T`, in the current engine) — see the container section below.
    SturdyBool is_container;
    uint8_t reserved;
} SturdyReflectionFieldInfo;

/// What the engine knows about a reflected method.
typedef struct SturdyReflectionMethodInfo {
    /// Set by the engine to `sizeof(SturdyReflectionMethodInfo)` as this build sees it.
    uint32_t struct_size;
    SturdyReflectionId method;
    SturdyReflectionId return_type;
    uint32_t param_count;
    /// True for a static member function — call it with `sturdy_reflection_invoke_static_method`
    /// instead of the instance version.
    SturdyBool is_static;
    uint8_t reserved[3];
} SturdyReflectionMethodInfo;

/// A type-erased method call/override/hook body: `object` is the receiver (null for a static
/// method), `args` is one already-typed, caller-marshalled pointer per parameter (see
/// `SturdyReflectionMethodInfo::param_count`), `out_return` is uninitialized storage for the
/// return value (ignored when the method returns nothing), `user_data` is whatever was passed to
/// `sturdy_reflection_register_override`.
typedef void(STURDY_ABI_CALL *SturdyReflectionMethodFn)(void *object,
                                                         const void *const *args,
                                                         void *out_return,
                                                         void *user_data);

/// A type-erased *observer* body — used for both a method before/after hook and an event
/// listener, which share this exact shape (unlike `SturdyReflectionMethodFn`, there is no
/// `out_return`: neither a hook nor an event listener produces a value, only observes `args`).
/// `object` is the method's receiver (null for a static method) or whatever
/// `sturdy_reflection_fire_event` was called with; `args` is one already-typed pointer per
/// parameter; `user_data` is whatever was passed to `_add_before_hook`/`_add_after_hook`/
/// `_subscribe_event`.
typedef void(STURDY_ABI_CALL *SturdyReflectionObserverFn)(void *object,
                                                           const void *const *args,
                                                           void *user_data);

/// Handle to an installed before-hook/after-hook/event-listener subscription, for later removal
/// with the matching `_remove_*`/`_unsubscribe_event` call. Not scope-bound — stays valid until
/// explicitly removed or the type it was installed on is unregistered.
typedef struct SturdyReflectionSubscription {
    uint64_t id;
} SturdyReflectionSubscription;

// ---- Type lookup ----

/// Looks up a reflected type by canonical name.
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when nothing is registered under that name.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_find_type(const char *name, SturdyReflectionId *out_type);

/// Reads a type's canonical name. See the string-output convention above.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_name(SturdyReflectionId type,
                                                                    char *buffer,
                                                                    size_t capacity,
                                                                    size_t *out_length);

/// Reads a type's instance size/alignment — what `object` must point at `size` bytes of, aligned
/// to `align`, for every instance-field/method call below.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_size(SturdyReflectionId type,
                                                                    uint32_t *out_size,
                                                                    uint32_t *out_align);

/// Unregisters a type, so no future `find_type`/field/method/event lookup can reach it — the
/// mod-unload primitive. See `TypeRegistry::unregister_type`'s C++ doc comment for the full
/// quiescence requirement: stop dispatching to this type (no calls in flight against it on any
/// thread) *before* calling this, and only unload any shared library it came from after.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_unregister_type(SturdyReflectionId type);

// ---- Field/method/event lookup ----

/// Looks up a field by name, walking `type`'s reflected base chain if not declared directly on it
/// (the same behavior as `TypeRegistry::find_field`).
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when no such field is declared on `type` or any ancestor.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_find_field(SturdyReflectionId type,
                                                                     const char *name,
                                                                     SturdyReflectionId *out_field);

/// Reads what the engine knows about a field.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_field_info(SturdyReflectionId type,
                                                                     SturdyReflectionId field,
                                                                     SturdyReflectionFieldInfo *out_info);

/// Looks up a method by name, walking `type`'s reflected base chain. See `sturdy_reflection_find_field`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_find_method(SturdyReflectionId type,
                                                                      const char *name,
                                                                      SturdyReflectionId *out_method);

/// Reads what the engine knows about a method.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_method_info(SturdyReflectionId type,
                                                                      SturdyReflectionId method,
                                                                      SturdyReflectionMethodInfo *out_info);

/// Looks up a declared hook point by name, walking `type`'s reflected base chain. See
/// `sturdy_reflection_find_field`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_find_event(SturdyReflectionId type,
                                                                     const char *name,
                                                                     SturdyReflectionId *out_event);

// ---- Field access ----

/// Copies a field's current value out of `object` into `out_data`.
///
/// @param size Must equal the field's registered size (`SturdyReflectionFieldInfo::size`) — a
///        mismatch is rejected rather than partially copied.
/// @return `STURDY_ERROR_INVALID_ARGUMENT` on a size mismatch or a field with no readable
///         representation (an opaque non-`Trivial`, non-container field this ABI cannot copy).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_get_field(SturdyReflectionId type,
                                                                    void *object,
                                                                    SturdyReflectionId field,
                                                                    void *out_data,
                                                                    uint32_t size);

/// Overwrites a field's value in place on `object`. See `sturdy_reflection_get_field`.
///
/// @return `STURDY_ERROR_INVALID_ARGUMENT` on a size mismatch, a `ReadOnly` field, or a field with
///         no writable representation.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_set_field(SturdyReflectionId type,
                                                                    void *object,
                                                                    SturdyReflectionId field,
                                                                    const void *data,
                                                                    uint32_t size);

/// Copies a static field's current value out. There is no per-instance object — the field has one
/// true address, tracked internally.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_get_static_field(SturdyReflectionId type,
                                                                           SturdyReflectionId field,
                                                                           void *out_data,
                                                                           uint32_t size);

/// Overwrites a static field's value. See `sturdy_reflection_get_static_field`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_set_static_field(SturdyReflectionId type,
                                                                           SturdyReflectionId field,
                                                                           const void *data,
                                                                           uint32_t size);

// ---- Container access ----
//
// For a field where SturdyReflectionFieldInfo::is_container is true (currently: std::vector<T>
// with a trivially-copyable T — see Reflection/ContainerInfo.hpp's doc comment for why other
// shapes are not yet supported). `object` is the same field-owning instance
// sturdy_reflection_get_field/_set_field would take; element access goes through the field, not
// through a separate container handle.

/// Returns a container field's current element count.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_container_size(SturdyReflectionId type,
                                                                         void *object,
                                                                         SturdyReflectionId field,
                                                                         uint32_t *out_count);

/// Reads the element at `index` out of a container field.
///
/// @param element_size Must equal the container's declared element size.
/// @return `STURDY_ERROR_OUT_OF_RANGE` when `index >= sturdy_reflection_container_size(...)`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_container_get_element(SturdyReflectionId type,
                                                                                void *object,
                                                                                SturdyReflectionId field,
                                                                                uint32_t index,
                                                                                void *out_data,
                                                                                uint32_t element_size);

/// Writes `data` into the element at `index` of a container field. See
/// `sturdy_reflection_container_get_element`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_container_set_element(SturdyReflectionId type,
                                                                                void *object,
                                                                                SturdyReflectionId field,
                                                                                uint32_t index,
                                                                                const void *data,
                                                                                uint32_t element_size);

/// Resizes a container field to exactly `new_size` elements (default-constructing new ones,
/// destroying truncated ones).
///
/// @return `STURDY_ERROR_NOT_AVAILABLE` when the element type has no default constructor.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_container_resize(SturdyReflectionId type,
                                                                           void *object,
                                                                           SturdyReflectionId field,
                                                                           uint32_t new_size);

// ---- Method invocation ----

/// Calls an instance method on `object`, dispatching to a mod-installed override
/// (`sturdy_reflection_register_override`) instead of the real implementation when one is
/// present, and firing any before/after hooks around it either way.
///
/// @param args One already-typed pointer per parameter, in declared order
///        (`SturdyReflectionMethodInfo::param_count` entries). Pass null when `param_count` is 0.
/// @param out_return Receives the return value (`out_return_size` bytes), ignored when the method
///        returns nothing. May be null if you do not need it.
/// @return `STURDY_ERROR_INVALID_ARGUMENT` when `arg_count` does not match `param_count`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_invoke_method(SturdyReflectionId type,
                                                                        void *object,
                                                                        SturdyReflectionId method,
                                                                        const void *const *args,
                                                                        uint32_t arg_count,
                                                                        void *out_return,
                                                                        uint32_t out_return_size);

/// Calls a static method. See `sturdy_reflection_invoke_method`; there is no receiver.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_invoke_static_method(SturdyReflectionId type,
                                                                               SturdyReflectionId method,
                                                                               const void *const *args,
                                                                               uint32_t arg_count,
                                                                               void *out_return,
                                                                               uint32_t out_return_size);

// ---- Overrides and hooks ----

/// Installs a full replacement for a method — `sturdy_reflection_invoke_method`/
/// `_invoke_static_method` call `override_fn` instead of the real implementation from this point
/// on. Only one override may be active per method; installing a second silently replaces the
/// first (there is no chain — a mod that wants "wrap, not replace" should call through the
/// before/after hooks instead, or capture and call the previous override itself before
/// installing its own).
///
/// @param override_fn Must not be null. Pass a null-checking no-op and call
///        `sturdy_reflection_clear_override` instead of passing null here to remove an override.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_register_override(SturdyReflectionId type,
                                                                            SturdyReflectionId method,
                                                                            SturdyReflectionMethodFn override_fn,
                                                                            void *user_data);

/// Removes a previously installed override, restoring the real implementation.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_clear_override(SturdyReflectionId type,
                                                                         SturdyReflectionId method);

/// Adds a listener that fires just before a method runs (with the same `args` the real call
/// receives), in addition to — not instead of — the real implementation/override. Unlike
/// `sturdy_reflection_register_override`, any number of mods may add hooks without clobbering
/// each other's.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_add_before_hook(SturdyReflectionId type,
                                                                          SturdyReflectionId method,
                                                                          SturdyReflectionObserverFn hook,
                                                                          void *user_data,
                                                                          SturdyReflectionSubscription *out_subscription);

/// Removes a hook added by `sturdy_reflection_add_before_hook`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_remove_before_hook(SturdyReflectionId type,
                                                                             SturdyReflectionId method,
                                                                             SturdyReflectionSubscription subscription);

/// Adds a listener that fires just after a method returns. See `sturdy_reflection_add_before_hook`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_add_after_hook(SturdyReflectionId type,
                                                                         SturdyReflectionId method,
                                                                         SturdyReflectionObserverFn hook,
                                                                         void *user_data,
                                                                         SturdyReflectionSubscription *out_subscription);

/// Removes a hook added by `sturdy_reflection_add_after_hook`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_remove_after_hook(SturdyReflectionId type,
                                                                            SturdyReflectionId method,
                                                                            SturdyReflectionSubscription subscription);

// ---- Events ----

/// Subscribes a listener to a declared hook point (`SFT_REFLECT_EVENT`). Any number of mods may
/// subscribe to the same event independently.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_subscribe_event(SturdyReflectionId type,
                                                                          SturdyReflectionId event,
                                                                          SturdyReflectionObserverFn listener,
                                                                          void *user_data,
                                                                          SturdyReflectionSubscription *out_subscription);

/// Removes a listener added by `sturdy_reflection_subscribe_event`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_unsubscribe_event(SturdyReflectionId type,
                                                                            SturdyReflectionId event,
                                                                            SturdyReflectionSubscription subscription);

/// Fires an event, calling every subscribed listener in registration order. A no-op — cheap, no
/// error — when nobody has subscribed.
///
/// @param args One already-typed pointer per event parameter. Pass null when the event takes none.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_fire_event(SturdyReflectionId type,
                                                                     void *object,
                                                                     SturdyReflectionId event,
                                                                     const void *const *args,
                                                                     uint32_t arg_count);

// ---- Dynamic type registration ----
//
// The mods-can-have-mods primitive: a foreign-language mod with no C++ type behind it at all (a
// scripted mod adding an item type, say) describes fields/methods by hand — offsets into a
// buffer it allocates and owns, plus type-erased accessor callbacks — instead of reflecting an
// existing struct. Once finished, the resulting type is indistinguishable from a macro-reflected
// one to every other mod: findable by name, readable/writable, invocable, hookable. Mirrors
// `SFT::Reflection::TypeInfoBuilder` exactly; see its C++ doc comment for the full rationale.

/// Handle to an in-progress type description. Owned by the caller — build it up with the
/// `_add_*` calls below, then either `_finish` it (consuming the builder, registering the type)
/// or `_discard` it (consuming the builder without registering anything).
typedef struct SturdyReflectionTypeBuilder {
    uint64_t token;
} SturdyReflectionTypeBuilder;

/// Same shape as `SturdyReflectionMethodFn`'s two data-access halves — a non-`Trivial` field's
/// copy-out implementation, called with `object` = the field-owning instance.
typedef void(STURDY_ABI_CALL *SturdyReflectionFieldGetFn)(const void *object, void *out_value);
/// Copy-in counterpart to `SturdyReflectionFieldGetFn`.
typedef void(STURDY_ABI_CALL *SturdyReflectionFieldSetFn)(void *object, const void *in_value);

/// Starts building a type descriptor.
///
/// @param canonical_name Stable name other code will look this type up by. Must not collide with
///        an already-registered name.
/// @param size Size in bytes of one instance.
/// @param align Required alignment of one instance.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_create(const char *canonical_name,
                                                                              uint32_t size,
                                                                              uint32_t align,
                                                                              SturdyReflectionTypeBuilder *out_builder);

/// Sets the construct/destroy callbacks for the type being built.
///
/// @param move_construct Must not be null — every registered type needs a working move
///        constructor (used internally by the registry's own bookkeeping, independently of
///        whether your mod ever calls a "construct/destroy an instance" entry point itself).
/// @param destroy Must not be null — every registered type needs a working destructor.
/// @param default_construct May be null when the type does not support default construction.
/// @param copy_construct May be null when the type does not support copy construction.
typedef void(STURDY_ABI_CALL *SturdyReflectionMoveConstructFn)(void *destination, void *source, void *user_data);
typedef void(STURDY_ABI_CALL *SturdyReflectionDestroyFn)(void *object, void *user_data);
typedef void(STURDY_ABI_CALL *SturdyReflectionDefaultConstructFn)(void *destination, void *user_data);
typedef void(STURDY_ABI_CALL *SturdyReflectionCopyConstructFn)(void *destination, const void *source, void *user_data);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_set_constructors(SturdyReflectionTypeBuilder builder,
                                                                                        SturdyReflectionMoveConstructFn move_construct,
                                                                                        SturdyReflectionDestroyFn destroy,
                                                                                        SturdyReflectionDefaultConstructFn default_construct,
                                                                                        SturdyReflectionCopyConstructFn copy_construct,
                                                                                        void *user_data);

/// Declares one field on the type being built.
///
/// @param is_trivial True when raw offset+memcpy access is safe (`copy_get`/`copy_set` are then
///        ignored and may be null); false requires both callbacks.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_field(SturdyReflectionTypeBuilder builder,
                                                                                 const char *name,
                                                                                 uint32_t offset,
                                                                                 uint32_t size,
                                                                                 uint32_t align,
                                                                                 SturdyReflectionId field_type,
                                                                                 SturdyBool is_trivial,
                                                                                 SturdyReflectionFieldGetFn copy_get,
                                                                                 SturdyReflectionFieldSetFn copy_set);

/// Declares one method on the type being built.
///
/// @param param_types One id per parameter, in call order. May be null when `param_count` is 0.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_method(SturdyReflectionTypeBuilder builder,
                                                                                  const char *name,
                                                                                  SturdyReflectionId return_type,
                                                                                  const SturdyReflectionId *param_types,
                                                                                  uint32_t param_count,
                                                                                  SturdyReflectionMethodFn invoke);

/// Declares one hook point on the type being built. See `SFT_REFLECT_EVENT`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_add_event(SturdyReflectionTypeBuilder builder,
                                                                                 const char *name,
                                                                                 const SturdyReflectionId *param_types,
                                                                                 uint32_t param_count);

/// Finishes building and registers the type, consuming `builder`.
///
/// @return `STURDY_ERROR_INVALID_ARGUMENT` when the descriptor is incomplete (no `destroy`
///         callback set) or `canonical_name` collides with a different, already-registered type.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_finish(SturdyReflectionTypeBuilder builder,
                                                                              SturdyReflectionId *out_type);

/// Discards `builder` without registering anything. Safe to call on an already-finished or
/// already-discarded builder (a no-op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_reflection_type_builder_discard(SturdyReflectionTypeBuilder builder);

// ---------------------------------------------------------------------------------------------
// Networking, files, processes and data parallelism (the engine's Tokio + Rayon)
// ---------------------------------------------------------------------------------------------
//
// Everything that waits returns an **operation** (`SturdyAsyncOp`): poll it, wait on it with a timeout, cancel it, or attach a
// completion callback. A finished operation holds a result — a status, an error, and some of {bytes, text, a number, named fields, a new
// object} whose meaning each function documents. Long-lived things (a connection, a server, a file) are **objects** (`SturdyAsyncObject`)
// that you release when done; releasing closes them. One model for every protocol, in every language.
//
// - Both handle kinds are owned by you and live until released, like `SturdyTask`. Tokens are never reused.
// - Completion callbacks run on a network or timer thread, **not** on the engine's frame thread: keep them short, never wait on an
//   operation inside one, and read results with the `sturdy_op_*` getters before returning.
// - Do not wait on an operation from a scheduler worker that the operation might depend on. `sturdy_op_wait` blocks the caller.
// - Option structs: zero-initialize, then set what you need. A null pointer means "all defaults". Zero in a numeric field means "the
//   default" where one is documented; booleans are named so that zero is the safe default (`no_http2`, not `http2`).
// - Strings you pass are copied. Strings and arrays the engine hands you in a callback are valid only during that callback.
// - Security defaults match the C++ API: TLS verifies the chain and host name unless you pin a certificate; there is no switch that
//   disables verification. `identify_peer_by_certificate_only` and `accept_any_client_certificate` are the two loud opt-ins for
//   peer-to-peer identity by key, and then **you** must check the peer's fingerprint.

/// A pending or finished operation. Owned until `sturdy_op_release`.
typedef struct SturdyAsyncOp {
    uint64_t token;
} SturdyAsyncOp;

/// A long-lived object (connection, listener, server, file, ...). Owned until `sturdy_object_release`, which closes it.
typedef struct SturdyAsyncObject {
    uint64_t token;
} SturdyAsyncObject;

/// Where an operation stands.
typedef enum SturdyOpStatus {
    STURDY_OP_PENDING = 0,
    /// Finished with a result.
    STURDY_OP_OK = 1,
    /// Finished with an error: see `sturdy_op_error_code` / `sturdy_op_error_message`.
    STURDY_OP_FAILED = 2,
    /// Stopped by `sturdy_op_cancel` (or by releasing what it was waiting on) before it finished.
    STURDY_OP_CANCELLED = 3,
    STURDY_OP_STATUS_FORCE_U32 = 0x7fffffff
} SturdyOpStatus;

/// Why an operation failed.
typedef enum SturdyIoError {
    STURDY_IO_NONE = 0,
    STURDY_IO_NOT_FOUND = 1,
    STURDY_IO_PERMISSION_DENIED = 2,
    STURDY_IO_ALREADY_EXISTS = 3,
    STURDY_IO_INVALID_ARGUMENT = 4,
    STURDY_IO_CONNECTION_REFUSED = 5,
    STURDY_IO_CONNECTION_RESET = 6,
    STURDY_IO_TIMED_OUT = 7,
    STURDY_IO_UNSUPPORTED = 8,
    STURDY_IO_UNKNOWN = 9,
    /// The peer closed the stream before a fixed-size read was satisfied.
    STURDY_IO_UNEXPECTED_EOF = 10,
    /// The local object was closed before or during the operation.
    STURDY_IO_CLOSED = 11,
    STURDY_IO_TLS_FAILURE = 12,
    /// A configured size or count limit was exceeded.
    STURDY_IO_LIMIT_EXCEEDED = 13,
    /// The peer violated the protocol.
    STURDY_IO_PROTOCOL_ERROR = 14,
    STURDY_IO_ERROR_FORCE_U32 = 0x7fffffff
} SturdyIoError;

/// A name/value pair (HTTP headers, environment variables). Both strings are null-terminated.
typedef struct SturdyKeyValue {
    const char *name;
    const char *value;
} SturdyKeyValue;

/// Optional parts of the build / machine, as a bit set from `sturdy_async_features`.
typedef enum SturdyAsyncFeature {
    STURDY_ASYNC_FEATURE_QUIC = 1,
    STURDY_ASYNC_FEATURE_HTTP2 = 2,
    STURDY_ASYNC_FEATURE_HTTP3 = 4,
    STURDY_ASYNC_FEATURE_IO_URING = 8,
    STURDY_ASYNC_FEATURE_PROCESS = 16,
    STURDY_ASYNC_FEATURE_GZIP = 32,
    STURDY_ASYNC_FEATURE_NATIVE_FILE_WATCH = 64,
    STURDY_ASYNC_FEATURE_FORCE_U32 = 0x7fffffff
} SturdyAsyncFeature;

/// Called when an operation finishes. Runs on a network, timer or worker thread. The op is still owned by you; read its result here or later.
/// Called exactly once for every registration — also when the op was released or cancelled meanwhile, so the `user_data` you attached can always
/// be freed here. In that case `op` is no longer a valid handle (its getters report an expired handle); check with `sturdy_op_status` first.
typedef void(STURDY_ABI_CALL *SturdyAsyncOpFn)(void *user_data, SturdyAsyncOp op);

// ---- operations --------------------------------------------------------------------------------

/// Bit set of `SturdyAsyncFeature`: which optional protocols this build contains and whether this machine supports them.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_features(uint32_t *out_features);

/// Reads an operation's status without blocking.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_status(SturdyAsyncOp op, SturdyOpStatus *out_status);

/// Waits until the operation finishes or `timeout_ms` passes (negative: no limit). `out_done` says which.
/// Blocks the calling thread; do not call it from a completion callback.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_wait(SturdyAsyncOp op, int64_t timeout_ms, SturdyBool *out_done);

/// Waits for the first of `ops` to finish. `out_index` is its position, or -1 when `timeout_ms` passed first.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_wait_any(const SturdyAsyncOp *ops, uint32_t count, int64_t timeout_ms, int32_t *out_index);

/// Asks the operation to stop. It then finishes as `STURDY_OP_CANCELLED` (or with its result, if it already had one). Harmless on a finished op.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_cancel(SturdyAsyncOp op);

/// Runs `callback` when the operation finishes — at once, on this thread, if it already has.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_on_complete(SturdyAsyncOp op, SturdyAsyncOpFn callback, void *user_data);

/// The failure kind of a `STURDY_OP_FAILED` op (`STURDY_IO_NONE` otherwise). `STURDY_ERROR_BUSY` while pending.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_error_code(SturdyAsyncOp op, SturdyIoError *out_code);

/// The failure message of a failed op (empty otherwise), following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_error_message(SturdyAsyncOp op, char *buffer, size_t capacity, size_t *out_length);

/// The result's number (byte count, status code, port, ... — see the function that made the op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_number(SturdyAsyncOp op, int64_t *out_number);

/// The result's text, following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_text(SturdyAsyncOp op, char *buffer, size_t capacity, size_t *out_length);

/// The result's bytes. Pass a null buffer to learn the size; `out_length` always receives the full size, and
/// `STURDY_ERROR_BUFFER_TOO_SMALL` is returned (after copying what fits) when the buffer was smaller. No terminator is added.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_bytes(SturdyAsyncOp op, uint8_t *buffer, size_t capacity, size_t *out_length);

/// The object the operation produced (a connection, a file, ...), as a new handle you own. Calling it again returns the same handle.
/// `STURDY_ERROR_NOT_AVAILABLE` when the op produced none. The object lives until **both** its handle and the operation are released, so a
/// "permit" or a "mutex_guard" is only dropped once the op is released too — or release it explicitly with its own function.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_object(SturdyAsyncOp op, SturdyAsyncObject *out_object);

/// How many named fields the result has.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_field_count(SturdyAsyncOp op, uint32_t *out_count);

/// Name of field `index`, following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_field_name(SturdyAsyncOp op, uint32_t index, char *buffer, size_t capacity, size_t *out_length);

/// Value of field `index`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_field_value(SturdyAsyncOp op, uint32_t index, char *buffer, size_t capacity, size_t *out_length);

/// Value of the first field called `name` (HTTP header lookup is case-insensitive; other fields are exact).
/// `STURDY_ERROR_NOT_AVAILABLE` when there is none.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_field(SturdyAsyncOp op, const char *name, char *buffer, size_t capacity, size_t *out_length);

/// Releases an operation. A pending one keeps running; its result is dropped.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_release(SturdyAsyncOp op);

/// Releases an object, closing whatever it wraps. Operations still pending on it finish as closed or cancelled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_object_release(SturdyAsyncObject object);

/// The object's kind ("tcp", "websocket", "quic_stream", ...), following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_object_kind(SturdyAsyncObject object, char *buffer, size_t capacity, size_t *out_length);

// ---- timers, signals, DNS, STUN, identities ---------------------------------------------------

/// Finishes (OK) after `milliseconds`, without occupying a thread.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_timer_sleep(int64_t milliseconds, SturdyAsyncOp *out_op);

/// Finishes when the process receives SIGINT or SIGTERM (Ctrl+C / termination). Number: the signal.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_signal_shutdown(SturdyAsyncOp *out_op);

/// Resolves a host name. Fields: one `address` per result (numeric text).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dns_resolve(const char *host, SturdyAsyncOp *out_op);

/// Asks a STUN server what public address this machine appears as. Text: host. Number: port. `server` null: stun.l.google.com:19302.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stun_query(const char *server, uint16_t port, int64_t timeout_ms, SturdyAsyncOp *out_op);

/// Generates a self-signed ECDSA P-256 identity (for peers and dev servers). Returns an already-finished op with fields
/// `certificate_pem`, `private_key_pem`, `sha256`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tls_generate_identity(const char *common_name, uint32_t valid_days, SturdyAsyncOp *out_op);

/// Hex SHA-256 of the first certificate in `certificate_pem`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tls_certificate_sha256(const char *certificate_pem, char *buffer, size_t capacity, size_t *out_length);

/// How to verify a server (client side of TLS/QUIC/HTTPS/WSS). Chain and host name are always checked unless `pinned_sha256` names the
/// exact certificate(s) accepted.
typedef struct SturdyTlsClientOptions {
    /// Name sent as SNI and checked against the certificate; null: the host being connected to.
    const char *server_name;
    /// Do not trust the operating system's roots (use `ca_pem` only).
    SturdyBool no_system_roots;
    /// Extra trusted CA certificates, PEM (may hold several). May be null.
    const char *ca_pem;
    /// Hex SHA-256 of acceptable leaf certificates; when present they replace chain validation.
    const char *const *pinned_sha256;
    uint32_t pinned_sha256_count;
    /// Peer-to-peer opt-in: accept any certificate and let **you** compare `peer_sha256`. Never default.
    SturdyBool identify_peer_by_certificate_only;
    /// Client certificate for mutual TLS (both or neither).
    const char *client_certificate_pem;
    const char *client_private_key_pem;
    /// ALPN protocols to offer, most preferred first.
    const char *const *alpn;
    uint32_t alpn_count;
    /// Time allowed for the TLS handshake; 0: 10 s.
    uint32_t handshake_timeout_ms;
} SturdyTlsClientOptions;

/// How to present and check identities on the accepting side.
typedef struct SturdyTlsServerOptions {
    const char *certificate_pem;
    const char *private_key_pem;
    /// CAs whose client certificates are accepted (mutual TLS). May be null.
    const char *client_ca_pem;
    SturdyBool require_client_certificate;
    /// Peer-to-peer opt-in: accept any self-signed client certificate; authorise by `peer_sha256` yourself.
    SturdyBool accept_any_client_certificate;
    const char *const *pinned_client_sha256;
    uint32_t pinned_client_sha256_count;
    const char *const *alpn;
    uint32_t alpn_count;
    /// Time allowed for each client's TLS handshake; 0: 10 s.
    uint32_t handshake_timeout_ms;
} SturdyTlsServerOptions;

// ---- TCP and TLS streams ------------------------------------------------------------------------

/// Connects (resolving, trying each address). Object "tcp". `tls` null: plain TCP; otherwise a verified TLS handshake.
/// `timeout_ms` 0: 30 seconds.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_connect(const char *host, uint16_t port, const SturdyTlsClientOptions *tls, int64_t timeout_ms, SturdyAsyncOp *out_op);

/// Starts listening. Object "tcp_listener" (finished op). `tls` non-null: every accepted connection completes a TLS handshake first.
/// `backlog` is the pending-connection queue length; 0: 1024.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_listen(const char *host, uint16_t port, const SturdyTlsServerOptions *tls, int32_t backlog, SturdyAsyncOp *out_op);

/// Waits for the next connection. Object "tcp".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_accept(SturdyAsyncObject listener, SturdyAsyncOp *out_op);

/// The port a listener is bound to.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_port(SturdyAsyncObject listener, uint16_t *out_port);
/// "host:port" the listener is bound to, following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_address(SturdyAsyncObject listener, char *buffer, size_t capacity, size_t *out_length);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_is_open(SturdyAsyncObject listener, SturdyBool *out_open);

/// Sends all of `data`. Number: bytes sent. The data is copied.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_send(SturdyAsyncObject connection, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);

/// Receives 1..`max_bytes` bytes as soon as any arrive. Bytes: empty means the peer finished.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_receive(SturdyAsyncObject connection, size_t max_bytes, SturdyAsyncOp *out_op);

/// Receives exactly `count` bytes (`STURDY_IO_UNEXPECTED_EOF` if the peer closes first).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_receive_exact(SturdyAsyncObject connection, size_t count, SturdyAsyncOp *out_op);

/// Receives up to and including `delimiter` (a line, a header block); `STURDY_IO_LIMIT_EXCEEDED` past `max_bytes`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_receive_until(SturdyAsyncObject connection, const char *delimiter, size_t max_bytes, SturdyAsyncOp *out_op);

/// Half-close: tells the peer no more data is coming while reads continue.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_shutdown_write(SturdyAsyncObject connection, SturdyAsyncOp *out_op);

/// Upgrades an open, quiet connection to TLS in place (STARTTLS, after an HTTP CONNECT). Client or server role.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_start_tls_client(SturdyAsyncObject connection, const char *host, const SturdyTlsClientOptions *tls, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_start_tls_server(SturdyAsyncObject connection, const SturdyTlsServerOptions *tls, SturdyAsyncOp *out_op);

/// Closes now (the object is still released separately).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_close(SturdyAsyncObject connection);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_close(SturdyAsyncObject listener);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_is_open(SturdyAsyncObject connection, SturdyBool *out_open);
/// Socket options; each finishes once applied (a failure is reported on the op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_set_nodelay(SturdyAsyncObject connection, SturdyBool enabled, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_set_keepalive(SturdyAsyncObject connection, SturdyBool enabled, SturdyAsyncOp *out_op);

/// Connection details as a finished op: fields `peer`, `local` ("host:port") and, over TLS, `protocol`, `cipher`, `alpn`, `peer_sha256`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_tcp_info(SturdyAsyncObject connection, SturdyAsyncOp *out_op);

// ---- UDP ----------------------------------------------------------------------------------------

typedef struct SturdyUdpOptions {
    SturdyBool reuse_address;
    SturdyBool broadcast;
    /// Kernel socket buffer sizes in bytes; 0: the OS default.
    uint64_t receive_buffer;
    uint64_t send_buffer;
} SturdyUdpOptions;

/// Binds a UDP socket. Object "udp". `host` null/empty: all interfaces. `port` 0: any. `options` may be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_bind(const char *host, uint16_t port, const SturdyUdpOptions *options, SturdyAsyncOp *out_op);

/// Fixes the peer: `sturdy_udp_send` then needs no address and only that peer's datagrams are received.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_connect(SturdyAsyncObject socket, const char *host, uint16_t port, SturdyAsyncOp *out_op);

/// Sends one datagram on a connected socket. Number: bytes sent.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_send(SturdyAsyncObject socket, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);

/// Sends one datagram. Number: bytes sent.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_send_to(SturdyAsyncObject socket, const char *host, uint16_t port, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);

/// Receives the next datagram (at most `max_bytes`). Bytes: the datagram. Text: sender host. Number: sender port.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_receive(SturdyAsyncObject socket, size_t max_bytes, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_port(SturdyAsyncObject socket, uint16_t *out_port);
/// "host:port" the socket is bound to, following the string-output convention.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_address(SturdyAsyncObject socket, char *buffer, size_t capacity, size_t *out_length);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_set_broadcast(SturdyAsyncObject socket, SturdyBool enabled, SturdyAsyncOp *out_op);
/// `interface_address` null/empty: the default interface.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_join_multicast(SturdyAsyncObject socket, const char *group, const char *interface_address, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_leave_multicast(SturdyAsyncObject socket, const char *group, const char *interface_address, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_set_multicast_loopback(SturdyAsyncObject socket, SturdyBool enabled, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_set_multicast_ttl(SturdyAsyncObject socket, uint32_t hops, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_close(SturdyAsyncObject socket);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_udp_is_open(SturdyAsyncObject socket, SturdyBool *out_open);

// ---- HTTP client --------------------------------------------------------------------------------

typedef struct SturdyHttpClientOptions {
    /// Deadline for one request including redirects. 0: 30 s.
    int64_t timeout_ms;
    /// Redirects followed; -1: the default (5); 0: return the redirect itself.
    int32_t max_redirects;
    /// 0: 16 MiB. Larger responses fail with `STURDY_IO_LIMIT_EXCEEDED`.
    uint64_t max_response_body_bytes;
    SturdyBool no_keep_alive;
    /// Do not ask for or decode gzip/deflate.
    SturdyBool no_decompress;
    /// Keep and send cookies (a private jar per client).
    SturdyBool cookies;
    SturdyBool no_http2;
    /// Do not switch to HTTP/3 on `Alt-Svc`.
    SturdyBool no_http3;
    /// Try HTTP/3 first, before any `Alt-Svc` was seen.
    SturdyBool http3_prior_knowledge;
    const char *user_agent;
    /// `http://[user:password@]host:port`; https targets go through CONNECT and stay verified end to end.
    const char *proxy;
    const char *const *no_proxy;
    uint32_t no_proxy_count;
    SturdyTlsClientOptions tls;
    /// Follow an https -> http redirect (refused by default).
    SturdyBool allow_https_downgrade;
    /// 0: 64 KiB.
    uint32_t max_response_header_bytes;
    /// 0: 4.
    uint32_t max_idle_connections_per_host;
    /// 0: 30 s.
    int64_t idle_connection_timeout_ms;
    /// 0: 3 s before falling back from HTTP/3.
    int64_t http3_connect_timeout_ms;
    /// A "cookie_jar" to share or persist (implies `cookies`); the zero handle: a private jar when `cookies` is set.
    SturdyAsyncObject cookie_jar;
} SturdyHttpClientOptions;

/// Creates a client with a connection pool (HTTP/1.1 keep-alive, HTTP/2 multiplexing, HTTP/3 after `Alt-Svc`). Object "http_client".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_client_create(const SturdyHttpClientOptions *options, SturdyAsyncObject *out_client);

/// Sends a request. Number: status. Fields: the response headers. Bytes: the body. Text: the final URL after redirects. Non-2xx
/// statuses are ordinary results; only transport, TLS, protocol, limit and timeout problems fail the op. `X-` headers etc. are yours;
/// `Host`, `Content-Length`, `Connection` and `Transfer-Encoding` are managed by the client and ignored if supplied.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_client_request(SturdyAsyncObject client,
                                                                  const char *method,
                                                                  const char *url,
                                                                  const SturdyKeyValue *headers,
                                                                  uint32_t header_count,
                                                                  const uint8_t *body,
                                                                  size_t body_size,
                                                                  SturdyAsyncOp *out_op);

/// Closes idle pooled connections.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_client_close_idle(SturdyAsyncObject client);

// ---- HTTP server --------------------------------------------------------------------------------

/// One request, valid only during the handler call (copy what you keep).
typedef struct SturdyHttpRequest {
    const char *method;
    const char *target;
    /// The client's address (IPv4-mapped IPv6 normalised).
    const char *remote_address;
    /// 11 = HTTP/1.1, 20 = HTTP/2, 30 = HTTP/3.
    uint32_t version;
    const SturdyKeyValue *headers;
    uint32_t header_count;
    const uint8_t *body;
    size_t body_size;
    /// When the server spools large uploads to disk (`spool_requests_above`): the temporary file holding the body (`body_size` is then 0).
    /// Deleted after the response is sent; null otherwise.
    const char *body_file;
    // Set when the request came through a router (null / 0 for a plain handler):
    /// The percent-decoded path without the query.
    const char *path;
    /// The raw query string without '?'.
    const char *query;
    /// `:name` / `*name` captures of the matched pattern.
    const SturdyKeyValue *params;
    uint32_t param_count;
} SturdyHttpRequest;

/// Answers one request. Owned until `sturdy_http_respond` (which consumes it); never release it otherwise.
typedef struct SturdyHttpResponder {
    uint64_t token;
} SturdyHttpResponder;

/// Called for every request, on a handler thread (not the network thread). Call `sturdy_http_respond` exactly once with `responder`,
/// now or later from any thread. A request that is never answered holds its connection until the client gives up.
typedef void(STURDY_ABI_CALL *SturdyHttpHandlerFn)(void *user_data, const SturdyHttpRequest *request, SturdyHttpResponder responder);

typedef struct SturdyHttpServerOptions {
    /// null: "0.0.0.0".
    const char *bind_address;
    /// 0: any free port.
    uint16_t port;
    /// PEM files; both or neither (neither: plain HTTP).
    const char *tls_certificate_chain_file;
    const char *tls_private_key_file;
    /// 0: a default sized to the machine.
    uint32_t handler_threads;
    /// 0: 256.
    uint32_t max_connections;
    /// 0: 1 MiB.
    uint64_t max_request_body_bytes;
    /// 0: 32 KiB.
    uint32_t max_request_header_bytes;
    /// 0: 8 MiB.
    uint64_t max_response_body_bytes;
    /// 0: unlimited.
    uint32_t max_connections_per_ip;
    SturdyBool no_http2;
    /// Also serve HTTP/3 over QUIC (needs TLS) and advertise it with Alt-Svc.
    SturdyBool enable_http3;
    /// UDP port for HTTP/3; 0: the same number as the TCP port.
    uint16_t http3_port;
    /// Reactor threads serving the sockets; 0: automatic.
    uint32_t io_threads;
    /// Time allowed to drain active sessions on stop; 0: 5 s.
    uint32_t shutdown_timeout_ms;
    /// Request bodies larger than this go to a temporary file (`SturdyHttpRequest::body_file`) instead of memory; 0: always in memory.
    uint64_t spool_requests_above;
    /// Largest spooled upload (bigger ones get 413); 0: 1 GiB.
    uint64_t max_spooled_body_bytes;
    /// Streams one HTTP/2 connection may have in flight; 0: 100.
    uint32_t http2_max_concurrent_streams;
    /// 0: 100 / 1024 connections / 64 per address.
    uint32_t http3_max_concurrent_streams;
    uint64_t http3_max_connections;
    uint64_t http3_max_connections_per_address;
    // WebSockets upgraded through `sturdy_http_server_on_websocket`:
    /// 0: 1 MiB.
    uint64_t websocket_max_message_bytes;
    /// 0: 4 MiB.
    uint64_t websocket_max_queued_send_bytes;
    /// 0: 60 s.
    int64_t websocket_idle_timeout_ms;
    SturdyBool websocket_compression;
    /// If non-empty, an upgrade request carrying an Origin header must match one exactly.
    const char *const *websocket_allowed_origins;
    uint32_t websocket_allowed_origin_count;
    /// Supported subprotocols, most preferred first.
    const char *const *websocket_subprotocols;
    uint32_t websocket_subprotocol_count;
    /// Upgraded sockets alive at once; 0: 1024.
    uint64_t max_websockets;
} SturdyHttpServerOptions;

/// Starts an HTTP(S) server. Object "http_server". Stops (and drains) when the object is released.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_start(const SturdyHttpServerOptions *options,
                                                                SturdyHttpHandlerFn handler,
                                                                void *user_data,
                                                                SturdyAsyncObject *out_server);

/// Sends the response for a request. `headers` may be null. CR/LF in a header makes the server answer 500 instead.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_respond(SturdyHttpResponder responder,
                                                           uint32_t status,
                                                           const SturdyKeyValue *headers,
                                                           uint32_t header_count,
                                                           const uint8_t *body,
                                                           size_t body_size);

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_port(SturdyAsyncObject server, uint16_t *out_port);
/// 0 when HTTP/3 is not served.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_http3_port(SturdyAsyncObject server, uint16_t *out_port);
/// Counters as a finished op's fields (decimal text): connections_accepted, requests, responses_2xx..5xx, active_connections, ...
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_metrics(SturdyAsyncObject server, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_stop(SturdyAsyncObject server);

/// Static server facts as a finished op's fields: `running`, `tls`, `http2` ("1"/"0"), `port`, `http3_port`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_info(SturdyAsyncObject server, SturdyAsyncOp *out_op);

/// Creates a server without starting it, so WebSocket paths can be added first; `sturdy_http_server_begin` then starts it.
/// Object "http_server". `handler` answers every request that is not a WebSocket upgrade.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_create(const SturdyHttpServerOptions *options,
                                                                 SturdyHttpHandlerFn handler,
                                                                 void *user_data,
                                                                 SturdyAsyncObject *out_server);

/// Same, serving a router (see below). The router may be released afterwards: the server keeps an immutable snapshot of it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_create_router(const SturdyHttpServerOptions *options,
                                                                        SturdyAsyncObject router,
                                                                        SturdyAsyncObject *out_server);

/// `sturdy_http_server_create_router` and `sturdy_http_server_begin` in one step.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_start_router(const SturdyHttpServerOptions *options,
                                                                       SturdyAsyncObject router,
                                                                       SturdyAsyncObject *out_server);

/// Called for each WebSocket upgraded on a path registered with `sturdy_http_server_on_websocket`, on a handler thread. `socket` is a new
/// "websocket" object **you own** (release it when done; releasing closes the connection). `request` is valid only during the call.
typedef void(STURDY_ABI_CALL *SturdyWebSocketFn)(void *user_data, SturdyAsyncObject socket, const SturdyHttpRequest *request);

/// Upgrades requests whose path (without query) equals `path` to WebSockets, on the same port and over TLS when the server uses it. Call
/// before `sturdy_http_server_begin`. `user_data` must stay valid until the server object is released.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_on_websocket(SturdyAsyncObject server, const char *path, SturdyWebSocketFn handler, void *user_data);

/// Opens the listener and starts serving.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_server_begin(SturdyAsyncObject server);

/// A complete response. Zero-initialize, then set what you need: a status, headers, and one body — bytes, a file, or a stream.
typedef struct SturdyHttpResponse {
    uint32_t status;
    const SturdyKeyValue *headers;
    uint32_t header_count;
    const uint8_t *body;
    size_t body_size;
    /// Send `Connection: close` and drop the connection afterwards.
    SturdyBool close_connection;
    /// Serve the body from this file (streamed from disk, never fully loaded) instead of `body`; null otherwise.
    const char *body_file;
    /// With `body_file`: start at this byte...
    uint64_t body_offset;
    /// ...and send this many (needs `limit_body_length`); otherwise to the end of the file.
    uint64_t body_length;
    SturdyBool limit_body_length;
    /// Stream the body with chunked transfer encoding: write chunks to the "http_body" object `sturdy_http_respond_ex` returns.
    SturdyBool stream;
    /// Server-Sent Events: implies `stream`, adds `Content-Type: text/event-stream`, `Cache-Control: no-cache` and `X-Accel-Buffering: no`.
    SturdyBool sse;
    /// Chunks buffered before a write waits for the client; 0: 16.
    uint32_t stream_queue;
} SturdyHttpResponse;

/// Sends a response. Fails (without spending `responder`) with `STURDY_ERROR_INVALID_ARGUMENT` if more than one body is given. For `stream`
/// or `sse` responses `out_body` receives the writer object (required then); otherwise it is ignored and may be null. The writer is yours to
/// release; releasing it ends the body.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_respond_ex(SturdyHttpResponder responder, const SturdyHttpResponse *response, SturdyAsyncObject *out_body);

typedef enum SturdyHttpResponseKind {
    /// `text/plain; charset=utf-8`.
    STURDY_HTTP_RESPONSE_TEXT = 0,
    /// `text/html; charset=utf-8`.
    STURDY_HTTP_RESPONSE_HTML = 1,
    /// `application/json`; `body` must already be valid JSON text.
    STURDY_HTTP_RESPONSE_JSON = 2,
    /// `body` is the Location.
    STURDY_HTTP_RESPONSE_REDIRECT = 3,
    STURDY_HTTP_RESPONSE_KIND_FORCE_U32 = 0x7fffffff
} SturdyHttpResponseKind;

/// Sends a canned response of `kind` with `status` (a redirect usually 302/301/307/308) and the null-terminated `body`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_respond_canned(SturdyHttpResponder responder, SturdyHttpResponseKind kind, uint32_t status, const char *body);

// ---- streamed response bodies (the "http_body" object `sturdy_http_respond_ex` returns) ----

/// Queues a chunk. Finishes once it is accepted into the bounded queue (so a slow client paces the producer); fails with `STURDY_IO_CLOSED`
/// once the client is gone or the body was finished.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_body_write(SturdyAsyncObject body, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);

/// One Server-Sent Event. Line breaks in `event` and `id` are dropped, so a hostile value cannot inject fields.
typedef struct SturdySseEvent {
    /// May contain newlines; each line becomes a `data:` field. Null-terminated.
    const char *data;
    /// Optional event name and last-event-id.
    const char *event;
    const char *id;
    /// Reconnect delay hint, sent when `has_retry`.
    uint32_t retry_ms;
    SturdyBool has_retry;
} SturdySseEvent;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_body_write_sse(SturdyAsyncObject body, const SturdySseEvent *event, SturdyAsyncOp *out_op);
/// Ends the body once queued chunks drain.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_body_finish(SturdyAsyncObject body);
/// Finishes when the client disconnected or the server stopped, so a producer can stop. Cancelling it finishes it as cancelled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_body_closed(SturdyAsyncObject body, SturdyAsyncOp *out_op);

/// The wire format of one event, ending with the blank line that dispatches it (string-output convention).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_format_sse(const SturdySseEvent *event, char *buffer, size_t capacity, size_t *out_length);
/// Percent-decodes `text` (string-output convention); `STURDY_ERROR_INVALID_ARGUMENT` on a malformed escape or an encoded NUL.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_percent_decode(const char *text, SturdyBool plus_is_space, char *buffer, size_t capacity, size_t *out_length);

// ---- router -------------------------------------------------------------------------------------

/// Static file serving options. Zero means the default noted.
typedef struct SturdyStaticFilesOptions {
    /// Directory served; files outside it are unreachable, including through symlinks. Required.
    const char *root;
    /// Served for a request naming a directory; null: "index.html".
    const char *index_file;
    /// Sent as Cache-Control; null: "public, max-age=300".
    const char *cache_control;
    /// Files and directories starting with '.' are hidden (404) unless set.
    SturdyBool allow_dotfiles;
    /// Allow symlinks that point outside `root` (a link planted in the tree must not expose the disk).
    SturdyBool follow_symlinks;
    /// Do not serve `name.br` / `name.gz` siblings to clients that accept them.
    SturdyBool no_precompressed;
    /// Served (status 200) when nothing matches a GET — single-page-app routing. null: 404.
    const char *spa_fallback;
    /// Extra or overriding MIME mappings (name = extension with dot, value = type).
    const SturdyKeyValue *mime_types;
    uint32_t mime_type_count;
} SturdyStaticFilesOptions;

/// Creates a URL router. Patterns are '/'-separated literals, `:name` (one segment) and a final `*name` (the rest); literals beat
/// parameters, which beat wildcards. Object "router". HEAD requests reuse GET routes; unmatched methods on known paths get 405 with an
/// Allow header; malformed paths get 400.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_create(SturdyAsyncObject *out_router);
/// Adds a route (`method` upper-case, e.g. "GET"). `handler` runs on a handler thread, exactly like a server handler, with `path`, `query`
/// and `params` filled in. `user_data` must stay valid until every server serving this router is released.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_route(SturdyAsyncObject router, const char *method, const char *pattern, SturdyHttpHandlerFn handler, void *user_data);
/// Adds middleware that runs before every matching route. Call `sturdy_http_respond*` with the responder **before returning** to answer
/// the request yourself (auth, rate limits); return without answering to let it continue. Answering later is ignored.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_use(SturdyAsyncObject router, SturdyHttpHandlerFn middleware, void *user_data);
/// Serves files under `mount` (e.g. "/assets") from `options->root`, GET and HEAD only. `STURDY_ERROR_INVALID_ARGUMENT` when the root is not a directory.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_static_files(SturdyAsyncObject router, const char *mount, const SturdyStaticFilesOptions *options);
/// Replaces the default 404. The handler must answer before it returns.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_not_found(SturdyAsyncObject router, SturdyHttpHandlerFn handler, void *user_data);
/// Middleware answering 429 with Retry-After once one client address exceeds `rate_per_second` (bursts up to `burst`). `max_keys` 0: 10000.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_rate_limit(SturdyAsyncObject router, double rate_per_second, double burst, uint64_t max_keys);
/// Runs one request through the router synchronously (tests, in-process calls). Number: status. Fields: headers. Bytes: body (a streamed body is not collected).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_router_dispatch(SturdyAsyncObject router,
                                                              const char *method,
                                                              const char *target,
                                                              const char *remote_address,
                                                              const SturdyKeyValue *headers,
                                                              uint32_t header_count,
                                                              const uint8_t *body,
                                                              size_t body_size,
                                                              SturdyAsyncOp *out_op);

// ---- rate limiter and cookie jar ----------------------------------------------------------------

/// A keyed token bucket: each key may use `rate_per_second` operations per second on average, with bursts up to `burst`. Thread-safe; memory is
/// bounded by `max_keys` (0: 10000). Object "rate_limiter".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_create(double rate_per_second, double burst, uint64_t max_keys, SturdyAsyncObject *out_limiter);
/// Takes `cost` tokens for `key` if available.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_try_acquire(SturdyAsyncObject limiter, const char *key, double cost, SturdyBool *out_acquired);
/// Milliseconds until `key` could take `cost` tokens (0 if it could now).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_retry_after_ms(SturdyAsyncObject limiter, const char *key, double cost, int64_t *out_ms);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_tracked_keys(SturdyAsyncObject limiter, uint64_t *out_keys);

/// One cookie (RFC 6265).
typedef struct SturdyCookie {
    const char *name;
    const char *value;
    /// Lowercase, no leading dot.
    const char *domain;
    /// null: "/".
    const char *path;
    /// The Domain attribute was given: subdomains receive it too. Off: only the exact host.
    SturdyBool domain_wide;
    /// Only sent over https.
    SturdyBool secure;
    SturdyBool http_only;
    /// Seconds since the Unix epoch; 0: a session cookie.
    int64_t expires_unix;
} SturdyCookie;

/// A thread-safe cookie store: a client fills it from `Set-Cookie` and sends matching cookies. A Domain attribute must cover the request
/// host and have at least two labels; IP hosts keep host-only cookies; the jar is capped (50 per domain, 3000 total). Object "cookie_jar".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_create(SturdyAsyncObject *out_jar);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_add(SturdyAsyncObject jar, const SturdyCookie *cookie);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_store_from_response(SturdyAsyncObject jar,
                                                                             const char *host,
                                                                             const char *path,
                                                                             SturdyBool https,
                                                                             const SturdyKeyValue *response_headers,
                                                                             uint32_t header_count);
/// "a=1; b=2" for the cookies that apply to a request (string-output convention; empty if none).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_header_for(SturdyAsyncObject jar, const char *host, const char *path, SturdyBool https, char *buffer, size_t capacity, size_t *out_length);
/// Every stored cookie as a finished op's fields `name:N`, `value:N`, `domain:N`, `path:N`, `domain_wide:N`, `secure:N`, `http_only:N`, `expires:N` (N = 0, 1, ...).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_all(SturdyAsyncObject jar, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_clear(SturdyAsyncObject jar);
/// The jar a client uses (shared, not a copy). `STURDY_ERROR_NOT_AVAILABLE` when cookies are off.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_http_client_cookie_jar(SturdyAsyncObject client, SturdyAsyncObject *out_jar);

// ---- WebSocket ----------------------------------------------------------------------------------

typedef struct SturdyWebSocketOptions {
    /// 0: 1 MiB. Bigger incoming messages close the socket with 1009.
    uint64_t max_message_bytes;
    /// 0: 4 MiB of unsent data before `send` fails with `STURDY_IO_LIMIT_EXCEEDED`.
    uint64_t max_queued_send_bytes;
    /// 0: 60 s without inbound traffic drops the connection (pings keep healthy idle links alive).
    int64_t idle_timeout_ms;
    /// Negotiate permessage-deflate (the inflated size is bounded by `max_message_bytes`).
    SturdyBool compression;
    /// Received messages buffered before reading pauses; 0: 16.
    uint64_t inbox_messages;
    /// Time allowed for the opening handshake; 0: 10 s.
    int64_t handshake_timeout_ms;
} SturdyWebSocketOptions;

typedef struct SturdyWebSocketClientOptions {
    SturdyWebSocketOptions socket;
    SturdyTlsClientOptions tls;
    const SturdyKeyValue *headers;
    uint32_t header_count;
    const char *const *subprotocols;
    uint32_t subprotocol_count;
    /// 0: 30 s for resolve + connect + TLS + upgrade.
    int64_t timeout_ms;
} SturdyWebSocketClientOptions;

typedef struct SturdyWebSocketServerOptions {
    const char *bind_address;
    uint16_t port;
    SturdyWebSocketOptions socket;
    /// Serve wss:// with `tls` instead of ws://.
    SturdyBool use_tls;
    SturdyTlsServerOptions tls;
    const char *const *allowed_origins;
    uint32_t allowed_origin_count;
    const char *const *paths;
    uint32_t path_count;
    const char *const *subprotocols;
    uint32_t subprotocol_count;
    /// 0: 1024.
    uint64_t max_connections;
    /// 0: 16 KiB.
    uint32_t max_request_header_bytes;
} SturdyWebSocketServerOptions;

/// Connects (`ws://` or `wss://`). Object "websocket". `options` may be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_connect(const char *url, const SturdyWebSocketClientOptions *options, SturdyAsyncOp *out_op);
/// Starts a WebSocket server. Object "ws_server" (finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_server_listen(const SturdyWebSocketServerOptions *options, SturdyAsyncOp *out_op);
/// Waits for the next upgraded connection. Object "websocket".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_accept(SturdyAsyncObject server, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_server_port(SturdyAsyncObject server, uint16_t *out_port);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_server_close(SturdyAsyncObject server);
/// Queues a text message (UTF-8, `size` bytes; no terminator needed). The data is copied.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_send_text(SturdyAsyncObject socket, const char *text, size_t size, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_send_binary(SturdyAsyncObject socket, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// Sends a ping (payload up to 125 bytes).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_ping(SturdyAsyncObject socket, const uint8_t *payload, size_t size, SturdyAsyncOp *out_op);
/// Next data message. Bytes: payload. Number: 1 for text, 0 for binary. Fails with `STURDY_IO_CLOSED` once the connection ended.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_receive(SturdyAsyncObject socket, SturdyAsyncOp *out_op);
/// Graceful close with an RFC 6455 code (1000 normal) and a reason of up to 123 bytes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_close(SturdyAsyncObject socket, uint16_t code, const char *reason, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_is_open(SturdyAsyncObject socket, SturdyBool *out_open);
/// Fields: `target`, `origin`, `subprotocol`, `header:<Name>` for each upgrade-request header, `peer` ("host:port"), the TLS fields over wss, and
/// once closed `close_code`, `close_reason` (a finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_info(SturdyAsyncObject socket, SturdyAsyncOp *out_op);
/// Drops the connection without a closing handshake.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_websocket_abort(SturdyAsyncObject socket);

// ---- QUIC ---------------------------------------------------------------------------------------

/// Limits both QUIC endpoints advertise. Zero means the default noted.
typedef struct SturdyQuicLimits {
    /// 0: 30 s.
    int64_t idle_timeout_ms;
    /// Send keep-alives this often; 0: off. Keep it under the idle timeout and under your NAT's UDP timeout.
    int64_t keep_alive_ms;
    /// 0: 10 s.
    int64_t handshake_timeout_ms;
    /// Streams the peer may have open; 0: 100.
    uint32_t max_bidirectional_streams;
    uint32_t max_unidirectional_streams;
    /// Receive windows; 0: 256 KiB per stream, 2 MiB per connection. Memory is bounded by these.
    uint64_t stream_receive_window;
    uint64_t connection_receive_window;
    /// Do not offer unreliable datagrams.
    SturdyBool no_datagrams;
    /// Unacknowledged bytes one stream may buffer before `sturdy_quic_stream_send` waits; 0: 1 MiB.
    uint64_t stream_send_buffer;
    /// Datagrams buffered for `sturdy_quic_receive_datagram` / waiting to be sent; 0: 256 each.
    uint64_t datagram_receive_queue;
    uint64_t datagram_send_queue;
} SturdyQuicLimits;

typedef struct SturdyQuicClientOptions {
    SturdyQuicLimits limits;
    /// `alpn` is required.
    SturdyTlsClientOptions tls;
} SturdyQuicClientOptions;

typedef struct SturdyQuicServerOptions {
    SturdyQuicLimits limits;
    /// Certificate, key and `alpn` are required.
    SturdyTlsServerOptions tls;
    const char *bind_address;
    uint16_t port;
    /// 0: 1024.
    uint64_t max_connections;
    /// 0: 32.
    uint64_t max_connections_per_address;
    /// Skip the Retry round trip that proves client addresses are real (trusted networks only).
    SturdyBool no_address_validation;
} SturdyQuicServerOptions;

/// Connects and completes the TLS 1.3 handshake. Object "quic_conn". Datagrams and many streams, no head-of-line blocking.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_connect(const char *host, uint16_t port, const SturdyQuicClientOptions *options, SturdyAsyncOp *out_op);
/// Starts a QUIC server. Object "quic_listener" (finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_listen(const SturdyQuicServerOptions *options, SturdyAsyncOp *out_op);
/// Next connection whose handshake completed. Object "quic_conn".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_accept(SturdyAsyncObject listener, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_listener_port(SturdyAsyncObject listener, uint16_t *out_port);
/// Counters as a finished op's fields: connections_accepted, retries_sent, rejected_limit, ...
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_listener_metrics(SturdyAsyncObject listener, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_listener_stop(SturdyAsyncObject listener);
/// Opens a stream (waits if the peer's stream limit is reached). Object "quic_stream".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_open_stream(SturdyAsyncObject connection, SturdyBool bidirectional, SturdyAsyncOp *out_op);
/// Next stream the peer opened. Object "quic_stream".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_accept_stream(SturdyAsyncObject connection, SturdyAsyncOp *out_op);
/// Sends one unreliable datagram; it may be lost or reordered. `STURDY_ERROR_OUT_OF_RANGE` when larger than the current maximum,
/// `STURDY_ERROR_BUSY` when the send queue is full, `STURDY_ERROR_NOT_AVAILABLE` when the peer takes no datagrams.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_send_datagram(SturdyAsyncObject connection, const uint8_t *data, size_t size);
/// Next received datagram. Bytes: the payload.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_receive_datagram(SturdyAsyncObject connection, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_max_datagram_size(SturdyAsyncObject connection, size_t *out_size);
/// Closes the connection with an application error code (0 = clean).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_close(SturdyAsyncObject connection, uint64_t error_code, const char *reason);
/// Finishes when the connection ends, for any reason. Number: the code. Text: the reason. Fields: `from_peer`, `application`, `timed_out` ("1"/"0").
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_closed(SturdyAsyncObject connection, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_is_open(SturdyAsyncObject connection, SturdyBool *out_open);
/// Fields: `peer`, `local`, and once the handshake finished `protocol`, `alpn`, `peer_sha256`, `cipher` (a finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_info(SturdyAsyncObject connection, SturdyAsyncOp *out_op);
/// Fields (decimal): rtt_us, min_rtt_us, bytes_in_flight, cwnd, packets_lost, bytes_sent, bytes_received (a finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stats(SturdyAsyncObject connection, SturdyAsyncOp *out_op);
/// Queues data on a stream; finishes once it fits under `stream_send_buffer` (so a slow peer paces the sender).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_send(SturdyAsyncObject stream, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// Ends the sending side cleanly after everything queued.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_finish(SturdyAsyncObject stream, SturdyAsyncOp *out_op);
/// 1..`max_bytes` bytes as they arrive. Bytes: empty means the peer finished. Reading is what grants the peer more credit.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_receive(SturdyAsyncObject stream, size_t max_bytes, SturdyAsyncOp *out_op);
/// Abruptly stops sending (the peer's reads fail with `error_code`) / asks the peer to stop sending.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_reset(SturdyAsyncObject stream, uint64_t error_code);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_stop_sending(SturdyAsyncObject stream, uint64_t error_code);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_id(SturdyAsyncObject stream, int64_t *out_id);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_quic_stream_is_bidirectional(SturdyAsyncObject stream, SturdyBool *out_bidirectional);

// ---- peer-to-peer, relay, NAT traversal ---------------------------------------------------------

typedef struct SturdyPeerOptions {
    /// Required: peers only talk to nodes with the same network id.
    const char *network_id;
    const char *display_name;
    /// Long-term identity (from `sturdy_tls_generate_identity`, persisted); both null: an ephemeral one.
    const char *identity_certificate_pem;
    const char *identity_private_key_pem;
    /// Do not accept inbound connections.
    SturdyBool no_listen;
    const char *bind_address;
    uint16_t port;
    /// 0: 64.
    uint64_t max_peers;
    /// 0: 1 MiB.
    uint64_t max_message_bytes;
    /// If non-empty, only these peer ids may connect or be connected to.
    const char *const *allowed_peers;
    uint32_t allowed_peer_count;
    SturdyBool no_peer_exchange;
    /// Announce on the local network (multicast + broadcast).
    SturdyBool lan_discovery;
    SturdyBool no_auto_connect;
    /// 0: 10 s.
    int64_t handshake_timeout_ms;
    /// Inbound links that may be mid-handshake at once; 0: 32.
    uint64_t max_pending_inbound;
    /// Keep-alive pings and the silence after which a link is dropped; 0: 15 s and 45 s.
    int64_t ping_interval_ms;
    int64_t idle_timeout_ms;
    /// Events buffered for `sturdy_peer_next_event`; 0: 1024.
    uint64_t event_queue;
    /// LAN discovery: UDP port (0: 47831), multicast group (null: "239.255.83.31") and announce period (0: 5 s).
    uint16_t discovery_port;
    const char *discovery_group;
    int64_t discovery_interval_ms;
} SturdyPeerOptions;

/// Starts a peer node: encrypted, mutually authenticated links with self-certifying ids. Object "peer"; text: our peer id.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_start(const SturdyPeerOptions *options, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_id(SturdyAsyncObject peer, char *buffer, size_t capacity, size_t *out_length);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_listen_port(SturdyAsyncObject peer, uint16_t *out_port);
/// The node's identity as a finished op: fields `certificate_pem`, `private_key_pem`, `sha256`. Persist it to keep the same id.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_identity(SturdyAsyncObject peer, SturdyAsyncOp *out_op);
/// Dials a peer. `expected_id` non-null pins it (the link is refused if someone else answers). Fields of the linked peer: id, name,
/// address, inbound, listen_port.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_connect(SturdyAsyncObject peer, const char *host, uint16_t port, const char *expected_id, SturdyAsyncOp *out_op);
/// Meets a peer through a relay when neither can be dialed. Both sides use the same `token` and opposite `dial`; the peer-to-peer TLS runs
/// through the relay end to end. `relay_tls` non-null: the hop to the relay itself uses TLS too.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_connect_via_relay(SturdyAsyncObject peer,
                                                                     const char *relay_host,
                                                                     uint16_t relay_port,
                                                                     const char *token,
                                                                     SturdyBool dial,
                                                                     const char *expected_id,
                                                                     const SturdyTlsClientOptions *relay_tls,
                                                                     SturdyAsyncOp *out_op);
/// Sends a message on channel 0..0xFEFF to one peer.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_send(SturdyAsyncObject peer, const char *peer_id, uint16_t channel, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// Sends to every connected peer, except `except_peer_id` when non-null (forwarding what that peer just sent). Number: how many links took it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_broadcast(SturdyAsyncObject peer, uint16_t channel, const uint8_t *data, size_t size, const char *except_peer_id, SturdyAsyncOp *out_op);
/// Next event. Number: 0 connected, 1 disconnected, 2 message, 3 discovered. Bytes: the message. Fields: peer, channel, name, address, reason, inbound.
/// Not consuming events eventually stalls the peers' readers.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_next_event(SturdyAsyncObject peer, SturdyAsyncOp *out_op);
/// Connected peers as a finished op: fields `id:N`, `name:N`, `address:N`, `inbound:N`, `listen_port:N` for N = 0, 1, ...
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_list(SturdyAsyncObject peer, SturdyAsyncOp *out_op);
/// Drops a peer; `reason` (null: "disconnected by application") is what the other side sees.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_disconnect(SturdyAsyncObject peer, const char *peer_id, const char *reason);
/// Refuses the peer from now on and drops it if connected.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_block(SturdyAsyncObject peer, const char *peer_id);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_peer_stop(SturdyAsyncObject peer);

typedef struct SturdyRelayOptions {
    const char *bind_address;
    uint16_t port;
    /// Wrap the hop to the relay in TLS (hides tokens and metadata from the network).
    SturdyBool use_tls;
    SturdyTlsServerOptions tls;
    /// 0: 1024 joined sessions / 1024 waiting connections / 16 waiting per address.
    uint64_t max_sessions;
    uint64_t max_waiting;
    uint64_t max_waiting_per_address;
    /// 0: 30 s a peer waits for its partner; 0: 120 s idle limit on a joined session.
    int64_t pair_timeout_ms;
    int64_t idle_timeout_ms;
    /// Cut a session after this many relayed bytes; 0: unlimited.
    uint64_t max_session_bytes;
    /// Longest rendezvous token accepted; 0: 128.
    uint64_t max_token_bytes;
} SturdyRelayOptions;

/// Lets the relay authenticate clients. Called on a network thread (answer quickly) with the rendezvous token and the client's address; return
/// non-zero to accept, zero to refuse. `user_data` must stay valid until the relay is released.
typedef SturdyBool(STURDY_ABI_CALL *SturdyRelayAuthorizeFn)(void *user_data, const char *token, const char *client_address);

/// Starts a rendezvous relay: pairs two connections that name the same token and forwards opaque bytes. It cannot read the peers' traffic
/// when they run TLS through it. Object "relay_server". `authorize` may be null (any token is accepted).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_server_start(const SturdyRelayOptions *options, SturdyRelayAuthorizeFn authorize, void *user_data, SturdyAsyncObject *out_relay);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_server_port(SturdyAsyncObject relay, uint16_t *out_port);
/// Counters as a finished op's fields: sessions_started, sessions_active, waiting, bytes_relayed, rejected, timed_out.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_server_metrics(SturdyAsyncObject relay, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_server_stop(SturdyAsyncObject relay);
/// A fresh unguessable rendezvous token (32 hex characters).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_random_token(char *buffer, size_t capacity, size_t *out_length);
/// Connects to a relay and waits for the partner. Object "tcp" connected to the partner: start TLS on it before sending anything private.
/// `relay_tls` non-null: TLS to the relay itself. `timeout_ms` 0: 35 s.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_relay_connect(const char *host,
                                                            uint16_t port,
                                                            const char *token,
                                                            SturdyBool dial,
                                                            const SturdyTlsClientOptions *relay_tls,
                                                            int64_t timeout_ms,
                                                            SturdyAsyncOp *out_op);

/// Router port-mapping options. Zero means the default noted.
typedef struct SturdyPortMapOptions {
    /// Router address; null: the default gateway (found on Linux only).
    const char *gateway;
    /// 0: one hour.
    int64_t lifetime_seconds;
    /// 0: the same number as the internal port.
    uint16_t external_port;
    /// Per method; 0: 3 s.
    int64_t timeout_ms;
    /// Bit set: 1 PCP, 2 NAT-PMP, 4 UPnP. 0: all three, in that order.
    uint32_t methods;
    /// 0: 5351.
    uint16_t gateway_port;
    /// UPnP discovery target; null: the SSDP multicast group.
    const char *ssdp_host;
    /// 0: 1900.
    uint16_t ssdp_port;
} SturdyPortMapOptions;

/// Asks the router to forward a public port to this machine (PCP, NAT-PMP or UPnP IGD). Object "mapping". Number: granted lifetime in
/// seconds (0: permanent — remove it yourself). Fields: external_host, external_port, method, gateway. Renew before half the lifetime.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_nat_map_port(SturdyBool tcp, uint16_t internal_port, const SturdyPortMapOptions *options, SturdyAsyncOp *out_op);
/// Removes a mapping made by `sturdy_nat_map_port`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_nat_unmap_port(SturdyAsyncObject mapping, const SturdyPortMapOptions *options, SturdyAsyncOp *out_op);
/// The default gateway's address ("" when it cannot be determined).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_nat_default_gateway(char *buffer, size_t capacity, size_t *out_length);
/// UDP hole punching: both peers call this at once with the other's candidate addresses ("host:port") and a shared secret. Text/number: the
/// candidate that answered; keep using the same socket afterwards. Fails with `STURDY_IO_TIMED_OUT` (typical of symmetric NATs: use a relay).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_nat_punch_hole(SturdyAsyncObject udp_socket,
                                                             const char *const *candidates,
                                                             uint32_t candidate_count,
                                                             const char *shared_secret,
                                                             int64_t timeout_ms,
                                                             SturdyAsyncOp *out_op);

// ---- files, watching, processes -----------------------------------------------------------------

typedef enum SturdyFileMode {
    STURDY_FILE_READ = 1,
    STURDY_FILE_WRITE = 2,
    /// Create the file if missing (needs write or append).
    STURDY_FILE_CREATE = 4,
    STURDY_FILE_TRUNCATE = 8,
    /// Every write goes to the end.
    STURDY_FILE_APPEND = 16,
    /// Create, failing if it exists (atomic).
    STURDY_FILE_CREATE_NEW = 32,
    STURDY_FILE_MODE_FORCE_U32 = 0x7fffffff
} SturdyFileMode;

/// Opens a file for streaming and random access. Object "file". `mode` is a bit set of `SturdyFileMode`. Positional reads and writes use
/// io_uring on Linux when available; nothing occupies a worker thread while the kernel works.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_open(const char *path, uint32_t mode, SturdyAsyncOp *out_op);
/// Reads up to `max_bytes` at the cursor (advancing it). Bytes: empty at end of file.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_read(SturdyAsyncObject file, size_t max_bytes, SturdyAsyncOp *out_op);
/// Reads at `offset` without touching the cursor.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_read_at(SturdyAsyncObject file, uint64_t offset, size_t max_bytes, SturdyAsyncOp *out_op);
/// Reads from the cursor to the end; fails with `STURDY_IO_INVALID_ARGUMENT` if more than `max_bytes` remain.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_read_to_end(SturdyAsyncObject file, size_t max_bytes, SturdyAsyncOp *out_op);
/// Writes all of `data` at the cursor. Number: bytes written.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_write(SturdyAsyncObject file, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_write_at(SturdyAsyncObject file, uint64_t offset, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// Moves the cursor (`origin`: 0 start, 1 current, 2 end). Number: the new position.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_seek(SturdyAsyncObject file, int64_t offset, int32_t origin, SturdyAsyncOp *out_op);
/// Number: the size in bytes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_size(SturdyAsyncObject file, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_set_len(SturdyAsyncObject file, uint64_t length, SturdyAsyncOp *out_op);
/// Flushes to stable storage (fsync, or fdatasync when `data_only`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_sync(SturdyAsyncObject file, SturdyBool data_only, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_close(SturdyAsyncObject file);

typedef struct SturdyWatchOptions {
    SturdyBool recursive;
    /// Bursts of writes within this window become one event; 0: 50 ms.
    int64_t debounce_ms;
    /// Use the portable polling backend even where the OS offers events.
    SturdyBool force_polling;
    /// The polling backend's scan period; 0: 250 ms.
    int64_t poll_interval_ms;
    /// Events buffered before an overflow event is reported; 0: 4096.
    uint64_t max_pending_events;
} SturdyWatchOptions;

/// Watches a path for changes (inotify on Linux, polling elsewhere). Object "watcher". `options` may be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_path(const char *path, const SturdyWatchOptions *options, SturdyAsyncObject *out_watcher);
/// Whether the OS's event API is used (false: polling).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_uses_native_backend(SturdyAsyncObject watcher, SturdyBool *out_native);
/// The remaining events as a "stream" object; each item is `sturdy_frames_pack` of {kind as decimal text, path, old_path, "1"/"0" for is_directory}.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_events_stream(SturdyAsyncObject watcher, SturdyAsyncObject *out_stream);
/// Next change. Number: 0 created, 1 modified, 2 removed, 3 renamed, 4 overflow (rescan). Text: path. Fields: old_path, is_directory.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_next(SturdyAsyncObject watcher, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_close(SturdyAsyncObject watcher);

typedef struct SturdyProcessOptions {
    /// Executable name (PATH lookup when it has no directory) or path.
    const char *program;
    const char *const *args;
    uint32_t arg_count;
    /// null: inherit.
    const char *working_directory;
    /// Added to (or, with `clear_environment`, replacing) the environment.
    const SturdyKeyValue *environment;
    uint32_t environment_count;
    SturdyBool clear_environment;
    /// Terminate the child when its object is released while it still runs.
    SturdyBool kill_on_release;
    /// What the child's standard streams connect to (`SturdyStdioMode`; 0 is a pipe). For `sturdy_process_spawn`; `sturdy_process_run`
    /// always pipes what it collects. A stream that is not a pipe cannot be read or written.
    uint32_t stdin_mode;
    uint32_t stdout_mode;
    uint32_t stderr_mode;
} SturdyProcessOptions;

typedef enum SturdyStdioMode {
    STURDY_STDIO_PIPE = 0,
    /// The null device (reads end at once, writes vanish).
    STURDY_STDIO_NULL = 1,
    /// Shared with this process.
    STURDY_STDIO_INHERIT = 2,
    STURDY_STDIO_MODE_FORCE_U32 = 0x7fffffff
} SturdyStdioMode;

/// Runs a program to completion, feeding `stdin_data` and collecting stdout/stderr (bounded; the child is killed past `max_output_bytes`
/// — 0: 16 MiB — or `timeout_ms` — 0: no limit). Bytes: stdout. Text: stderr. Number: exit code (the signal number if signalled).
/// Fields: `signaled`, `success`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_run(const SturdyProcessOptions *options,
                                                          const uint8_t *stdin_data,
                                                          size_t stdin_size,
                                                          int64_t timeout_ms,
                                                          uint64_t max_output_bytes,
                                                          SturdyAsyncOp *out_op);
/// Starts a child (standard streams per the options, piped by default). Object "process".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_spawn(const SturdyProcessOptions *options, SturdyAsyncObject *out_process);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_write_stdin(SturdyAsyncObject process, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_close_stdin(SturdyAsyncObject process);
/// Bytes: up to `max_bytes` from the child's stdout / stderr (empty at end).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_read_stdout(SturdyAsyncObject process, size_t max_bytes, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_read_stderr(SturdyAsyncObject process, size_t max_bytes, SturdyAsyncOp *out_op);
/// Number: exit code (the signal number if signalled). Fields: `signaled`, `success`, `signal`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_wait(SturdyAsyncObject process, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_id(SturdyAsyncObject process, int64_t *out_id);
/// Asks the child to exit (SIGTERM / a close request); `sturdy_process_kill` forces it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_request_exit(SturdyAsyncObject process);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_process_kill(SturdyAsyncObject process);

// ---- data parallelism (Rayon) and thread pools --------------------------------------------------

/// Body of `sturdy_par_for`: called once per index, on any worker thread. Must not unwind.
typedef void(STURDY_ABI_CALL *SturdyParForFn)(void *user_data, uint64_t index);
/// Body of `sturdy_par_for_chunks`: called with consecutive `[begin, end)` ranges.
typedef void(STURDY_ABI_CALL *SturdyParChunkFn)(void *user_data, uint64_t begin, uint64_t end);

/// Element type for `sturdy_par_sort`.
typedef enum SturdyNumericType {
    STURDY_NUMERIC_F32 = 0,
    STURDY_NUMERIC_F64 = 1,
    STURDY_NUMERIC_I32 = 2,
    STURDY_NUMERIC_I64 = 3,
    STURDY_NUMERIC_U32 = 4,
    STURDY_NUMERIC_U64 = 5,
    STURDY_NUMERIC_TYPE_FORCE_U32 = 0x7fffffff
} SturdyNumericType;

/// Runs `body` for every index in `[0, count)` across the worker pool and returns when all are done (the caller helps, so it never
/// deadlocks the pool). `min_chunk` is the smallest run of indices handed to one task (0/1: let the engine decide). Calling it from inside
/// a body is fine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_for(uint64_t count, uint64_t min_chunk, SturdyParForFn body, void *user_data);
/// Runs `body` over consecutive ranges of at most `chunk` indices (`chunk` must be positive).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_for_chunks(uint64_t count, uint64_t chunk, SturdyParChunkFn body, void *user_data);
/// Sorts `count` elements of `type` in place, ascending, in parallel.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sort(SturdyNumericType type, void *data, uint64_t count);
/// Parallel sums. Float sums use SIMD and may differ from a sequential sum by rounding.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sum_f32(const float *data, uint64_t count, float *out_sum);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sum_f64(const double *data, uint64_t count, double *out_sum);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sum_i64(const int64_t *data, uint64_t count, int64_t *out_sum);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sum_u64(const uint64_t *data, uint64_t count, uint64_t *out_sum);
/// Parallel minimum and maximum. `STURDY_ERROR_OUT_OF_RANGE` when `count` is 0.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_min_max_f32(const float *data, uint64_t count, float *out_min, float *out_max);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_min_max_f64(const double *data, uint64_t count, double *out_min, double *out_max);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_min_max_i64(const int64_t *data, uint64_t count, int64_t *out_min, int64_t *out_max);

/// Creates a separate worker pool (Rayon's `ThreadPoolBuilder`): work run through it stays on its own threads, so a long job cannot
/// starve the engine's scheduler. `threads` 0: the machine's core count. Object "pool"; released objects shut their threads down.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_pool_create(uint32_t threads, const char *name, SturdyAsyncObject *out_pool);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_pool_threads(SturdyAsyncObject pool, uint32_t *out_threads);
/// `sturdy_par_for` on the pool's own threads.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_pool_par_for(SturdyAsyncObject pool, uint64_t count, uint64_t min_chunk, SturdyParForFn body, void *user_data);
/// Runs `task` on the pool and waits for it (anything it parallelises stays on the pool).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_pool_run(SturdyAsyncObject pool, SturdyTaskFn task, void *user_data);

// ---- whole-file helpers, directories ------------------------------------------------------------

/// Reads a whole file on the IO lane. Bytes: the content; `STURDY_IO_INVALID_ARGUMENT` when it is larger than `max_bytes` (0: unlimited).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_read_file(const char *path, uint64_t max_bytes, SturdyAsyncOp *out_op);
/// Reads `byte_count` bytes at `offset` (fewer at the end of the file). Bytes: the content.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_read_range(const char *path, uint64_t offset, uint64_t byte_count, SturdyAsyncOp *out_op);
/// Creates or replaces the file with `data`, or appends to it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_write_file(const char *path, const uint8_t *data, size_t size, SturdyBool append, SturdyAsyncOp *out_op);
/// Number: the size in bytes.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_size(const char *path, SturdyAsyncOp *out_op);
/// Number: 1 if the path exists, else 0.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_exists(const char *path, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_create_directories(const char *path, SturdyAsyncOp *out_op);
/// Number: 1 if something was removed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_remove_file(const char *path, SturdyAsyncOp *out_op);
/// Removes a directory and everything in it. Number: 1 if something was removed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_remove_directory(const char *path, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_copy(const char *source, const char *destination, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_rename(const char *source, const char *destination, SturdyAsyncOp *out_op);
/// Entry paths of a directory. Bytes: the paths as packed frames (`sturdy_frames_count` / `sturdy_frames_get`). Number: how many.
/// At most `max_entries` (0: 4096); more fail with `STURDY_IO_INVALID_ARGUMENT`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_fs_list(const char *path, uint64_t max_entries, SturdyBool recursive, SturdyAsyncOp *out_op);
/// Walks a directory entry by entry. Object "dir_iter".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dir_iter_create(const char *path, SturdyBool recursive, SturdyAsyncObject *out_iter);
/// Number: 1 and text: the next path, or number 0 at the end.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dir_iter_next(SturdyAsyncObject iter, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_file_is_open(SturdyAsyncObject file, SturdyBool *out_open);

/// A read-only view of bytes.
typedef struct SturdyBytesView {
    const uint8_t *data;
    size_t size;
} SturdyBytesView;

/// Several byte strings in one: a 32-bit little-endian count, then each as a 32-bit little-endian length and its bytes. Used where an
/// operation or stream item carries more than one value. Packs `count` views (bytes-output convention: null buffer to learn the size).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frames_pack(const SturdyBytesView *frames, uint32_t count, uint8_t *buffer, size_t capacity, size_t *out_length);
/// How many frames `packed` holds; `STURDY_ERROR_INVALID_ARGUMENT` when it is malformed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frames_count(const uint8_t *packed, size_t size, uint32_t *out_count);
/// Frame `index` (bytes-output convention).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_frames_get(const uint8_t *packed, size_t size, uint32_t index, uint8_t *buffer, size_t capacity, size_t *out_length);

// ---- generic byte streams: copy, buffer, frame --------------------------------------------------
//
// Readers: "tcp", "file", "pipe_end", "memory_reader", "process" (stdout or stderr). Writers: "tcp", "file", "pipe_end", "memory_writer",
// "process" (stdin). A `SturdyProcessStream` says which pipe of a "process" is meant and is ignored for every other kind.

typedef enum SturdyProcessStream {
    STURDY_PROCESS_STDOUT = 0,
    STURDY_PROCESS_STDERR = 1,
    STURDY_PROCESS_STDIN = 2,
    STURDY_PROCESS_STREAM_FORCE_U32 = 0x7fffffff
} SturdyProcessStream;

typedef struct SturdyCopyOptions {
    /// 0: 64 KiB.
    uint64_t chunk_bytes;
    /// Stop after this many bytes; 0: until the reader ends.
    uint64_t limit;
    /// Shut the writer down (close a TCP write side, sync a file, close a process's stdin) when done.
    SturdyBool shutdown_writer;
} SturdyCopyOptions;

/// Copies until the reader ends, overlapping each read with the previous write. Number: bytes copied.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_io_copy(SturdyAsyncObject reader,
                                                      SturdyProcessStream reader_stream,
                                                      SturdyAsyncObject writer,
                                                      SturdyProcessStream writer_stream,
                                                      const SturdyCopyOptions *options,
                                                      SturdyAsyncOp *out_op);
/// Bytes: everything until the reader ends; `STURDY_IO_LIMIT_EXCEEDED` past `max_bytes` (0: unlimited).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_io_read_to_end(SturdyAsyncObject reader, SturdyProcessStream stream, uint64_t max_bytes, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_io_write_all(SturdyAsyncObject writer, SturdyProcessStream stream, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);

/// A reader over a copy of `data`. Object "memory_reader".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_memory_reader_create(const uint8_t *data, size_t size, SturdyAsyncObject *out_reader);
/// A writer that collects what it is given. Object "memory_writer".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_memory_writer_create(SturdyAsyncObject *out_writer);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_memory_writer_bytes(SturdyAsyncObject writer, uint8_t *buffer, size_t capacity, size_t *out_length);
/// Two connected in-memory ends ("pipe_end"): what one writes the other reads. `chunks`: messages buffered per direction (0: 8).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_memory_pipe_create(uint64_t chunks, SturdyAsyncObject *out_end_a, SturdyAsyncObject *out_end_b);

/// Adds read-ahead to a reader (lines, delimiters, exact counts). One operation at a time per object. Object "buffered_reader".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_reader_create(SturdyAsyncObject reader, SturdyProcessStream stream, uint64_t chunk_bytes, SturdyAsyncObject *out_reader);
/// Bytes: up to `max_bytes` (empty at the end).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_read_some(SturdyAsyncObject reader, uint64_t max_bytes, SturdyAsyncOp *out_op);
/// Bytes: exactly `count`; `STURDY_IO_UNEXPECTED_EOF` when the stream ends first.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_read_exact(SturdyAsyncObject reader, uint64_t count, SturdyAsyncOp *out_op);
/// Number 1 and bytes: up to and including `delimiter`; number 0 at a clean end of stream; `STURDY_IO_LIMIT_EXCEEDED` past `max_bytes`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_read_until(SturdyAsyncObject reader, const char *delimiter, uint64_t max_bytes, SturdyAsyncOp *out_op);
/// Number 1 and text: one line without its terminator (LF or CRLF); number 0 at a clean end of stream. `max_bytes` 0: 64 KiB.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_read_line(SturdyAsyncObject reader, uint64_t max_bytes, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_reader_buffered(SturdyAsyncObject reader, uint64_t *out_bytes);
/// Batches small writes. Object "buffered_writer". `capacity` 0: 16 KiB.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_writer_create(SturdyAsyncObject writer, SturdyProcessStream stream, uint64_t capacity, SturdyAsyncObject *out_writer);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_write(SturdyAsyncObject writer, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// Sends what is pending.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_flush(SturdyAsyncObject writer, SturdyAsyncOp *out_op);
/// Flushes, then shuts the underlying writer down.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_buffered_shutdown(SturdyAsyncObject writer, SturdyAsyncOp *out_op);

typedef enum SturdyFrameCodec {
    /// A 4-byte big-endian length prefix, then the frame.
    STURDY_FRAME_LENGTH_DELIMITED = 0,
    /// One line per frame (no newline inside a frame).
    STURDY_FRAME_LINES = 1,
    STURDY_FRAME_CODEC_FORCE_U32 = 0x7fffffff
} SturdyFrameCodec;

/// Messages over a byte stream. `io` must read and write ("tcp", "file", "pipe_end"). `max_frame_bytes` 0: 8 MiB (64 KiB for lines).
/// Object "framed".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_framed_create(SturdyAsyncObject io, SturdyFrameCodec codec, uint64_t max_frame_bytes, SturdyAsyncObject *out_framed);
/// Number 1 and bytes: the next frame; number 0 at a clean end of stream.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_framed_next(SturdyAsyncObject framed, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_framed_send(SturdyAsyncObject framed, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
/// The remaining frames as a "stream" object (do not call `sturdy_framed_next` meanwhile).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_framed_stream(SturdyAsyncObject framed, SturdyAsyncObject *out_stream);

// ---- cancellation -------------------------------------------------------------------------------

/// A cancellation source and the tokens it hands out: cancel once, and everything armed on a token reacts. Objects "cancel_source" / "cancel_token".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_source_create(SturdyAsyncObject *out_source);
/// Cancels. `out_first` (may be null) says whether this call was the first.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_source_cancel(SturdyAsyncObject source, SturdyBool *out_first);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_source_is_cancelled(SturdyAsyncObject source, SturdyBool *out_cancelled);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_source_token(SturdyAsyncObject source, SturdyAsyncObject *out_token);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_token_is_cancelled(SturdyAsyncObject token, SturdyBool *out_cancelled);
/// Finishes (OK) when the token is cancelled, at once if it already is.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_cancel_token_wait(SturdyAsyncObject token, SturdyAsyncOp *out_op);
/// Cancelling `token` cancels `op`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_link_cancel(SturdyAsyncOp op, SturdyAsyncObject token);
/// A new op that finishes as `op` does, or fails with `STURDY_IO_TIMED_OUT` after `timeout_ms` — cancelling `op` then. Cancelling the new op cancels `op`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_with_timeout(SturdyAsyncOp op, int64_t timeout_ms, SturdyAsyncOp *out_op);

// ---- message channels and synchronisation (Tokio `sync`) ----------------------------------------
//
// Messages are byte strings. Waiting costs no thread. A "permit" or a guard unlocks when released — explicitly, or when its object *and* the op
// that produced it are both released.

/// Result of a non-blocking send or receive.
typedef enum SturdyTryStatus {
    STURDY_TRY_OK = 0,
    /// Full (send) / empty (receive): try again later.
    STURDY_TRY_WOULD_BLOCK = 1,
    STURDY_TRY_CLOSED = 2,
    STURDY_TRY_STATUS_FORCE_U32 = 0x7fffffff
} SturdyTryStatus;

/// Bounded multi-producer channel holding up to `capacity` messages (at least 1). Objects "channel_sender" / "channel_receiver". It closes when
/// the last sender is released or `sturdy_channel_close` is called on either end; buffered messages are still delivered.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_create(uint64_t capacity, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_sender_clone(SturdyAsyncObject sender, SturdyAsyncObject *out_sender);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver);
/// Finishes once the message is buffered or handed to a receiver (waits while the channel is full); `STURDY_IO_CLOSED` when closed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_try_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyTryStatus *out_status);
/// Number 1 and bytes: the next message; number 0 once the channel is closed and drained.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op);
/// Takes a buffered message if there is one: `out_status` says whether, and on `STURDY_TRY_OK` `out_op` is a finished op whose bytes are the message
/// (otherwise `out_op` is the zero handle).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_try_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op, SturdyTryStatus *out_status);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_channel_close(SturdyAsyncObject end);

/// One-shot value hand-off. Objects "oneshot_sender" / "oneshot_receiver". A sender released without sending fails the receiver with `STURDY_IO_CLOSED`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_oneshot_create(SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver);
/// `out_first` (may be null) says whether this call set the value.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_oneshot_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, SturdyBool *out_first);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_oneshot_fail(SturdyAsyncObject sender, const char *message, SturdyBool *out_first);
/// Bytes: the value. A receiver can be waited on once.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_oneshot_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op);

/// A single value many tasks observe. Objects "watch_sender" / "watch_receiver"; each receiver tracks what it has seen on its own.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_send(SturdyAsyncObject sender, const uint8_t *data, size_t size);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_subscribe(SturdyAsyncObject sender, SturdyAsyncObject *out_receiver);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver);
/// The current value (bytes-output convention); `_and_update` also marks it seen.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_borrow(SturdyAsyncObject receiver, uint8_t *buffer, size_t capacity, size_t *out_length);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_borrow_and_update(SturdyAsyncObject receiver, uint8_t *buffer, size_t capacity, size_t *out_length);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_has_changed(SturdyAsyncObject receiver, SturdyBool *out_changed);
/// Finishes when the value changed since this receiver last looked; `STURDY_IO_CLOSED` once the sender is gone and nothing new remains.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_watch_value_changed(SturdyAsyncObject receiver, SturdyAsyncOp *out_op);

/// Every receiver sees every message; one that falls `capacity` behind is told how many it missed. Objects "broadcast_sender" / "broadcast_receiver".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_broadcast_create(uint64_t capacity, SturdyAsyncObject *out_sender, SturdyAsyncObject *out_receiver);
/// `out_receivers` (may be null): the number of live receivers.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_broadcast_send(SturdyAsyncObject sender, const uint8_t *data, size_t size, uint64_t *out_receivers);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_broadcast_subscribe(SturdyAsyncObject sender, SturdyAsyncObject *out_receiver);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_broadcast_receiver_clone(SturdyAsyncObject receiver, SturdyAsyncObject *out_receiver);
/// Number 0 and bytes: a message; number 1 and field `skipped`: this receiver lagged; number 2: the sender is gone. Cannot be cancelled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_broadcast_recv(SturdyAsyncObject receiver, SturdyAsyncOp *out_op);

/// A counting semaphore. Object "semaphore".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_create(uint64_t permits, SturdyAsyncObject *out_semaphore);
/// Waits for `count` permits. Object "permit". `STURDY_IO_CLOSED` if the semaphore closes first.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_acquire(SturdyAsyncObject semaphore, uint64_t count, SturdyAsyncOp *out_op);
/// `out_permit` receives the zero handle when the permits are not available.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_try_acquire(SturdyAsyncObject semaphore, uint64_t count, SturdyAsyncObject *out_permit);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_add_permits(SturdyAsyncObject semaphore, uint64_t count);
/// Fails every waiter and every future acquire.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_close(SturdyAsyncObject semaphore);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_available(SturdyAsyncObject semaphore, uint64_t *out_permits);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_semaphore_is_closed(SturdyAsyncObject semaphore, SturdyBool *out_closed);
/// Returns the permits now.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_permit_release(SturdyAsyncObject permit);
/// Keeps the permits consumed forever (they are never returned).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_permit_forget(SturdyAsyncObject permit);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_permit_count(SturdyAsyncObject permit, uint64_t *out_count);

/// An async mutex / read-write lock protecting a byte-string value. Objects "async_mutex" / "async_rwlock".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_mutex_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_mutex);
/// Waits for exclusive access. Object "mutex_guard".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_mutex_lock(SturdyAsyncObject mutex, SturdyAsyncOp *out_op);
/// `out_guard` receives the zero handle when the mutex is held.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_mutex_try_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_create(const uint8_t *initial, size_t size, SturdyAsyncObject *out_lock);
/// Many readers or one writer; waiters queue in order, so a waiting writer is not starved. Objects "rwlock_read_guard" / "rwlock_write_guard".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_read(SturdyAsyncObject lock, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_async_rwlock_write(SturdyAsyncObject lock, SturdyAsyncOp *out_op);
/// The protected value (bytes-output convention).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_guard_get(SturdyAsyncObject guard, uint8_t *buffer, size_t capacity, size_t *out_length);
/// Replaces the value (mutex and write guards only).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_guard_set(SturdyAsyncObject guard, const uint8_t *data, size_t size);
/// Unlocks now.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_guard_release(SturdyAsyncObject guard);

/// A wake-up signal with at most one stored permit. Object "notify".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_notify_create(SturdyAsyncObject *out_notify);
/// Finishes when notified; a `sturdy_notify_one` that happened earlier with nobody waiting is consumed first.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_notify_notified(SturdyAsyncObject notify, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_notify_one(SturdyAsyncObject notify);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_notify_waiters(SturdyAsyncObject notify);
/// A rendezvous of `parties` callers. Object "barrier"; `sturdy_barrier_wait` has number 1 for exactly one of them (the last to arrive).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_barrier_create(uint64_t parties, SturdyAsyncObject *out_barrier);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_barrier_wait(SturdyAsyncObject barrier, SturdyAsyncOp *out_op);

// ---- tasks --------------------------------------------------------------------------------------

/// Where a task's result is written from inside its callback (copied immediately).
typedef struct SturdyTaskOutput {
    /// Engine-private; do not touch.
    void *internal;
} SturdyTaskOutput;

/// Sets a task's result: a number, bytes and text; a non-null, non-empty `error` fails the task with that message.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_output_set(SturdyTaskOutput *output, int64_t number, const uint8_t *bytes, size_t size, const char *text, const char *error);

/// The work of a task, run on a worker thread. `cancel_token` (a "cancel_token", valid only during the call) turns cancelled when the task's op is
/// cancelled or its join set aborted: poll `sturdy_cancel_token_is_cancelled` to stop early. Fill `output` before returning. Must not unwind.
typedef void(STURDY_ABI_CALL *SturdyTaskWorkFn)(void *user_data, SturdyAsyncObject cancel_token, SturdyTaskOutput *output);

typedef enum SturdyTaskLane {
    /// The worker pool, for short work.
    STURDY_LANE_LIGHT = 0,
    /// The worker pool, for long work.
    STURDY_LANE_HEAVY = 1,
    /// The bounded blocking-IO lane, for work that waits on the OS.
    STURDY_LANE_BLOCKING = 2,
    STURDY_TASK_LANE_FORCE_U32 = 0x7fffffff
} SturdyTaskLane;

/// Runs `work` and reports its result as an op: number, bytes and text as set; a task error fails the op. `user_data` must stay valid until the
/// task finishes. Cancelling the op sets the token the work polls.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_spawn(SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane, SturdyAsyncOp *out_op);
/// Same, on a custom pool (`sturdy_pool_create`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_spawn_on(SturdyAsyncObject pool, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op);

/// A dynamic set of tasks joined in completion order. Object "join_set".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_join_set_create(SturdyAsyncObject *out_set);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_join_set_spawn(SturdyAsyncObject set, SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane);
/// Finishes with the next task to complete. Field `outcome` is "ok", "failed" (field `error` says why) or "empty" (nothing left); number, bytes
/// and text are the task's.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_join_set_join_next(SturdyAsyncObject set, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_join_set_len(SturdyAsyncObject set, uint64_t *out_len);
/// Sets the cancel token of every running task in the set.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_join_set_abort_all(SturdyAsyncObject set);

/// Tasks that run only on the thread that pumps the set. Object "local_set".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_local_set_create(SturdyAsyncObject *out_set);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_local_set_spawn(SturdyAsyncObject set, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op);
/// Runs queued tasks on the calling thread until none are left. `out_ran` (may be null): how many.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_local_set_run_pending(SturdyAsyncObject set, uint64_t *out_ran);
/// Runs queued tasks on the calling thread until `op` finishes (blocks).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_local_set_block_on(SturdyAsyncObject set, SturdyAsyncOp op);

/// A named thread that runs submitted work one at a time, in order. Object "dedicated_thread".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_create(const char *name, SturdyAsyncObject *out_thread);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_run(SturdyAsyncObject thread, SturdyTaskWorkFn work, void *user_data, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_pin_to_core(SturdyAsyncObject thread, uint32_t core_index, SturdyBool *out_pinned);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_dedicated_thread_pin_to_fastest_core(SturdyAsyncObject thread, SturdyBool *out_pinned);

/// A value that follows the flow of work: tasks spawned while it is bound inherit it. Object "task_local".
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_local_create(SturdyAsyncObject *out_local);
/// Runs `body` with `value` bound.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_local_scope(SturdyAsyncObject local, const uint8_t *value, size_t size, SturdyTaskFn body, void *user_data);
/// The bound value (bytes-output convention); `STURDY_ERROR_NOT_AVAILABLE` when none is bound here.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_task_local_get(SturdyAsyncObject local, uint8_t *buffer, size_t capacity, size_t *out_length);

/// Queues `fn` for the engine's main thread (run when it pumps). `user_data` must stay valid until `fn` has run.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_main_thread_run(SturdyTaskFn fn, void *user_data);
/// Runs the queued main-thread work on the calling thread.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_main_thread_pump(void);
/// Installs a callback run (on the posting thread) after every queued job, so a loop that blocks waiting for events can wake and pump. Null removes it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_main_thread_set_wake_hook(SturdyTaskFn hook, void *user_data);

// ---- runtime, topology, timers, signals ---------------------------------------------------------

/// Starts the worker pool (0 workers: one per hardware thread). The engine starts it on first use; call this only to choose the size
/// first, or in a program that does not run the engine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scheduler_initialize(uint32_t workers);
/// Stops the worker pool. The engine owns it: only a standalone program should call this.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_scheduler_shutdown(void);
/// Tunes the blocking-IO lane (0 workers: automatic; 0 `max_pending`: 1024). `out_applied` says whether it took effect (false once it started).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_io_lane_configure(uint32_t workers, uint32_t max_pending, SturdyBool *out_applied);
/// Fields `backend`, `workers`, `pending`, `capacity` (a finished op).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_io_lane_stats(SturdyAsyncOp *out_op);
/// Sets the number of network reactor threads. `out_applied` says whether it took effect (false once the reactor is running).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_network_reactor_configure(uint32_t threads, SturdyBool *out_applied);
/// Logical (or physical) cores ranked fastest first (bytes-output convention in `uint32_t` units: `out_count` always receives the number).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_topology_ranked_cores(SturdyBool physical, uint32_t *buffer, size_t capacity, size_t *out_count);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_thread_pin_current(uint32_t core_index, SturdyBool *out_pinned);

/// Finishes at an absolute wall-clock time (milliseconds since the Unix epoch), at once if it has passed.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_timer_sleep_until(int64_t unix_ms, SturdyAsyncOp *out_op);
/// A periodic timer that does not drift. Object "interval"; `sturdy_interval_tick` finishes at each period boundary.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_interval_create(int64_t period_ms, SturdyAsyncObject *out_interval);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_interval_tick(SturdyAsyncObject interval, SturdyAsyncOp *out_op);
/// Number: the first of `signals` (SIGINT, SIGTERM, ...) delivered to the process.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_signal_wait(const int32_t *signals, uint32_t count, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_signal_ctrl_c(SturdyAsyncOp *out_op);

// ---- streams of byte messages (the `Stream<T>` pipeline) ----------------------------------------
//
// A "stream" is consumed by the combinator or sink that takes it: using it afterwards fails with `STURDY_ERROR_INVALID_ARGUMENT`. Callbacks
// run on whichever thread completed the previous stage; keep them short. Sinks other than `sturdy_stream_next` also stop when their op is
// cancelled (as the next item arrives).

/// Where a stream callback writes the item it produces (copied immediately).
typedef struct SturdyBytesSink {
    /// Engine-private; do not touch.
    void *internal;
} SturdyBytesSink;
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_bytes_sink_set(SturdyBytesSink *sink, const uint8_t *data, size_t size);

typedef void(STURDY_ABI_CALL *SturdyStreamMapFn)(void *user_data, const uint8_t *data, size_t size, SturdyBytesSink *out);
/// Returns non-zero to keep the item.
typedef SturdyBool(STURDY_ABI_CALL *SturdyStreamPredicateFn)(void *user_data, const uint8_t *data, size_t size);
/// Returns non-zero to emit what was written to `out`, zero to drop the item.
typedef SturdyBool(STURDY_ABI_CALL *SturdyStreamFilterMapFn)(void *user_data, const uint8_t *data, size_t size, SturdyBytesSink *out);
typedef void(STURDY_ABI_CALL *SturdyStreamEachFn)(void *user_data, const uint8_t *data, size_t size);
typedef void(STURDY_ABI_CALL *SturdyStreamFoldFn)(void *user_data, const uint8_t *accumulator, size_t accumulator_size, const uint8_t *item, size_t item_size, SturdyBytesSink *out_accumulator);
/// Called for each pull on a blocking-lane thread (it may block). Write the next item to `out` and return non-zero, or return zero to end the stream.
typedef SturdyBool(STURDY_ABI_CALL *SturdyStreamSourceFn)(void *user_data, SturdyBytesSink *out);

/// Sources. Every callback's `user_data` must stay valid until the stream is finished with.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_from_channel(SturdyAsyncObject receiver, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_of_items(const SturdyBytesView *items, uint32_t count, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_empty(SturdyAsyncObject *out_stream);
/// Items are 8-byte little-endian steady-clock nanoseconds, one per period.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_interval(int64_t period_ms, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_generate(SturdyStreamSourceFn source, void *user_data, SturdyAsyncObject *out_stream);
/// Adapters. Each takes (consumes) its input stream(s) and returns a new one.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_map(SturdyAsyncObject stream, SturdyStreamMapFn fn, void *user_data, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_filter(SturdyAsyncObject stream, SturdyStreamPredicateFn pred, void *user_data, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_filter_map(SturdyAsyncObject stream, SturdyStreamFilterMapFn fn, void *user_data, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_inspect(SturdyAsyncObject stream, SturdyStreamEachFn fn, void *user_data, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_take(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_skip(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_take_while(SturdyAsyncObject stream, SturdyStreamPredicateFn pred, void *user_data, SturdyAsyncObject *out_stream);
/// Items become frames {index as 8-byte little-endian, item}.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_enumerate(SturdyAsyncObject stream, SturdyAsyncObject *out_stream);
/// Items become packed frames of up to `count` items; the `_timeout` form also emits a partial batch once `max_wait_ms` passed since its first item.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_chunks(SturdyAsyncObject stream, uint64_t count, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_chunks_timeout(SturdyAsyncObject stream, uint64_t count, int64_t max_wait_ms, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_chain(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_merge(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream);
/// Items become frames {a, b}.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_zip(SturdyAsyncObject first, SturdyAsyncObject second, SturdyAsyncObject *out_stream);
/// Runs `fn` on the worker pool for up to `concurrency` items at once (0: twice the worker count); `ordered` keeps the input order.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_map_concurrent(SturdyAsyncObject stream, SturdyStreamMapFn fn, void *user_data, uint64_t concurrency, SturdyBool ordered, SturdyAsyncObject *out_stream);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_throttle(SturdyAsyncObject stream, int64_t period_ms, SturdyAsyncObject *out_stream);
/// An item that takes longer than `limit_ms` fails the next pull with `STURDY_IO_TIMED_OUT`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_timeout_each(SturdyAsyncObject stream, int64_t limit_ms, SturdyAsyncObject *out_stream);
/// Sinks. Number 1 and bytes: the next item; number 0 at the end. (Not consuming; cannot be cancelled — close the source to stop it.)
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_next(SturdyAsyncObject stream, SturdyAsyncOp *out_op);
/// Drains the stream. Bytes: the items as packed frames. Number: how many.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_collect(SturdyAsyncObject stream, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_for_each(SturdyAsyncObject stream, SturdyStreamEachFn fn, void *user_data, SturdyAsyncOp *out_op);
/// Bytes: the final accumulator.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_fold(SturdyAsyncObject stream, const uint8_t *initial, size_t initial_size, SturdyStreamFoldFn fn, void *user_data, SturdyAsyncOp *out_op);
/// Number: how many items.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_count(SturdyAsyncObject stream, SturdyAsyncOp *out_op);
/// Number 1 and bytes: the last item; number 0 for an empty stream.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_last(SturdyAsyncObject stream, SturdyAsyncOp *out_op);
/// Sends every item into a channel (does not close it).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_stream_forward(SturdyAsyncObject stream, SturdyAsyncObject sender, SturdyAsyncOp *out_op);

// ---- more data parallelism ----------------------------------------------------------------------

typedef SturdyBool(STURDY_ABI_CALL *SturdyParPredicateFn)(void *user_data, uint64_t index);
typedef int64_t(STURDY_ABI_CALL *SturdyParMapI64Fn)(void *user_data, uint64_t index);
typedef int64_t(STURDY_ABI_CALL *SturdyParReduceI64Fn)(void *user_data, int64_t a, int64_t b);
typedef double(STURDY_ABI_CALL *SturdyParMapF64Fn)(void *user_data, uint64_t index);
typedef double(STURDY_ABI_CALL *SturdyParReduceF64Fn)(void *user_data, double a, double b);
/// Returns non-zero when the element at `a` sorts before the one at `b`.
typedef SturdyBool(STURDY_ABI_CALL *SturdyParLessFn)(void *user_data, const void *a, const void *b);
/// Body of `sturdy_par_scope`; `scope` ("par_scope") is valid until `sturdy_par_scope` returns.
typedef void(STURDY_ABI_CALL *SturdyParScopeFn)(void *user_data, SturdyAsyncObject scope);

/// The threads a parallel call can use right now.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_num_threads(uint32_t *out_threads);
/// Runs `a` and `b` in parallel and returns when both are done.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_join(SturdyTaskFn a, void *a_user_data, SturdyTaskFn b, void *b_user_data);
/// Runs `body` with a scope; every task spawned on the scope finishes before this returns.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_scope(SturdyParScopeFn body, void *user_data);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_scope_spawn(SturdyAsyncObject scope, SturdyTaskFn fn, void *user_data);
/// Calls `body(begin)` for every window start in `[0, count - window]`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_for_windows(uint64_t count, uint64_t window, SturdyParForFn body, void *user_data);
/// Parallel predicates over `[0, count)`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_any(uint64_t count, SturdyParPredicateFn pred, void *user_data, SturdyBool *out_result);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_all(uint64_t count, SturdyParPredicateFn pred, void *user_data, SturdyBool *out_result);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_count(uint64_t count, SturdyParPredicateFn pred, void *user_data, uint64_t *out_count);
/// The lowest index for which `pred` holds (-1: none) / any such index.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_find_first(uint64_t count, SturdyParPredicateFn pred, void *user_data, int64_t *out_index);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_find_any(uint64_t count, SturdyParPredicateFn pred, void *user_data, int64_t *out_index);
/// Maps every index and combines the results with an associative `reduce` that has `identity`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_map_reduce_i64(uint64_t count, SturdyParMapI64Fn map, int64_t identity, SturdyParReduceI64Fn reduce, void *user_data, int64_t *out_result);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_map_reduce_f64(uint64_t count, SturdyParMapF64Fn map, double identity, SturdyParReduceF64Fn reduce, void *user_data, double *out_result);
/// Sorts `count` elements of `element_size` bytes in place with `less`; `stable` keeps equal elements in order.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_sort_by(void *data, uint64_t count, uint64_t element_size, SturdyBool stable, SturdyParLessFn less, void *user_data);

typedef enum SturdyReduction {
    STURDY_REDUCE_SUM = 0,
    STURDY_REDUCE_PRODUCT = 1,
    STURDY_REDUCE_MIN = 2,
    STURDY_REDUCE_MAX = 3,
    STURDY_REDUCTION_FORCE_U32 = 0x7fffffff
} SturdyReduction;

/// The result of `sturdy_par_reduce`: the field matching the element type (f32 and f64 use `f64`, i32 and i64 use `i64`, u32 and u64 use `u64`;
/// 32-bit sums are accumulated in 64 bits).
typedef struct SturdyNumericValue {
    double f64;
    int64_t i64;
    uint64_t u64;
} SturdyNumericValue;

/// Reduces a typed array in parallel. `out_has_value` is false for an empty array with min/max.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_par_reduce(SturdyNumericType type, SturdyReduction reduction, const void *data, uint64_t count, SturdyNumericValue *out_value, SturdyBool *out_has_value);

// ---- shared-state primitives: blocking mutex, SPSC ring, Arc, Promise, execution senders -------------------------------------
//
// The engine's own `Mutex<T>`, `SpscRingBuffer<T>`, `Arc<T>`, `Promise<T>` and `Execution` layer, specialised to byte strings so that code in
// any language can use the same primitive the C++ code does. (Rust, Java and C# have their own mutexes and reference counts; these are for
// state that must be shared *with the engine and with other languages*, and they show up in the profiler like the C++ ones.)

/// A blocking mutex around a byte-string value, named for the profiler. Object "sync_mutex". It blocks the calling thread: never lock it on
/// a network or timer thread, never hold it across a wait, and release a guard on the thread that locked it. (`sturdy_async_mutex_*` waits
/// without a thread.)
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_create(const uint8_t *initial, size_t size, const char *debug_name, SturdyAsyncObject *out_mutex);
/// Blocks until the lock is free. Object "sync_guard": read and replace the value with `sturdy_guard_get` / `sturdy_guard_set`, unlock with
/// `sturdy_guard_release`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard);
/// `out_guard` receives the zero handle when the lock is held.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_sync_mutex_try_lock(SturdyAsyncObject mutex, SturdyAsyncObject *out_guard);

typedef enum SturdyRingStatus {
    STURDY_RING_OK = 0,
    /// A push found no room / a pop found nothing.
    STURDY_RING_FULL = 1,
    STURDY_RING_EMPTY = 2,
    /// The item is larger than the ring's slot size.
    STURDY_RING_TOO_LARGE = 3,
    STURDY_RING_STATUS_FORCE_U32 = 0x7fffffff
} SturdyRingStatus;

/// A wait-free single-producer single-consumer ring of byte messages. Object "spsc_ring". Exactly one thread may push and exactly one
/// (possibly another) may pop. Items are at most `max_item_bytes` (rounded up to 32, 128, 512, 2 Ki, 8 Ki or 64 Ki; 64 Ki is the limit); the
/// capacity (1..16 Mi items) is rounded up to a power of two.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_create(uint64_t capacity, uint64_t max_item_bytes, SturdyAsyncObject *out_ring);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_try_push(SturdyAsyncObject ring, const uint8_t *data, size_t size, SturdyRingStatus *out_status);
/// Pops the oldest item into `buffer`, which must hold at least `sturdy_ring_max_item_bytes` (a smaller one is refused before anything is
/// popped). `out_length` receives the item's size. `out_status` is `STURDY_RING_EMPTY` when there was nothing.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_try_pop(SturdyAsyncObject ring, uint8_t *buffer, size_t capacity, size_t *out_length, SturdyRingStatus *out_status);
/// Pops up to `max_items` (0: all) as a finished op: bytes are the items as packed frames (`sturdy_frames_count` / `sturdy_frames_get`), number is how many.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_drain(SturdyAsyncObject ring, uint64_t max_items, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_size(SturdyAsyncObject ring, uint64_t *out_size);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_capacity(SturdyAsyncObject ring, uint64_t *out_capacity);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_ring_max_item_bytes(SturdyAsyncObject ring, uint64_t *out_bytes);

/// Frees a foreign pointer owned by an arc. Runs once, when the last reference goes away, on whichever thread dropped it.
typedef void(STURDY_ABI_CALL *SturdyArcDestroyFn)(void *pointer);

/// A shared, reference-counted value: every clone is another handle to the same data, which is destroyed with the last one. Object "arc".
/// This one holds a copy of `data` (immutable).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_create(const uint8_t *data, size_t size, SturdyAsyncObject *out_arc);
/// An arc around a pointer of yours: share one native object (or a language runtime's handle to it) between languages and threads, with
/// `destroy` called exactly once when the last handle is released. `destroy` may be null.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_create_foreign(void *pointer, SturdyArcDestroyFn destroy, SturdyAsyncObject *out_arc);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_clone(SturdyAsyncObject arc, SturdyAsyncObject *out_arc);
/// How many references exist right now (a snapshot).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_use_count(SturdyAsyncObject arc, uint64_t *out_count);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_ptr_eq(SturdyAsyncObject a, SturdyAsyncObject b, SturdyBool *out_same);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_bytes(SturdyAsyncObject arc, uint8_t *buffer, size_t capacity, size_t *out_length);
/// The foreign pointer (null for a bytes arc).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_arc_pointer(SturdyAsyncObject arc, void **out_pointer);

/// The producing half of an operation: something other than the engine finishes `out_future` by calling `sturdy_promise_set_*`. Object
/// "promise"; `out_future` is an ordinary op (wait on it, attach a callback, link or time it out). Only the first completion wins; a promise
/// released while pending fails its future with `STURDY_IO_CLOSED`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_promise_create(SturdyAsyncObject *out_promise, SturdyAsyncOp *out_future);
/// Completes the future with a number, bytes and text. `out_first` (may be null) says whether this call completed it.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_promise_set_value(SturdyAsyncObject promise, int64_t number, const uint8_t *data, size_t size, const char *text, SturdyBool *out_first);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_promise_set_error(SturdyAsyncObject promise, SturdyIoError code, const char *message, SturdyBool *out_first);
/// Completes the future as cancelled.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_promise_cancel(SturdyAsyncObject promise, SturdyBool *out_first);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_promise_is_completed(SturdyAsyncObject promise, SturdyBool *out_completed);
/// An already-finished op (Tokio's `ready`): useful to answer an operation-shaped API without work.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_ready(int64_t number, const uint8_t *data, size_t size, const char *text, SturdyAsyncOp *out_op);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_op_failed(SturdyIoError code, const char *message, SturdyAsyncOp *out_op);

/// What the previous stage of an execution chain produced.
typedef struct SturdyTaskInput {
    int64_t number;
    const uint8_t *bytes;
    size_t size;
    const char *text;
} SturdyTaskInput;

/// A later stage of an execution chain: gets the previous stage's result and writes its own (see `sturdy_task_output_set`).
typedef void(STURDY_ABI_CALL *SturdyTaskThenFn)(void *user_data, SturdyAsyncObject cancel_token, const SturdyTaskInput *input, SturdyTaskOutput *output);

/// Lazy descriptions of work that compose and run later (the `Execution` layer). Object "exec_sender"; a sender may be started any number of
/// times. The `user_data` of every stage must stay valid until the sender is released and no run of it is still going.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_exec_just(SturdyTaskWorkFn work, void *user_data, SturdyTaskLane lane, SturdyAsyncObject *out_sender);
/// A sender that runs `then` on `lane` with the result of `sender`. A failure upstream skips it and propagates.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_exec_then(SturdyAsyncObject sender, SturdyTaskThenFn then, void *user_data, SturdyTaskLane lane, SturdyAsyncObject *out_sender);
/// Starts the chain. The op carries the last stage's number, bytes and text; cancelling it sets the token every stage receives.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_exec_submit(SturdyAsyncObject sender, SturdyAsyncOp *out_op);
/// Starts the chain and blocks for it; `out_op` is a finished op (OK, or failed with the stage's error). Never call it from a worker thread.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_exec_sync_wait(SturdyAsyncObject sender, SturdyAsyncOp *out_op);

// ---- scripts ------------------------------------------------------------------------------------------------------------------
//
// A *script* is a unit of game logic -- a module (see `SturdyGameModule`) -- written in any language, running next to scripts in other
// languages in one game. Where its code comes from depends on the language:
//
//  * Compiled languages (Rust, Java, C#, C++, Zig, Go ...): fill a `SturdyScriptInstance` with your callbacks and make a module of it with
//    `sturdy_script_module_create_native`. Nothing is interpreted.
//  * Interpreted languages (JavaScript, Lua, Python ...): the *application* brings the interpreter. Register a `SturdyScriptLanguage` adapter;
//    its `load` turns source text or bytecode into a `SturdyScriptInstance` whose callbacks run the script. The engine ships no interpreter,
//    only this seam; a script then loads by name (`sturdy_script_module_create`, `sturdy_script_load`) or by file extension.
//
// What a script may do: everything this header offers, with the `SturdyEngine` handle its callbacks receive. `update` runs at the engine's
// update boundary, when nothing else touches the world, so a script may spawn and destroy entities, change components and register systems
// directly rather than through command buffers.
//
// **Threading.** Every callback of every script runs with one lock held (the script lock): an interpreter that is not thread-safe never sees
// two threads at once. `start`, `update`, `on_message`, `call` and `stop` run on the thread that drives the engine's updates; `request_render_frame`
// may run on a render thread (it takes the same lock). A script that needs parallelism is a native module, which is never serialized.
//
// **Reload.** `sturdy_script_reload` loads the new version first (a parse error leaves the old one running), then asks the old one for its state
// (`save_state`), stops it, gives the state to the new one (`restore_state`) and starts it.
//
// **Hosting calls.** Scripts talk to the host application and to each other through byte strings: named host functions
// (`sturdy_script_register_host_function`), calls by script and function name (`sturdy_script_call`), and messages (`sturdy_script_send`,
// topics with `sturdy_script_publish` / `sturdy_script_subscribe`; delivered at the next update boundary, in send order). An adapter exposes
// those to its language however suits it.

/// Describes where a script comes from. Strings are copied unless noted.
typedef struct SturdyScriptSource {
    /// Set to `sizeof(SturdyScriptSource)`.
    uint32_t struct_size;
    uint32_t reserved;
    /// Unique, non-empty. The script is a module of this name.
    const char *name;
    /// The language adapter ("lua", "js"). Null or empty: chosen from `path`'s extension.
    const char *language;
    /// A file the adapter may read, and the extension that picks a language. May be null.
    const char *path;
    /// The source text or bytecode. Empty (null): the adapter loads `path`.
    const uint8_t *code;
    size_t code_size;
    /// Adapter-specific settings (a module root, strict mode, ...). May be null.
    const SturdyKeyValue *options;
    uint32_t option_count;
} SturdyScriptSource;

/// One engine update as a script sees it.
typedef struct SturdyScriptTick {
    double delta_seconds;
    double elapsed_seconds;
    uint64_t tick;
} SturdyScriptTick;

/// A message between scripts. Valid only during `on_message`.
typedef struct SturdyScriptMessage {
    /// The sending script; empty when the host application sent it.
    const char *from;
    const char *topic;
    const uint8_t *payload;
    size_t payload_size;
} SturdyScriptMessage;

/// A running script: callbacks plus the pointer they work on. Every callback may be null (a no-op). Fill it in `SturdyScriptLanguage::load`, or
/// pass it to `sturdy_script_module_create_native`. Callbacks must not unwind.
typedef struct SturdyScriptInstance {
    /// Set to `sizeof(SturdyScriptInstance)`.
    uint32_t struct_size;
    uint32_t reserved;
    /// Passed to every callback below.
    void *user_data;
    /// Runs once when the script starts. Return `STURDY_FALSE` to abort the load; write the reason to `error` (see `sturdy_bytes_sink_set`).
    SturdyBool(STURDY_ABI_CALL *start)(SturdyEngine engine, void *user_data, SturdyBytesSink *error);
    /// Runs once per engine update, at the update boundary, in module start order.
    void(STURDY_ABI_CALL *update)(SturdyEngine engine, const SturdyScriptTick *tick, void *user_data);
    /// Contributes to a frame exactly like `SturdyGameLogic::request_render_frame`. May run on a render thread.
    SturdyBool(STURDY_ABI_CALL *request_render_frame)(SturdyEngine engine, SturdySurface surface, const SturdyFrameInput *input, SturdyFrame frame, void *user_data);
    /// Runs once when the script is unloaded or replaced, after `save_state`.
    void(STURDY_ABI_CALL *stop)(SturdyEngine engine, void *user_data);
    /// Hot reload: write the state to hand to the replacement into `out` and return `STURDY_TRUE`, or return `STURDY_FALSE` for none.
    SturdyBool(STURDY_ABI_CALL *save_state)(void *user_data, SturdyBytesSink *out);
    /// Receives the previous version's state before `start`. Return `STURDY_FALSE` to refuse it (the reload fails and the script stays unloaded).
    SturdyBool(STURDY_ABI_CALL *restore_state)(void *user_data, const uint8_t *state, size_t size);
    /// Calls a function the script exposes. Write the result to `result` (or the reason to `error`) and return whether it worked.
    SturdyBool(STURDY_ABI_CALL *call)(void *user_data, const char *function, const uint8_t *args, size_t args_size, SturdyBytesSink *result, SturdyBytesSink *error);
    /// A message sent to this script or published on a topic it subscribed to.
    void(STURDY_ABI_CALL *on_message)(void *user_data, const SturdyScriptMessage *message);
    /// Releases `user_data`, exactly once, after `stop` (or if the script never started).
    void(STURDY_ABI_CALL *destroy)(void *user_data);
} SturdyScriptInstance;

/// A language adapter an application registers to run scripts in an interpreter it provides.
typedef struct SturdyScriptLanguage {
    /// Set to `sizeof(SturdyScriptLanguage)`.
    uint32_t struct_size;
    uint32_t reserved;
    /// The name `SturdyScriptSource::language` refers to. Copied.
    const char *name;
    /// File extensions with the dot (".lua") that select this language. May be null.
    const char *const *extensions;
    uint32_t extension_count;
    void *user_data;
    /// Parses or compiles `source` into `out_instance` (struct_size set by you). Return `STURDY_FALSE` with the reason in `error` when it does
    /// not load. The instance must not start running yet. Called with the script lock held.
    SturdyBool(STURDY_ABI_CALL *load)(void *user_data, const SturdyScriptSource *source, SturdyScriptInstance *out_instance, SturdyBytesSink *error);
    /// Releases `user_data` once the adapter is unregistered and every script it loaded is gone. May be null.
    void(STURDY_ABI_CALL *destroy)(void *user_data);
} SturdyScriptLanguage;

/// Registers a language adapter for the whole process (before the runtime starts, or while it runs).
/// @return `STURDY_ERROR_INVALID_ARGUMENT` when a language of that name already exists; nothing is taken then (you keep `user_data`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_language_register(const SturdyScriptLanguage *language);
/// Forgets a language. Scripts already running keep working until they are unloaded.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_language_unregister(const char *name);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_language_count(uint32_t *out_count);
/// The name of language `index` (string-output convention).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_language_name(uint32_t index, char *buffer, size_t capacity, size_t *out_length);

/// A host function: scripts call it by name with a byte string and get a byte string back. Write the result to `result` (or the reason to
/// `error`) and return whether it worked. May be called on any thread a script runs on.
typedef SturdyBool(STURDY_ABI_CALL *SturdyScriptHostFn)(void *user_data, const uint8_t *args, size_t args_size, SturdyBytesSink *result, SturdyBytesSink *error);
/// Registers `function` under `name` for the whole process. `destroy` (may be null) releases `user_data` when the function is unregistered.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_register_host_function(const char *name, SturdyScriptHostFn function, void *user_data, void(STURDY_ABI_CALL *destroy)(void *user_data));
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_unregister_host_function(const char *name);
/// Calls a host function (what an adapter does on a script's behalf). Returns a finished op: bytes on success, a failure otherwise.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_call_host_function(const char *name, const uint8_t *args, size_t args_size, SturdyAsyncOp *out_op);

/// A script as a module: where it comes from and where it sits in the start order.
typedef struct SturdyScriptModuleDesc {
    /// Set to `sizeof(SturdyScriptModuleDesc)`.
    uint32_t struct_size;
    uint32_t reserved;
    SturdyScriptSource source;
    /// Modules that must start before this one (and shut down after it).
    const char *const *after;
    uint32_t after_count;
    /// Nonzero: the script may supply the scene for a frame; zero (the usual): it never replaces the scene.
    SturdyBool contributes_scene;
} SturdyScriptModuleDesc;

/// Makes the module for a script that loads through a registered language adapter when the game starts. Pass it to `sturdy_runtime_run_modules` or
/// `sturdy_module_request_load`, or release it with `sturdy_script_module_discard` if you never will. The strings inside `out_module` stay valid until then.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_module_create(const SturdyScriptModuleDesc *desc, SturdyGameModule *out_module);

/// A script built from callbacks directly (no language adapter): the way compiled languages write scripts.
typedef struct SturdyNativeScriptDesc {
    /// Set to `sizeof(SturdyNativeScriptDesc)`.
    uint32_t struct_size;
    uint32_t reserved;
    const char *name;
    /// Copied; its `destroy` runs exactly once.
    SturdyScriptInstance instance;
    const char *const *after;
    uint32_t after_count;
    SturdyBool contributes_scene;
} SturdyNativeScriptDesc;

STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_module_create_native(const SturdyNativeScriptDesc *desc, SturdyGameModule *out_module);
/// Destroys a module made by the functions above that was never handed to the engine.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_module_discard(SturdyGameModule *module);

/// `sturdy_script_module_create` and `sturdy_module_request_load` in one step: adds a script to the running game at the next update boundary.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_load(SturdyEngine engine, const SturdyScriptModuleDesc *desc);
/// Hot reload (see above). Takes effect at the next update boundary; a parse error is reported at once. Failures after that are reported through
/// `sturdy_module_take_failure`.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_reload(SturdyEngine engine, const SturdyScriptSource *source);
/// Hot reload of a script built from callbacks: `instance` replaces the running one.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_reload_native(SturdyEngine engine, const char *name, const SturdyScriptInstance *instance);
/// Removes a script (as `sturdy_module_request_unload`).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_unload(SturdyEngine engine, const char *name);

/// How many scripts are loaded, and the name of script `index` (start order, string-output convention).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_count(SturdyEngine engine, uint32_t *out_count);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_name(SturdyEngine engine, uint32_t index, char *buffer, size_t capacity, size_t *out_length);
/// A script's details as a finished op's fields: `language`, `path`, `generation` (1 + reloads), `healthy` ("1"/"0") and `failure`.
/// `STURDY_ERROR_NOT_AVAILABLE` for an unknown script.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_info(SturdyEngine engine, const char *name, SturdyAsyncOp *out_op);

/// Calls a function of a script now (taking the script lock, so it is safe from any thread). Returns a finished op: bytes on success, a failure
/// otherwise (an unknown script or function, or the script's own error).
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_call(SturdyEngine engine, const char *script, const char *function, const uint8_t *args, size_t args_size, SturdyAsyncOp *out_op);

/// Queues a message for `to` (delivered at the next update boundary). `from` null: the host application. `STURDY_ERROR_NOT_AVAILABLE` when no such script is loaded.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_send(SturdyEngine engine, const char *from, const char *to, const char *topic, const uint8_t *payload, size_t size);
/// Queues a message for every script subscribed to `topic` except `from`. `out_receivers` (may be null) receives how many.
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_publish(SturdyEngine engine, const char *from, const char *topic, const uint8_t *payload, size_t size, uint32_t *out_receivers);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_subscribe(SturdyEngine engine, const char *script, const char *topic);
STURDY_ABI SturdyResult STURDY_ABI_CALL sturdy_script_unsubscribe(SturdyEngine engine, const char *script, const char *topic);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // STURDY_FFI_STURDY_H
