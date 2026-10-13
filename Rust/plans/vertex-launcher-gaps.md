# Vertex Launcher migration: verification report and status

Vertex (a Rust launcher, ~100k lines of UI) consumes SE5 as a git dependency. Governing rule: no setting needs a
relaunch. This file is the written verification of the gap list Vertex supplied (2026-10-11), then the status of what
was built. Method: each bullet was checked against the engine tree and `Rust/sturdy/src` by reading the files named
below, not by keyword alone, except where a row says *(search only)*, meaning the absence was established by keyword
search and the owning files were not read end to end.

Legend: **exists** (path), **partial**, **missing**. "Rust" = reachable from `Rust/sturdy`.

## Verification

### P0: live device and swapchain

| # | Item | Status |
|---|---|---|
| 1 | Switch graphics API at runtime | **partial → built this pass.** The C++ engine already rebuilt its backend in place: `Engine::apply_runtime_settings` → `Renderer::reconfigure_backend` (`Renderer/src/Renderer/RendererRecovery.cpp`) re-creates meshes, textures, offscreen targets, materials and window swapchains with handles unchanged, and `Demos/UiWorkbenchGameLogic` used it. Missing: any Rust path; typed Switched/Fallback/Failed; rollback (a failed rebuild left `initialized_ = false`, i.e. a dead window); the UI renderer release hook; a change event. Built, see below. |
| 1b | Progress/complete callback | **built** as `DeviceChangeEvent` observers (`Switching` before the old device dies, `Switched`/`Recovered`/`LostUnrecoverable` after) plus a drainable queue for Rust. The switch itself is synchronous (hundreds of ms); there is no incremental progress, only before/after. |
| 2 | Enumerate backends/adapters without creating a device | **partial → built.** `Engine::gpu_inventory()` (startup `enumerate_gpu_inventory`, headless) held name/vendor/ids/type/`physical_device_id` and per-backend driver strings; Rust exposed only a thin `Diagnostics::gpu_inventory`. Built: `available_backends()`, `available_adapters()` with stable id, per-backend driver/API version, active marker. |
| 2b | **Per-surface presentability** | **missing, still missing.** `RhiAdapter` has no "can present to this surface" query (Vulkan has `queue_family_supports_present` internally, D3D12/WebGPU have nothing equivalent exposed). Reported as `Presentability::Unknown`, `Yes` only for the running device. Real probing needs a new `RhiAdapter` virtual per backend plus a surface on that adapter's instance. |
| 2c | Stable adapter id + profile fallback when saved adapter is gone | **missing → built.** `adapter_stable_id` (FNV-1a over vendor id, device id, name; backend-independent) and `resolve_device_request`. Pure C++, CPU-tested (`EngineDeviceSwitchTest`). |
| 3 | Device lost / removed handling exposed to the app | **partial → built.** Automatic recovery already existed (`Renderer::recover_from_device_loss`, triggered from the render path on `DeviceLost`, same create info). Missing: any app-visible event and the rollback. Built: `Recovered` / `LostUnrecoverable` events. *Adapter removed while idle* (no failing call) is not detected; recovery is triggered by the next failing submit/present. |
| 4 | Transparent window + composition at runtime | **exists (partial verification).** `Core::PresentationSettings::transparent_composition` is applied live: `apply_runtime_settings` → `set_presentation_settings` → swapchain rebuilt on the next frame with `CompositeAlphaMode::Premultiplied` vs `Opaque` (`Renderer/src/Renderer/RendererLifecycle.cpp:1428`, Vulkan mapping `VulkanRhiBridgeSwapchain.cpp:317`); bound in Rust as `Engine::set_presentation_settings`. Separately `WindowRequests::set_transparent` → `Window::set_transparent` (Win32 effect at `Windows/WindowEffectsImpl.cpp:150`). **Not verified on a real compositor here** (Linux dev box, no transparent-desktop session); D3D12 `DxgiFromVisual` vs plain HWND swapchain switching was not found in `D3D12DeviceSwapchain.cpp` *(search only)*: treat as **missing on D3D12** until read. Linux X11/Wayland and macOS alpha not tested. |

### P0: window effects

`WindowEffectKind` is as the brief says. Corrections to the brief:

* **Linux blur protocol selection already exists in C++**: `WindowEffect::linux_blur_protocol` (`Automatic`, `ExtBackgroundEffect`, `KdeBlur`) with a real `ext-background-effect-v1` and `org_kde_kwin_blur` implementation (`Linux/WaylandBackgroundEffects.cpp`), including removal when disabled. **Rust cannot reach it**: `Engine::set_window_effect(surface, kind, enabled)` has no protocol parameter. X11 `_KDE_NET_WM_BLUR_BEHIND_REGION` was not found: **missing**. Blur region following resize/rounded corners: **not verified**.
* **Readback exists in C++**: `Window::enable_window_effect` returns `WindowEffectResult{Success|…, details}`. The queued `WindowRequests` path throws it away, and Rust has no readback: **partial**.
* Windows: DWMSBT constants `None`, `MainWindow`(Mica), `TransientWindow`(Acrylic), `TabbedWindow` are used internally (`Windows/WindowEffectsImpl.cpp:34`) but only Mica/Acrylic/Tabbed are selectable; no explicit *Auto/None/Transient* kinds, no legacy accent-policy/blur-behind fallback: **missing**.
* macOS: `MacOS/WindowEffectsImpl.cpp` (127 lines) has no `NSVisualEffectView` material/blending/state/emphasis: **missing**.
* `window_effect_supported(kind)` / `current_window_effect(surface)`: **missing**. Flash-free toggle and re-apply after device switch/monitor move: **missing**.

