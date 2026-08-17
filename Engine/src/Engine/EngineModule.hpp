#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <optional>
#include <vector>
#pragma endregion

#include "AssetManager.hpp"
#include "EcsEvents.hpp"
#include "EcsRendering.hpp"
#include "EcsUi.hpp"
#include "FrameTime.hpp"
#include "InputState.hpp"
#include "RenderTarget.hpp"
#include "TimeScale.hpp"
#include "WindowRequests.hpp"
#include "WindowState.hpp"
#include <Core/Core.hpp>
#include <Ecs/src/System.hpp>
#include <Ecs/src/World.hpp>
#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>

using std::optional;
using std::vector;

namespace SFT::Engine {

    struct EngineConfig {
        RHI::BackendType graphics_backend = RHI::BackendType::Vulkan;
        string graphics_physical_device_id;
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";
        // Walked recursively for *.slang files at the start of Engine::initialize(), before the
        // graphics backend comes up. Relative paths are resolved against the current working directory.
        std::filesystem::path shaders_directory = "Shaders";
        // Persist compiled shader variants (bytecode + reflection) to disk under
        // Core::Slang::default_shader_cache_directory ("Shaders/.cache") so a later run can skip
        // recompiling them with Slang entirely on a cache hit — see Core/Slang/ShaderCache.hpp and
        // ShaderVariantCache's disk-cache constructor parameters.
        bool enable_shader_disk_cache = true;
    };

    // The glue layer. Owns the high-level Renderer and binds it to Platform windows. The Renderer
    // owns backend lifetimes/synchronization/resource records while still exposing low-level escape
    // hatches for Core backend and RHI access. Future subsystems (audio, physics, scene) hang here.
    class Engine {
      public:
        Engine();
        ~Engine();

        Engine(const Engine &) = delete;
        Engine &operator=(const Engine &) = delete;

        // Bring the renderer up for the first window. The backend constructs and owns the initial
        // render surface, returning an opaque handle for render calls and resize notifications.
        Core::RendererExpected<Core::RenderSurfaceHandle> initialize(Platform::Windowing::Window &window,
                                                                     const EngineConfig &config = {});

        // Adds another window to an already-initialized engine. The backend stores this window's
        // resources (surface, swapchain, ...) keyed by its WindowId, alongside every other window.
        Core::RendererExpected<Core::RenderSurfaceHandle> add_window(Platform::Windowing::Window &window,
                                                                     u32 desired_frames_in_flight = 2);

        // Destroys one window's backend-owned resources. Call when a window is closed.
        void remove_window(Core::RenderSurfaceHandle surface) noexcept;

        // Persistent, absolute-size offscreen output owned by Renderer. The Engine handle remains a
        // stable opaque identity across graph and prepared-frame copies; zero is never returned.
        [[nodiscard]] Core::RendererExpected<RenderTargetHandle> create_offscreen_render_target(
            const OffscreenRenderTargetDescription &description);
        void destroy_offscreen_render_target(RenderTargetHandle target) noexcept;
        // Query this before freezing camera projection or UI layout. In particular, pass width/height
        // to Camera::set_viewport_size() and UiContext::begin_layout() for an off-screen frame.
        [[nodiscard]] optional<OffscreenRenderTargetDescription> offscreen_render_target_description(
            RenderTargetHandle target) const;

        // Sampling bridge for materials/UI that consume Renderer texture handles. The returned handle
        // is borrowed from the live offscreen target and becomes invalid when that target is destroyed.
        [[nodiscard]] SFT::Renderer::TextureHandle offscreen_render_target_texture(
            RenderTargetHandle target) const noexcept;

        // Pairs with Platform::Windowing::Window::recreate(): once the old window has been
        // destroyed and its replacement constructed, call this to retire the old window's
        // backend resources (which are keyed by its now-gone WindowId) and stand up fresh ones
        // for the new window. `old_surface` must not be used again after this call.
        Core::RendererExpected<Core::RenderSurfaceHandle> recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                          Platform::Windowing::Window &new_window,
                                                                          u32 desired_frames_in_flight = 2);

        // Forward a resize-needed notification to the backend. Call on every window resize event,
        // including resize-to-zero (minimized), with the already-resolved framebuffer extent — the
        // backend must not query the window itself (this can be called from a render thread; see
        // EngineBackend::on_surface_resize_needed). The backend rebuilds the swapchain lazily.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;

