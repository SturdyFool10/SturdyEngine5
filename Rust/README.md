# Rust support (`Rust/`)

First-party Rust support for SturdyEngine 5, living in the engine's own repository: parts of a game, or a whole
application, can be written in Rust. The C++ API is called **directly** (no C ABI) through the `cxx` crate: the Rust
side authors every `#[cxx::bridge]`, `build.rs` runs cxx-gen over them, and a thin C++ shim is compiled inside the
engine's own CMake build. It builds against this checkout, so there is no pinned or vendored engine revision.
(`CxxApi/` is the engine's cxx-bindable surface that the shim is being moved onto; see `plans/cxx-api.md`.)

| crate | role |
|---|---|
| `sturdy-sys/` | Builds the vendored engine as static archives (CMake, from `build.rs`), the C++ shim, and the `cxx` bridges — one bridge module per engine subsystem |
| `sturdy/` | The ergonomic API on top: `GameLogic` trait, `RuntimeConfig` builder, `Engine<'_>` (lifetime-scoped) and its subsystem accessors, a Tokio-shaped `task`/`sync`/`time` layer |
| `sturdy-macros/` | `#[derive(Bundle)]` for spawning multi-component ECS entities in one call |
| `examples/hello/` | Rust game logic running on the real engine, doubling as the integration test — every subsystem below is exercised against a live engine run, not just compiled |

```rust
use sturdy::prelude::*;

struct Game;
impl GameLogic for Game {
    fn frame(&mut self, engine: &mut Engine<'_>, frame: &Frame) -> Option<FrameDescription> {
        Some(FrameDescription::new(Camera::perspective().at([0.0, 1.0, 4.0])))
    }
}

fn main() -> std::process::ExitCode {
    sturdy::run(RuntimeConfig::new("My game"), Game)
}
```

## Engine surface bound so far

Each is its own `sturdy-sys/src/<name>.rs` bridge + `cpp/sturdy_rs/<name>.{hpp,cpp}` shim + a
`sturdy/src/<name>.rs` ergonomic wrapper, reached from `Engine` via `engine.ecs()`, `.assets()`,
`.rhi()`, `.ui()`, `.diagnostics()`, or directly as `sturdy::render::*` / `sturdy::reflection::*`.

Positions, rotations, scales, colors, and directions throughout the `sturdy` crate's public API are
[`glam`](https://docs.rs/glam) types (`Vec2`/`Vec3`/`Vec4`/`Quat`/`Mat4`), not raw `[f32; N]`
arrays — GLSL-shaped math (operator overloading, swizzling) instead of index juggling. `sturdy`
re-exports the crate as `sturdy::glam` so a downstream `Cargo.toml` doesn't need its own dependency
just to name the types. Built with glam's `scalar-math` feature, which keeps every type at plain
`[f32; N]` alignment (glam's default SIMD layout over-aligns `Vec4`/`Quat`/`Mat4` to 16 bytes) —
required for the ECS-component types below (`scene::WorldTransform`, ...) to stay byte-exact with
the engine's own (non-GLM-aligned) C++ structs. The `sturdy-sys` bridge layer underneath stays
plain-array-based throughout (`cxx` shared structs can't carry a foreign crate's types), with the
`sturdy` layer converting at the boundary.

- **Runtime & core** (`sturdy::{run, Engine, Input, Camera, RuntimeConfig, ...}`): launching the
  app, per-frame callbacks, keyboard/mouse, time scale, basic GPU info.
- **`ecs`**: the ECS (`World`), byte-level at the bottom (still available for dynamic/reflection
  callers) with a typed layer on top: `#[derive(Bundle)]` structs spawned in one call
  (`sturdy-macros`), a `Component` trait (`bytemuck::Pod`-based) with `register`/`get`/`set`/
  `insert`/`spawn_one`, variadic queries (`World::query::<(A, B, C, ...)>()`, up to 12-tuples), a typed `Resource` trait, and typed event channels (`Event` trait,
  `create_event_channel`/`send_event`/`read_events` — drain-on-read, see `ecs.rs`'s doc comment for
  why), and by-name reflected field access on components (`get_field`/`set_field`).
- **`schedule`**: typed, engine-scheduler-driven ECS systems (as opposed to `ecs`'s ad-hoc `World`
  access) — `add_system`/`add_global_system` with typed `Access`/`R<T>`/`W<T>` read/write
  declarations, real parallel scheduling via `Ecs::Schedule`, not a separate loop. Every system body
  also receives a `Commands` as its last argument for deferred spawn/despawn/add-component/
  remove-component (applied once the whole dispatch finishes). Global systems can also read/send
  event channels, and `Access::before`/`after` order systems relative to each other.
- **`scene`**: the ECS components that actually draw something — `WorldTransform`, `ModelRenderer`,
  `LightGizmoRenderer`, and the `DirectionalLightRenderer`/`SpotLightRenderer`/`PointLightRenderer`
  trio, matching the engine's own `SFT_ECS_COMPONENT`-registered layouts.
- **`task`/`sync`/`time`/`select!`**: a Tokio-shaped async layer — `task::spawn`/`JoinHandle`/
  `JoinError`/`yield_now`/`block_on`/`JoinSet` (including `detach_all`), `sync::{Mutex, RwLock,
  mpsc::{channel, unbounded_channel}}`, `time::{sleep, timeout, interval}`, `sturdy::select!`
  (`biased;`, per-branch `if guard`, an `else` arm) — all driven by the engine's own worker-pool
  scheduler (`SFT::Async::Scheduler`), not a separate thread pool: every future's actual poll runs
  on an engine worker, woken by a small waker that resubmits itself to that same scheduler. The one
  exception is `time`'s single dedicated timer-driver thread, which only tracks deadlines and wakes
  expired wakers — it never polls a future itself.
- **`assets`**: texture/shader/sound loading, glTF import, asset queries.
- **`render`**: render-graph settings (shadow, AO, anti-aliasing, bloom, tone-mapping, ReSTIR GI,
  motion blur) and per-frame scene ambient lighting, both attached to a frame via
  `FrameDescription`, multi-window/surfaces (creation plus runtime mutation — cursor icon/grab,
  window mode, decorated/transparent, relative-mouse/mouse-lock, IME text input, blur/vibrancy
  effects), offscreen render targets, presentation settings, HDR, and custom render-graph passes
  (`GraphStage`: your own fullscreen/compute/copy passes spliced into the standard chain).
- **`rhi`**: buffers, textures, samplers, bind groups, pipelines, command encoding, ray tracing
  (acceleration structures, ray tracing pipelines, acceleration structure copy/compaction), device
  feature/property limits, depth-bounds test, per-face stencil ops, custom MSAA sample locations,
  and CPU-driven multi-draw-indirect — the priority ops a game needs per frame, not full parity
  with the engine's internal RHI surface (see doc comments for what's deferred). A full real render
  pipeline (Slang-compiled shader, vertex buffer, two triangles, depth-bounds/stencil/blend-constant
  dynamic state, a genuine `draw_count = 2` multi-draw-indirect call) runs and checks exact pixel
  output in `examples/hello` — not just RHI resource plumbing, an actual draw.
- **`shader_compiler`**: compiles Slang source straight to SPIR-V bytes for `rhi::Rhi::
  create_shader_module`, independent of the engine's own file-based, `Asset`-producing `Assets::
  load_shader` (which has no bridge back to a raw `ShaderModuleHandle`).
- **`ui`**: a per-frame Rust closure hook into the engine's immediate-mode UI overlay (element/text
  primitives, pointer/text-input state) — not the full Clay widget tree.
- **`reflection`**: type/field/enum introspection of the engine's registered types, plus live
  access: instance and static field read/write, container element read/write/resize, method
  invocation (zero-argument), and event lookup/firing.
- **`diagnostics`**: native Vulkan/D3D12/WebGPU handle interop, native window handles
  (X11/Wayland/Win32/Cocoa), extended GPU inventory, window diagnostics, a log sink hook, and a
  minimal background-task scheduler handle.
- **`camera`**: the engine's full camera model (`Camera` value per frame, with persistent per-window
  temporal history) and `CameraRig`, a live camera for orbit/pan/frame helpers, matrices,
  project/unproject, picking rays and frustum queries.
- **`widgets`**: stateful UI widgets — button, checkbox/radio/switch, slider, text field/area,
  scroll area, dropdown, colour picker, masonry — plus easing curves; UI text works out of the box
  (the engine's default font is registered as font 0).
- **`geometry`**: Catmull-Rom splines (`Spline2`/`Spline3`, arc-length parameterization) and
  view-frustum sphere culling. `diagnostics` also has `run_on_main_thread` and CPU core ranking.
- **`text`/`text_render`**: the whole text package — fonts, shaping, layout, outlines,
  rasterization, the `Document` rope — plus GPU text: render to offscreen textures (RHI handles for
  your shaders, UI images, CPU readback), free-draw on screen, and text on quads in the world.
- **`images`**: the engine's CPU-side image pipeline as standalone functions — decode any supported
  format (animation, HDR/wide-gamut color management, previews), mip chains, BC1/3/4/5/7
  compression, ORM/metallic-roughness packing.
- **`hot_reload`**: `HotModule` (load/reload/unload a shared library that registers reflected types)
  and `FileWatcher`.

### Left for later

See `plans/unfinished.md` for the audited list. The remaining items are deliberate: `RenderGraphModule`
registration (C++ template composition with no runtime form), and modules that register reflected
types from Rust (a hot-reload module's register function takes the engine's C++ registry, so such
modules are authored in C++).

## Building

The engine source is this repository (`sturdy-sys/build.rs` uses the repository root; set `STURDY_RS_ENGINE_DIR`
to build against another checkout). Run cargo from `Rust/`.

Needs `git`, `cmake` (>= 3.28), `ninja`, `clang`/`clang++` (C++26), a libstdc++ with `libstdc++.a`, and
network access on the first build (the engine fetches its third-party sources; they are cached in
Cargo's `OUT_DIR`). The first build compiles ~2300 C++ files; later builds are incremental.

    cargo build --release -p hello

Environment knobs (see the top of `sturdy-sys/build.rs`): `STURDY_RS_TARGET_CPU` (default
`x86-64-v2`, never `native` unless you opt in — see the `native-cpu` feature below),
`STURDY_RS_BUILD_TYPE`, `STURDY_RS_CMAKE_ARGS`.

## Engine build flags, as Cargo features

An application picks its engine build configuration in its own `Cargo.toml` — no CMake, no
touching `sturdy-sys` — via features that forward to the engine's own `STURDY_*` CMake options
(mapping lives in `sturdy-sys/build.rs`'s `feature_cmake_args`, re-exported through `sturdy`'s own
`[features]`):

| feature | maps to | effect |
|---|---|---|
| `webgpu` | `STURDY_ENABLE_WEBGPU` | Builds the WebGPU (Dawn) RHI backend alongside Vulkan. Roughly doubles build time. |
| `glfw` | `STURDY_BUILD_GLFW_WINDOW_PROVIDER` | Builds the optional GLFW window provider alongside SDL3. |
| `tracy` | `STURDY_ENABLE_TRACY` | Compiles Tracy profiler instrumentation in. **Currently broken** on this toolchain — see Known limits. |
| `debug-sections` | `STURDY_ENABLE_STATIC_DEAD_STRIPPING=OFF` | Keeps debug-friendly sections instead of dead-strippable ones. |
| `native-cpu` | (build.rs `-march`) | Compiles for the build machine's exact CPU instead of the portable baseline. Faster, but the binary can `SIGILL` elsewhere. |

```toml
[dependencies]
sturdy = { path = "...", features = ["glfw"] }
```

## What ends up in the executable

Everything but the OS graphics stack: the engine, Slang, SDL3, FreeType/HarfBuzz, image codecs,
libstdc++ and the embedded shaders/fonts (the engine falls back to its embedded shaders when no
`Shaders/` directory exists). `build.rs` **fails the build** if any other system shared library
would be linked. Runtime needs only: a Vulkan driver, `libwayland-client.so.0` (Linux), and libc.

## Known limits

- **glibc floor**: a binary inherits the glibc of the machine that built it. To run on older
  distros, build inside an older-glibc container (or with `cargo zigbuild`).
- **Linux only so far**; Windows/macOS build paths are untested.
- The engine has an upstream Linux link bug (`resize_composition_swapchain_resources` has no
  non-Windows stub); `sturdy-sys/cpp/sturdy_rs/platform_fixups.cpp` supplies a weak one.
- **`tracy` feature is currently broken**: vendored Tracy 0.13.1's `tracy_concurrentqueue.h` fails
  to compile under this machine's Clang 22 once `TRACY_ENABLE` is actually defined (the client
  library itself always builds; only the instrumented path is affected). Needs either an older
  Clang or an upstream Tracy fix — not something to patch in the vendored engine tree.
- **API coverage is broad but not complete** — see "Engine surface bound so far" above and each
  module's own doc comments for exactly what's deferred per subsystem.