### P1: window management for a custom title bar

| Item | Status |
|---|---|
| begin interactive move / resize(edge), resize-edge cursor, hit-test regions / snap layouts | **missing** (`Window.hpp` virtuals read in full; `WindowEventKind` read in full) |
| is_maximized / is_minimized / is_fullscreen readback + events | **partial**: `fullscreen_mode()` exists; maximized/minimized readback and events **missing** |
| window icon, application id (`app_id` / `WM_CLASS`) | **missing** (not in `WindowDesc`/`WindowConfig`) |
| `set_title` cost | **exists**; not benchmarked |
| taskbar/dock progress, attention request | **missing** *(search only)* |
| system theme / accent colour query + change event | **missing** *(search only; the only "dark mode" hits are per-window DWM caption effects)* |
| monitor list with scale/refresh/HDR | **partial**: per-window `refresh_rate_hz()`, `content_scale()`, `hdr_properties()`; no monitor enumeration. `ContentScaleChanged` exists in C++ (`WindowEvent.hpp:38`); Rust exposure **not verified** |

### P1: on-demand rendering and frame limiter

* Reactive/on-demand mode, `request_redraw()`, `request_redraw_after()`: **missing** *(search only: no `request_redraw`/on-demand anywhere outside Audio)*. The loop is continuous (`Application::run_frame`).
* Frame limiter and vsync live: **exists**. `PresentationSettings::{vsync, frame_rate_limit_mode, frame_rate_limit_fps, variable_refresh}` applied through `apply_runtime_settings`/`set_presentation_settings`, bound in Rust (`render.rs:493`).

### P1: text and fonts

| Item | Status |
|---|---|
| runtime font registration | **partial**: `register_text_font` / `Ui::register_font` exist; unregister/replace **missing** *(search only)* |
| emoji/CJK families, fallback stacks | **partial**: `Text/FontFallback.cpp`, fallback-font registration (IME work); per-role families not verified |
| variable font axes | **missing**: no `fvar`/`FT_Set_Var`/`hb_font_set_variations` in `Renderer/src/Renderer/Text` |
| OpenType features per run | **exists** in `Text/Features.cpp`, `text_render::TextStyle::features`; reaching `UI::Context` text **not verified** |
| raster mode switch (alpha/SDF/MSDF/auto) | **partial**: `RasterFormat::{SDF, MSDF}` with auto size-based selection (`Text/Raster.hpp`); a live user switch not verified |
| colour emoji | **partial**: `Text/ColorGlyph.cpp` exists; UI-text path not verified |
| typography roles, rich spans/markdown widget, selectable static text, ellipsis | **missing** *(search only; markdown only in a demo)* |
| text-input parity | **partial**: IME, selection, undo, multi-line, clipboard exist (`UI/TextInput`, `TextEdit`, `DocumentTextArea`); password masking and context menu not verified |
| measured text queries before layout | **exists**: `text_render::measure` |

### P1: UI widgets

Present in `Renderer/src/Renderer/UI`: Button, ColorPicker, Dropdown, Graph, Masonry, ScrollArea, Slider, TextArea/TextInput, Toggle, Docking, custom elements, SVG (`UI/Svg`). Rust (`widgets.rs`): button, checkbox, radio, switch, slider, text field/area, scroll area, color picker, masonry, dropdown. **Missing** *(search only)*: virtualised scroll with visible range, popups/tooltips/context menus with edge clamping, modal layers, progress bar, date picker, tabs/sidebar as widgets, animated GIF/WebP playback in UI (the image decoders do decode animation, `Engine/ImageDecode`), gamepad focus traversal (`UiInput` has no gamepad focus API), **file drag-and-drop** (no drop event in any provider — verified in `WindowEventKind`), image/file-list clipboard. Per-frame CPU RGBA upload, offscreen target embedding: **partial**, not verified.

### P2: 3D skin viewer

Not verified in depth this pass. MSAA is `RenderSettings` (deferred MSAA exists), motion blur exists (`MotionBlurSettings`), BC compression exists; per-offscreen-target live MSAA and nearest-neighbour sampling selection not checked.

### P2: out-of-process items / threading

`diagnostics::run_on_main_thread` not re-verified; see `plans/threading-contract.md`.

### Packaging

`Rust/sturdy-sys/build.rs` registers `rerun-if-changed` only for `build.rs`, `src`, `cpp`, `cmake` inside its own package (good: touching Vertex sources does not re-run it). Whole-engine CMake output goes to `OUT_DIR`. Not yet verified: that the git-dependency checkout contains the full engine tree (it must, cargo checks out the whole repo), offline fetch of third-party sources, MSVC. See status below.