        // Runtime presentation policy for a surface: apps can expose this as vsync / low-latency / tearing
        // options. Changing it marks the swapchain dirty and applies on the next rendered frame.
        Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                       const Core::PresentationSettings &settings);

        [[nodiscard]] Core::RendererExpected<Core::RuntimeSettingsChangeResult>
        apply_runtime_settings(Core::RenderSurfaceHandle primary_surface,
                               const EngineConfig &settings);

        // Real HDR capability of whichever display `surface`'s window is currently on — peak/min
        // nits, supported transfer functions (PQ/HLG/scRGB), gamut, platform-reported metadata when
        // available. Call this before offering an HDR toggle in a settings UI: a multi-window app
        // must query per-window, since two windows can be on two different displays with different
        // (or no) HDR support — never assume one window's answer applies to another.
        [[nodiscard]] RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            Core::RenderSurfaceHandle surface) const;

        // See RHI::HdrContentLightLevelUpdate's own doc comment (RHI/HdrDisplay.hpp): a manual,
        // caller-supplied per-scene HDR metadata refresh, not real HDR10+/ST 2094-40 (this engine
        // doesn't analyze scene luminance itself). Returns Unsupported for a window not currently
        // presenting Hdr10St2084.
        [[nodiscard]] RHI::RhiResult update_hdr_content_light_level(
            Core::RenderSurfaceHandle surface,
            const RHI::HdrContentLightLevelUpdate &update);

        // See Renderer::presentation_resolution's own doc comment (RendererModule.hpp) — the
        // requested-vs-effective present mode this surface's swapchain actually resolved to, for a
        // caller (Application's per-window adaptive frame pacing) to tell vsync-paced windows from
        // uncapped ones.
        [[nodiscard]] RHI::PresentationResolution presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept;

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        // Runs the ECS render-extraction schedule on the coordinating caller thread and returns an
        // immutable CPU snapshot. Queue this snapshot to the render thread; never hand renderer/RHI
        // objects to ECS workers directly.
        [[nodiscard]] PreparedRenderFrame prepare_render_frame(Core::RenderSurfaceHandle surface,
                                                               const Core::FrameInput &frame,
                                                               const RenderFrameParameters &parameters = {});
        Core::RendererResult render(const PreparedRenderFrame &frame);

        // Publishes queued platform events into typed ECS event streams and runs gameplay/application
        // systems. Application calls this once per main-loop iteration, after pumping all windows and
        // before requesting frames, passing a delta independent of any one window's render cadence.
        // Advances FrameTime (Ecs::ReadResource<FrameTime>) before update_schedule_ runs.
        void update(f64 delta_seconds);
        void queue_window_event(Platform::Windowing::WindowId window,
                                const Platform::Windowing::WindowEvent &event);

        // Consumer extension points. Bind RenderFrameRequests as a World resource, then add
        // read-oriented extraction systems that submit into it.
        [[nodiscard]] Ecs::World &ecs_world() noexcept;
        [[nodiscard]] const Ecs::World &ecs_world() const noexcept;
        [[nodiscard]] Ecs::Schedule &update_schedule() noexcept;
        [[nodiscard]] Ecs::Schedule &render_extraction_schedule() noexcept;
        [[nodiscard]] RenderFrameRequests &render_frame_requests() noexcept;
        [[nodiscard]] LightFrameRequests &light_frame_requests() noexcept;
        [[nodiscard]] AssetManager &assets() noexcept;
        [[nodiscard]] const AssetManager &assets() const noexcept;

        // Read side of "what do the managed windows currently look like" for ECS consumers
        // (Ecs::ReadResource<WindowState>). Application calls WindowState::sync() once per tick,
        // before Application::run()'s per-window render loop — see WindowState.hpp.
        [[nodiscard]] WindowState &window_state() noexcept;
        [[nodiscard]] const WindowState &window_state() const noexcept;

        // Accumulated per-tick input state for ECS consumers (Ecs::ReadResource<InputState>) — kept
        // current automatically by a built-in system registered in Engine's own constructor, the same
        // way ui_pointer_state() below is. See InputState.hpp for the key_down()-vs-text_this_tick()
        // distinction.
        [[nodiscard]] InputState &input_state() noexcept;
        [[nodiscard]] const InputState &input_state() const noexcept;

        // Deferred host-control requests for spawning/closing managed OS windows. Application owns
        // execution; Engine/GameLogic/ECS own submission and completion consumption.
        [[nodiscard]] WindowRequests &window_requests() noexcept;

        // Window-independent delta/tick counter (Ecs::ReadResource<FrameTime>), advanced once per
        // update() call — see FrameTime.hpp for why this is separate from per-window Core::FrameInput.
        [[nodiscard]] const FrameTime &frame_time() const noexcept;

        // Time dilation applied to FrameTime::delta_seconds() (not unscaled_delta_seconds()) on the
        // next update() call — see TimeScale.hpp. Also reachable as Ecs::WriteResource<TimeScale> from
        // any system (e.g. a pause-menu or slow-motion system); this accessor exists for callers
        // outside the schedule, like a debug UI slider or an editor's own controls.
        [[nodiscard]] TimeScale &time_scale() noexcept;
        [[nodiscard]] const TimeScale &time_scale() const noexcept;

        // ECS-visible UI resources (Ecs::WriteResource<UiContext>/Ecs::ReadResource<UiPointerState>)
        // — see EcsUi.hpp. ui_pointer_state() is kept current automatically (a built-in system
        // registered in Engine's own constructor); ui_context() is a bare Context/UiRenderer pair
        // any system can build a tree against or query hovered()/clicked() on. The non-const
        // ui_pointer_state() overload exists for UiContext::begin_layout() callers (see its own
        // doc comment) — it needs to write back the consumed flag and drain the scroll accumulator.
        [[nodiscard]] UiContext &ui_context() noexcept;
        [[nodiscard]] UiPointerState &ui_pointer_state() noexcept;
        [[nodiscard]] const UiPointerState &ui_pointer_state() const noexcept;
        // Same "kept current by a built-in system" contract as ui_pointer_state() above, but for
        // keyboard/text/IME input — see UiTextInputState's own doc comment (EcsUi.hpp).
        [[nodiscard]] UiTextInputState &ui_text_input_state() noexcept;
        [[nodiscard]] const UiTextInputState &ui_text_input_state() const noexcept;
        // Path->TextureHandle cache backing a UI::Context::image() call built from a file path — see
        // UiImageCache's own doc comment for why this exists (avoids re-decoding on every frame).
        [[nodiscard]] UiImageCache &ui_image_cache() noexcept;
        // Path->rasterized-RGBA8-texture cache backing a UI::Context::svg() call — see UiSvgCache's
        // own doc comment.
        [[nodiscard]] UiSvgCache &ui_svg_cache() noexcept;

        // The window passed to initialize() — nullptr before that call. Exposed mainly for
        // Platform::Windowing::Window::clipboard_text()/set_clipboard_text() (clipboard is
        // process-global, not per-window, so any window works; the primary one is simplest to
        // reach), which text-editing UI widgets need and nothing else in Engine's own public
        // surface currently reaches.
        [[nodiscard]] Platform::Windowing::Window *primary_window() noexcept;

        // Repoints the primary-window pointer without going through initialize() — for
        // Application::recreate_primary_window() after it has spawned a replacement OS window and
        // wants Engine's own notion of "the primary window" (what primary_window() above, and
        // therefore apply_runtime_settings()'s Vulkan-WSI-extension query, dereferences) to follow
        // the swap instead of dangling once the old window is torn down.
        void set_primary_window(Platform::Windowing::Window &window) noexcept;

        [[nodiscard]] const EngineConfig &config() const noexcept;
        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept;
        [[nodiscard]] SFT::Renderer::Renderer *renderer() noexcept;
        [[nodiscard]] const SFT::Renderer::Renderer *renderer() const noexcept;
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept;
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept;

        // Backend-agnostic info about the GPU in use (name, vendor, driver version, ...). Returns
        // nullopt until a successful initialize() has selected a physical device.
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;

        // Startup snapshot of every physical GPU and the APIs that can enumerate it. This remains
        // available after renderer selection so settings/UI can explain the chosen fallback.
        [[nodiscard]] const RHI::GpuInventory &gpu_inventory() const noexcept;

        // Shaders discovered and reflected from EngineConfig::shaders_directory during initialize(),
        // before the graphics backend came up. Each one is lazily compiled: target bytecode for an
        // entry point is only generated the first time something asks for it.
        [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &shaders() const noexcept;

        // Block until all in-flight GPU work is complete. The destructor calls this automatically;
        // also useful as an explicit sync point before controlled shutdown or resource reloads.
        void wait_idle() noexcept;

      private:
        SFT::Renderer::Renderer renderer_;
        RHI::GpuInventory gpu_inventory_{};
        AssetManager assets_{renderer_};
        Ecs::ComponentRegistry ecs_component_registry_;
        RenderFrameRequests render_frame_requests_{assets_};
        LightFrameRequests light_frame_requests_{};
        Ecs::World ecs_world_{ecs_component_registry_};
        PlatformEventInbox platform_event_inbox_{};
        Ecs::Events<WindowEvent> window_events_{};
        Ecs::Events<KeyboardEvent> keyboard_events_{};
        Ecs::Events<TextInputEvent> text_input_events_{};
        Ecs::Events<TextEditingEvent> text_editing_events_{};
        Ecs::Events<MouseMoveEvent> mouse_move_events_{};
        Ecs::Events<MouseButtonEvent> mouse_button_events_{};
        Ecs::Events<MouseWheelEvent> mouse_wheel_events_{};
        Ecs::Events<WindowStateEvent> window_state_events_{};
        WindowState window_state_{};
        InputState input_state_{};
        WindowRequests window_requests_{};
        FrameTime frame_time_{};
        TimeScale time_scale_{};
        UiPointerState ui_pointer_state_{};
        UiTextInputState ui_text_input_state_{};
        UiContext ui_context_{};
        UiImageCache ui_image_cache_{};
        UiSvgCache ui_svg_cache_{};
        Ecs::Schedule update_schedule_;
        Ecs::Schedule render_extraction_schedule_{Ecs::ScheduleConfig{.clear_events_on_run = false}};
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        EngineConfig config_{};
        Platform::Windowing::Window *primary_window_ = nullptr;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