## Built in this pass (P0 core)

* `Engine/src/Engine/DeviceSwitch.{hpp,cpp}`: `DeviceRequest`, `DeviceSwitchResult`, `AdapterDescription`, `resolve_device_request`, `adapter_stable_id`.
* `Engine::switch_device`, `available_backends`, `available_adapters`, `device_generation`, `take_device_change_events`, `add_device_observer`.
* `Renderer::rebuild_backend_from_create_info` now restores the previous device when a replacement fails, bumps `device_generation`, and notifies observers.
* `ScreenUi` releases its GPU renderer on `Switching` and rebuilds lazily via `ensure_ready`.
* Rust: `sturdy::device` (`Engine::switch_device` etc.), bridge `sturdy-sys/src/device.rs`, shim `cpp/sturdy_rs/device.cpp`.
* Tests: `EngineDeviceSwitchTest` (CPU, resolver/enumeration), and a live check in `examples/hello` (`DeviceDemo`).

## Settings-applier migration note: `switch_device`

```rust
let r = engine.switch_device(&DeviceRequest::new()
    .backend(Backend::Vulkan).adapter(saved_stable_id).preference(DevicePreference::HighPerformance));
match r.outcome {
    SwitchOutcome::Switched => {}
    SwitchOutcome::SwitchedWithFallback(why) => toast(why),   // saved GPU gone, ran on the profile's pick
    SwitchOutcome::Failed(why) => toast(why),                  // old device still running; r.rolled_back says if it was rebuilt
}
```

Call from the frame callback. Persist `Adapter::stable_id`, not `physical_device_id`. Re-create any raw `rhi` objects
when `engine.device_generation()` changes. `ScreenUi`/`Ui` text, offscreen targets, assets and surfaces need nothing.

## Verification of the device switch, and bugs it exposed

Run live (`Rust/target/debug/hello`, nested Xwayland, RADV RX 9070, Vulkan; this machine exposes only Vulkan, so a
cross-API switch such as Vulkan→D3D12/WebGPU is **untested**; a same-API forced rebuild is what ran):
enumeration, refusal of an unsupported API (device untouched), missing-saved-adapter fallback, a forced in-place
rebuild (generation 0→1), and 60 further frames. The "frames rendered" claim is backed by the engine log having
**zero** `frame rendering failed` lines after the switch, not by the Rust check alone (the Rust frame callback runs
even when rendering fails; Rust has no render-failure signal). Rollback after a *failed replacement* is implemented
but **not exercised**: no way to make a real adapter fail creation on this machine.

The first live run found three pre-existing recovery bugs (they equally broke automatic device-loss recovery):

1. Material templates built from source released their compiler state, so recovery failed with "Cannot get bytecode
   from an empty Slang shader"; they are now recompiled from the retained source against the *new* device's shader
   target (needed anyway for cross-API switches).
2. Skinning buffers (incl. the previous-positions buffer the object-history bind group reads) were not invalidated, so
   every frame failed with "unknown buffer handle". Now forgotten and rebuilt.
3. A UI quad batch with a dead texture (an adopted texture created on the old device) failed the whole frame; it now
   drops that batch with a one-time warning. **Consequence:** adopted textures (`TextRenderer::adopt`, raw RHI
   textures) are blank after a switch until the app re-creates them: watch `DeviceChangeKind::Switched` /
   `device_generation()`.

Other renderer state that holds device handles (`restir_gi_`, `svgf_denoiser_`, displacement) was not audited beyond
what `hello`'s scene exercises; if a feature misbehaves after a switch, look at
`Renderer::invalidate_gpu_resource_handles_no_destroy` first.

## Built after the first pass: window-effect protocol selection and readback (P0 window effects, part)

* `Engine::set_window_effect_with(surface, kind, enabled, LinuxBlurProtocol, color_argb)` (Rust) /
  `WindowRequests::set_effect(window, WindowEffect)` (C++): reaches the existing Linux `Automatic` /
  `ExtBackgroundEffect` / `KdeBlur` selection and the colour effects.
* `Engine::window_effect_status(surface, kind) -> Option<WindowEffectStatus{requested_enabled, outcome: Applied|Degraded|Failed, details}>`:
  the platform's verdict, recorded on the window thread when the request runs (so read it a frame later; `None`
  until then). Before this, `Window::enable_window_effect`'s result was thrown away.
* `Engine::window_effect_supported(kind)`: per-platform upper bound (Windows all, Linux Blur only, others none). The
  real answer is the status after requesting.
* Verified live (`hello`, `[window-ext] blur verdict`): under X11 the blur request reads back `Failed` with
  "Wayland background blur requires a wl_display and wl_surface." Not verified on a Wayland compositor with blur,
  on Windows, or on macOS.
* "Set again after a device switch / monitor move": effects are window properties, not swapchain ones, so a device
  switch does not touch them (`hello` ran the switch after requesting blur; no change needed). The engine already
  re-applies recorded effects when it recreates the primary window (`Application::recreate_primary_window`). Moving
  to another monitor was not tested.

Still missing in this group: X11 `_KDE_NET_WM_BLUR_BEHIND_REGION`; Windows `Auto`/`None`/`Transient` backdrop kinds
and the legacy accent-policy fallback; macOS `NSVisualEffectView` (not attempted: Objective-C++ cannot be compiled
or run on this Linux machine, and writing it blind is how the earlier audio/physics code went wrong); a flash-free
toggle; blur region following resize/rounded corners (unverified). Also note `Linux/WindowEffectsImpl.cpp` documents
that window transparency on Linux is a creation-time `WindowConfig::transparent` flag: the swapchain's composite
alpha changes live, but the window itself must have been created transparent, so opaque→transparent at runtime on
Linux may need `recreate_window` (which keeps the surface handle semantics of `recreate_primary_window`). Unverified.

## Window effects, full pass (Linux / Windows / macOS), 2026-10-12

Interpretation of the request: Linux = `ext-background-effect-v1` first, KDE `org_kde_kwin_blur` as the fallback, and the
legacy X11 `_KDE_NET_WM_BLUR_BEHIND_REGION` property; Windows = every DWM backdrop plus the accent-policy and legacy
fallbacks; macOS = `NSVisualEffectView`. (Plasma's "Better Blur DX" is a KWin effect that blurs windows that never ask;
it needs nothing from an app, so there is nothing to implement for it. It also, like stock Blur, is what makes the KDE
protocol appear: see the caveat below.)

### What the engine can now do, by platform

| Platform | Routes | Per-effect options |
|---|---|---|
| Linux Wayland | `ext-background-effect-v1`, then `org_kde_kwin_blur` (Automatic), or either forced | blur region (up to 8 rects), corner radius (region follows window resize) |
| Linux X11 | `_KDE_NET_WM_BLUR_BEHIND_REGION` (Xlib loaded at run time, no link dependency) | same region and radius, in device pixels |
| Windows | `DWMWA_SYSTEMBACKDROP_TYPE` Auto / None / Acrylic(Transient) / Mica / Tabbed; accent policy Blur and Acrylic; legacy `DwmEnableBlurBehindWindow`; corner preference; dark mode; border/caption/text colour | accent tint (ARGB, alpha = acrylic opacity), per-side border bits, region and radius for legacy blur, corner preference |
| macOS | `NSVisualEffectView` (and window transparency) | material (14), blending mode, state, emphasized, appearance override |

New kinds (appended, Rust mirrors the order): `BackdropAuto`, `BackdropNone`, `AccentBlur`, `AccentAcrylic`,
`CornerPreference`, `VisualEffect`. Windows `Blur` is now the accent blur on Windows 10+, falling back to the legacy
DWM blur; a Windows backdrop that the OS lacks falls back to accent acrylic, then legacy blur, and says so (`Degraded`).

### Reporting what can be hit

`Engine::window_effect_support(window, kind)` (Rust `Engine::window_effect_support(surface, kind)`) returns, route by
route, `Available` / `Unavailable` (with the reason) / `Unknown`, without creating an effect. Linux Wayland reads what
the compositor advertises (including the `blur` capability bit of `ext-background-effect-v1`); Windows compares the OS
build number to when each facility shipped (backdrops 22621, accent acrylic 17134, dark mode 19041, colours/corners
22000); macOS checks the AppKit class. X11 is always `Unknown` at best: X11 cannot tell whether the window manager reads
the property. `Engine::set_window_effect_with(surface, kind, enabled, &WindowEffectParams{protocol, color_argb, options})`
requests; `window_effect_status` reads the verdict back a frame later.

### Verified

* `WindowManagerWindowEffectRegionTest` (CPU): rounded-corner region geometry (corners cut, edges kept, mirror symmetric,
  bounded rectangle count, radius clamp, area), and support aggregation. It caught a real bug (a zero-height rectangle
  when the radius equalled half the window).
* Live on a headless **KWin 6.7.5** (private D-Bus session, nested Wayland socket; the real desktop was not touched):
  support query reports `ext-background-effect-v1 Available`; the blur request with rounded corners reads back
  `Applied`. Forcing the KDE protocol correctly reads back `Failed` with the compositor's reason.
* Live under Xwayland: the X11 route sets the property and reads back `Applied`.
* Windows file: syntax-checked with clang against the mingw Windows headers (`-fsyntax-only`); never run.
* macOS file: syntax-checked against hand-written stub Objective-C headers; never compiled with a real SDK, never run.

### NOT verified: say so on the settings screen, do not assume

* **The KDE `org_kde_kwin_blur` route has never actually applied.** The test KWin did not advertise
  `org_kde_kwin_blur_manager` (a bare `--virtual` session loads no effects). It is advertised by the Blur effect on a real
  Plasma desktop, which your session has enabled, so the fallback should work there; only the "unavailable" path ran.
* The X11 property's contents were not read back (`xprop`), only that the call succeeded. The data layout (32-bit
  cardinals as `long[]` of x, y, w, h in device pixels; zero values = whole window) follows KDE's own `kwindowsystem`
  code and Ghostty's implementation, not a test against KWin on X11.
* Whether blur *visibly* happens was not looked at (no screenshots of a blurred window; the test compositor is virtual).
* Windows and macOS code paths have never executed. macOS drives AppKit through `objc_msgSend` from plain C++ (no
  Objective-C++ build setting needed), runs on the main thread (queued asynchronously from other threads, so its
  verdict is "queued", not final), and assumes arm64 (`objc_msgSend_stret` branch for x86_64 is untested).
* Wayland blur regions for rounded corners assume surface-local logical coordinates (per the protocol text).
* Linux window *transparency* is still a creation-time window flag (see the earlier note); only blur is live.
* "Blurred" window effects on Windows rely on the swapchain having alpha: that needs `transparent_composition` too, and
  the D3D12 transparent swapchain path is still unconfirmed.

## Third pass (2026-10-12): app-shell groundwork for an immediate-mode Vertex

> **Nothing in this section has been compiled or run.** It was written while the owner asked for no builds. The
> earlier sections above record what was built and run; this one is code review only. Expect compile errors on the
> first `cargo build`, and treat every "works" below as "should work". The first thing to do with it is build, then run
> `ui_idle` and `hello`, then the unit tests listed at the end.

### What Vertex can now do, and how

| Need | API | Notes |
|---|---|---|
| Idle at ~0% CPU/GPU | `Engine::set_frame_mode(FrameMode::OnDemand)`, `request_redraw()`, `request_redraw_after(d)`, `redraw_handle()` (clone to any thread) | Input, resize, close, drops and device changes redraw automatically; background workers hold a `RedrawHandle`. The window manager's poller backs off to ≤8 ms while idle. |
| UI-only window (no 3D scene) | `FrameDescription::ui_only()` | Skips the G-buffer, lighting, bloom and tone mapping. |
| Idle-CPU bug found on the way | `Async::Scheduler` workers now park after ~3 ms idle | Was 16 workers waking every 50 µs (267% CPU idle in a debug build); after the fix the `ui_idle` example measured 2.4% **(measured before this pass's later edits)**. |
| Custom title bar | `PrimaryWindow::set_hit_test_regions(HitTestRegions)` (drag + no-drag rects, resize border), `show_system_menu`, `toggle_maximized`, `is_maximized`/`is_minimized` | The OS moves/resizes (snap layouts work on Windows). `Diagnostics::window` also reports `maximized`/`minimized`/`content_scale`/`system_theme`. |
| Window icon, app id | `PrimaryWindow::set_icon(w, h, rgba)`, `RuntimeConfig::app_id("org.sturdyfool10.VertexLauncher")` | `app_id` is the Wayland `app_id` / X11 `WM_CLASS`; read once at startup. |
| Taskbar progress, attention | `set_taskbar_progress(TaskbarProgress, 0..1)`, `request_attention` | Via SDL 3.4 (`SDL_SetWindowProgressState`, `SDL_FlashWindow`); platform support varies. |
| OS theme, monitors | `PrimaryWindow::system_theme()`, `displays()` (scale, refresh, HDR, bounds) | |
| File drop (.mrpack/.zip/.vtmpack) | `Engine::take_drop_events() -> Vec<DropEvent>` (`Begin`, `Hover{position}`, `File{path}`, `Text`, `Complete`) | Drops also wake an on-demand app. |
| Frame limiter, vsync live | `PresentationSettings.frame_rate_limit: FrameRateLimit::{Unlimited, Fps(f64), MatchDisplay}` | `set_presentation_settings` no longer resets fields the binding doesn't carry. |
| Fonts live | `Engine::register_ui_font_stack(id, &FontStack)` (primary + emoji + fallbacks from a file, bytes or an installed family), `unregister_ui_font`, `system_font_families()` | Re-registering a different stack under a used id starts a new cache generation, so text re-shapes on the next frame. |
| Variable fonts | `FontStack::variation(FontVariation::weight(600.0))` (+ `width`, `slant`, `optical_size`), `font_variation_axes(&source)` | One instance per weight: register each weight under its own font id (a font instance's glyph cache is per instance). |
| OpenType features | `FontStack.features = "kern,liga,-calt,onum"` | Per font id, in UI text. |
| Text measuring, ellipsis | `Ui::measure_text`, `Ui::ellipsize(style, text, max_width)` | |
| Selectable static text | `TextInputStyle { read_only: true, .. }` | Drag-select, double-click word, Ctrl+A/C. |
| Undo/redo in text fields | engine `TextEditState` now keeps history (Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y), typing coalesced per word | |
| Rich text + links | `Ui::rich_text(.., &[RichSpan])`; `markdown::MarkdownView` (headings, lists, quotes, tables, code, strikethrough, task lists, links with click events) | Code colouring is a hook: `MarkdownStyle::code_colorizer` (Vertex already depends on `syntect`). Images show alt text; HTML is dropped. |
| Popups, tooltips, context menus, modals, tabs, progress bars | `ui_kit::{popup, context_menu, tooltip, modal, tabs, progress_bar}` | Positions are clamped to the window using last frame's bounds (one-frame lag the first time). |
| Virtualised lists | `ui_kit::visible_rows` / `visible_rows_for` + `spacer` | Fixed row height. |
| Gamepad / keyboard focus | `ui_kit::FocusNavigator` + `NavInput` (D-pad/arrows/tab → focus, A → `activated`, stick → scroll) | The engine does not read gamepads (Vertex's `gilrs` layer fills `NavInput`). |
| Images | `Assets::update_texture` (per-frame pixel updates, e.g. GIF), `ui_kit::AnimatedImage`, `Ui::image_sampled(.., ImageSampling::Nearest)`, rounded-corner clip and tint/opacity via the element | Nearest sampling adds a second sampler to the UI quad shader. |
| Skin viewer target | `Engine::resize_offscreen_target(handle, size)` | In place; the handle and its UI texture stay valid. **Still missing:** MSAA on offscreen targets, motion blur on an offscreen path, nearest-filtered target sampler. |
| Offline / shared dependency sources | `STURDY_RS_DEPS_DIR`, `STURDY_RS_OFFLINE=1`, `Rust/tools/prefetch-deps.sh` | Sources otherwise go to `OUT_DIR/deps`, one copy per cargo profile. |

### Packaging facts established by reading `sturdy-sys/build.rs` (not by building)

* Touching Vertex sources does **not** re-run the engine build: `rerun-if-changed` covers only `sturdy-sys`' own files plus
  the engine's source directories (which never change in a git-dependency checkout).
* The engine build is configured with `clang`/`clang++` and Ninja hard-coded, and the link-line mirroring was Linux-only
  (`.a`, `.so`, `pthread dl m`, a bundled `libstdc++`). This pass added a **Windows `.lib` branch, untested**; macOS
  (`-framework`, `.dylib`, `libc++`) is **not handled at all**. Windows also needs the engine itself to configure under
  clang on MSVC, which nobody has tried (`Rust/README.md` still says untested). Do not promise a Windows build before one
  has been attempted.
* Flatpak: set `STURDY_RS_DEPS_DIR` to a prefetched directory, `STURDY_RS_OFFLINE=1`, and pin the toolchain
  (clang with C++26 — `Foundation/Cxx26.hpp` rejects compilers lacking pack indexing / placeholder variables).

### Still missing after this pass

* Cross-API device switching and rollback after a failed replacement: untested (only Vulkan on the dev machine).
* Adapter presentability (per surface); D3D12 transparent swapchain; Linux opaque→transparent without recreating the
  window; macOS/Windows effects have never run.
* Per-widget semantics: virtualised list with variable row heights; date picker (Vertex does not use one); image /
  accessibility. (Image / file-list clipboard: **written, uncompiled** — `Window::{clipboard_has_mime, clipboard_data,
  set_clipboard_data}` with an SDL3 implementation, Rust `Window::{clipboard_has, clipboard_data, set_clipboard_data,
  clipboard_files, set_clipboard_files}`; the URI-list helpers are unit-tested standalone. Image bytes are whatever the
  source app offers, usually `image/png`; decode with the existing image API. GLFW provider reports no typed clipboard.)
* Per-role typography is an application-level mapping (role → `TextStyle`/`FontStack` ids); no engine type needed.
* MSAA and motion blur on offscreen targets (skin viewer parity).
* Drag-resize cursors on a custom title bar come from SDL's hit test; the engine's own `desired_cursor` is separate.
* No Rust unit tests exist for the C++-side pieces below; they are covered only by the next real run.

### Build and test status (2026-10-12, after the first full build)

Everything in this pass compiles. Rust unit tests for the new code pass: `ui_kit` (`place_near`, `clamp_to_viewport`,
`visible_rows_for`, `nearest_in_direction`), `ui::ellipsize_with`, `markdown`. C++ ctest: 60/63 pass; the three
failures are pre-existing and unrelated (`RendererUIWidgetTest` shift-click assertion — A/B verified against HEAD,
`EngineStaticAggregateUnusedTest`, `EngineKitStaticUnusedTest`). The Rust test
`path_tracing_scene_settings_reach_engine_validation` failed too; it was a real binding bug (the shim never set
`use_background_color`, so a Rust background colour was silently ignored) and is fixed, all 26 Rust tests pass. `ui_idle`, `hello` and
`shell_demo` run in a nested Xwayland and render. Still not exercised live: modal, context menu, tooltip, file drops,
title-bar hit-test drag, live font change, window-look dropdown, taskbar progress, animated images, and all
Windows/macOS paths. The `examples/ui_idle` example exercises
on-demand rendering and `request_redraw_after` (run it under a nested display, see `reference_safe_visual_capture`).
Missing and worth adding on the first build: a `hello` section for fonts (register/replace/variable weight), file drop
(inject through `Engine::inject_*`-style helper does not exist for drops), hit-test regions, and `update_texture`.

### Reference app and the immediate-mode pattern (compiled; Home/Library/Settings/About tabs verified visually)

`Rust/examples/shell_demo` is a launcher-shaped immediate-mode app and the pattern Vertex screens should follow (the way
`Demos/UiWorkbenchGameLogic` is the C++ one): one draw hook builds the whole UI each frame; state lives in plain structs
next to the widget state objects; the engine renders only on demand (`FrameMode::OnDemand` + `ui_kit::AnimationKeepAlive`
for hover fades). It exercises the custom title bar, tabs, a 50 000-row virtual list, Markdown with link events, progress
bars, tooltip, context menu, modal, file drop, taskbar progress and a Settings tab where *every* control applies live
(graphics API + GPU via `switch_device`, vsync, frame limiter, HDR, opaque/transparent/blurred window, font family, text
size, theme). Run it under a nested display after the first build; it is also the first integration test for most of the
third-pass APIs.

Also added in this stretch: `ui::set_hdr_reference_white`, `ui::set_raw_pixel_layout`, `ui_kit::{pressed, Shortcut,
AnimationKeepAlive}`, and a fix for a latent bug found by reading the shim: the UI renderer's colour format came from the
*startup* config, so a live HDR toggle left it in the wrong format; it now follows the surface's current HDR state and
rebuilds when that flips. Differences from UI Workbench worth knowing: the Rust UI is a single engine-owned context (no
per-window contexts / tear-off windows), and dock workspaces are available but not used by the demo.

## Fourth pass: a read of Vertex's own settings and dependencies (2026-10-12, written, NOT compiled)

Method: walked `../vertexlauncher/crates/config` (every `Config` field), `crates/vertexlauncher/Cargo.toml` and the
launcher/skin-viewer crates, and checked each engine-facing item against `Rust/sturdy`. Nothing here was built (the
machine was being used for something else), so the first build will likely show small errors; the pure-Rust URI-list
helpers were compiled standalone and their tests pass.

**Settings that were *not* engine work** (Vertex-side): `ui_opacity_percent` (scales theme background alpha), notification
easing/expiry, streamer/OBS modes (UI hiding only, no capture exclusion), Discord, sync, download limits,
`linux_set_opengl_driver`/`linux_use_zink_driver` (GL path; SE5 is Vulkan/D3D12/Metal, drop them), gamepad
calibration (Vertex keeps `gilrs` and feeds `ui_kit::FocusNavigator` and `Ui::add_pointer_scroll_delta`).

**New in this pass**

| Need | API | Notes |
|---|---|---|
| Parent `rfd` dialogs / `wry` webview to the engine window | `NativeWindowHandle: HasWindowHandle + HasDisplayHandle`, Cargo feature `raw-window-handle` | Win32, X11, Wayland. Cocoa reports `NotSupported` (engine stores `NSWindow*`; AppKit handles want the content `NSView*`). Compiled standalone against raw-window-handle 0.6.2. |
| Live "text rendering path" setting (Auto/Alpha/SDF/MSDF) | `Ui::set_text_raster_mode(TextRasterMode::{Auto,Sdf,Msdf})` | Per-format atlases, so it takes effect next frame. There is no plain alpha-mask atlas: map "Alpha Mask" to `Sdf`. |
| Live SVG anti-aliasing / UI-scale change without leaking textures | `Ui::clear_svg_cache()` | AA = rasterise at `target_px * supersample`, draw at `target_px`. The cache used to keep every size's texture forever. |
| Typed clipboard (images, file lists) | `Window::{clipboard_has, clipboard_data, set_clipboard_data, clipboard_files, set_clipboard_files}` | SDL3 only; GLFW reports none. |
| Engine log level / duplicate console lines with `tracing` | `diagnostics::{set_log_level, set_console_logging}` | Effective immediately from any thread. Pair with `add_log_sink` to forward engine logs into `tracing`. |
| **3D skin viewer in the UI** | `Engine::render_offscreen_view(handle, &FrameDescription)` | There was *no* way to render a 3D scene into an offscreen target from Rust (only the window frame). Per-view camera, lighting, render-graph settings (MSAA, FXAA, motion blur, ...) and stages. Scene separation by `ModelRenderer::visibility_mask` vs `Camera::culling_mask`. Synchronous, call from the frame callback / draw hook. Not run on a GPU; thread-safety against the window's render thread relies on the Renderer's own locking (the offscreen path is exercised by C++ GPU tests, the concurrent case is not). |
| Pixel-art (nearest) texture sampling on 3D models | `Assets::create_texture_sampled(..., nearest_sampling)`; `TextureAssetDesc::nearest_sampling` | Fixed at creation (sampler is part of the texture record and survives device recovery). To change live, create again, `set_model_texture`, unload the old one. |
| Failing Rust test `path_tracing_scene_settings_reach_engine_validation` | fixed earlier | Shim never set `use_background_color`. |

**Vertex skin-preview settings mapped to SE5**

| Vertex setting | SE5 | Status |
|---|---|---|
| MSAA samples 1–8 | `AntiAliasingSettings::msaa_samples` (1/2/4/8, deferred MSAA) | Per view via `render_offscreen_view`. 3, 5, 6, 7 are not valid: round to 2/4/8. |
| AA mode Off / MSAA / FXAA | `PostProcessAntiAliasing::{None, Fxaa}` + `msaa_samples` | exists |
| AA mode SMAA | `PostProcessAntiAliasing::ConservativeMorphological` | closest (a morphological filter, not SMAA proper); label it so |
| AA mode **TAA / FXAA+TAA** | none | **missing**. The engine has a temporal *upscaler* (jittered) but no temporal AA resolve. Fall back to FXAA+MSAA with a notice, or implement TAA as a stage. |
| Texel AA (`TexelBoundary`) | none | Needs a custom shader on the skin material (Slang via `load_shader_with`); the engine only provides the sampler choice above. |
| Motion blur (amount, shutter, samples) | `MotionBlurSettings` | exists (`intensity`, `shutter_angle_degrees` = shutter frames × 360, `sample_count`). Needs real motion vectors from the viewer's animation. |
| Hat/jacket thin layers (alpha mask/blend) | custom Slang material, `discard` for cutout | **Alpha-blended** thin layers have no forward transparent pass in the deferred graph: cutout works, true blending does not. |
| BC-compressed skin textures | `allow_compression` | exists |

**Still open after this pass** (unchanged unless listed): TAA; forward/transparent pass for alpha-blended geometry;
adapter presentability per surface; cross-API switch and rollback never exercised; Windows/macOS engine build and the
Cocoa `NSView` handle; per-window UI contexts; variable-height virtual lists.

### Fifth pass: closing generic gaps (2026-10-12, written, NOT compiled except where noted)

Direction from the user: no Vertex-specific features in the engine; remove anything that could block Vertex from
depending on SE5 out-of-tree and running as an **immediate-mode** application (one draw hook builds the whole UI every
frame; state in plain structs; `FrameMode::OnDemand` + `AnimationKeepAlive`). Nothing below is retained-mode state.

* **Variable-height virtual lists:** `ui_kit::VariableRows` (estimate + per-row measured heights kept by the app across
  frames; `set_len`, `measure(ui, index, element_id)`, `visible(ui, container_id, overscan)`, `offset_of`, `index_at`,
  `invalidate`). The scrollbar is approximate until rows have been seen once. The arithmetic was compiled standalone with
  a stub `Ui` and its three tests pass.
* **macOS:** `build.rs` now forwards `-framework X` pairs as `framework=X`, treats `.dylib` names, accepts `/usr/lib`
  system dylibs, and links `libc++` instead of bundling `libstdc++` (UNTESTED, no Mac). The Cocoa native handle gained
  `NativeWindowHandle::view` (the content `NSView*`, fetched through the Objective-C runtime) and the `raw-window-handle`
  adapter returns an AppKit handle from it. The raw-handle module compiles standalone against raw-window-handle 0.6.2.
* Reclassified as **not engine work** (Vertex-specific; build only if Vertex asks): TAA / FXAA+TAA, texel-boundary AA,
  alpha-blended skin layers. Everything the engine added is generic.

### What would still stop Vertex (honest list)

1. Nothing from the third pass onward has been through a full build + `ctest` on this repo state since the fourth/fifth
   passes (the third pass was). The first build will likely have small errors in: SDL3 clipboard callback lambda,
   `offscreen_render_view` (cxx aliases of the root bridge's `CameraDesc`/`SceneLightingDesc` in `render.rs`),
   `create_texture` signature changes, `diagnostics.cpp` Apple block.
2. `render_offscreen_view` has never rendered a frame; run it with a model scene and check the target's texture.
3. Windows/macOS have never built or run the engine; Cross-API device switch and rollback never exercised.

### Verification run (2026-10-12, after the user released the machine)

* `cargo build --workspace --all-targets --features sturdy/raw-window-handle`: clean, no errors (the fourth/fifth-pass
  C++/bridge code compiled first time). `cargo test --workspace`: 31 + 7 doctests pass.
* Full C++ build (`build/x86-64/linux/debug`, with the ninja dyndep workaround): no failures. `ctest -j1`: 154/155; the one
  failure is the old `RendererUIWidgetTest` shift-click assertion (A/B verified earlier against HEAD).
* **`Engine::render_offscreen_view` ran on the GPU** (new `examples/offscreen_view`, nested Xwayland): 450 renders, no
  errors, and a screenshot shows the 3D scene (sky, ground, the cube) inside a UI image. It found a real bug: the renderer
  needs a registered surface even for an offscreen target, so the shim now borrows the primary window's surface (fixed).
  The cube is dark because the example adds no light. Layer separation (viewer vs window camera) is by the documented
  masks but was not exercised with a second 3D camera; the example's window is UI-only.
* `ui_idle` still passes (9 frames in 4.4 s, 1.6% CPU).
* Still never run: clipboard (typed), nearest sampling, raster-mode switch, SVG cache clear, log controls, raw-handle
  adapter with a real dialog, `VariableRows` inside a live UI, Windows/macOS, cross-API switch/rollback.
