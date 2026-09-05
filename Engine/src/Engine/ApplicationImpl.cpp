#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/Async.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <Engine/Application.hpp>
#include <WindowManager/WindowManager.hpp>
#include <WindowManager/Providers/SDL3/SDL3.hpp>

#include <tracy/Tracy.hpp>

#if defined(STURDY_PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

using SFT::Foundation::f64;
using std::make_shared;
using std::make_unique;
using std::string;
using std::vector;
using std::chrono::duration;
using std::chrono::high_resolution_clock;

namespace SFT::Engine {

    /// Performs the application operation for `Engine` using the supplied arguments.
    ///
    /// @param client `client` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    Application::Application(ApplicationClient &client) noexcept : client_(&client) {}

    /// Destroys the `Engine` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    Application::~Application() {
        shutdown_client();


        for (auto &managed : windows_) {
            managed->render_thread.reset();
        }
        engine_.reset();
        Async::Scheduler::shutdown();
    }

    /// Drains render thread using the supplied arguments and current state.
    ///
    /// @return Returns the current drain render thread value.
    /// @note This function does not throw exceptions.
    void Application::drain_render_thread() noexcept {
        for (auto &managed : windows_) {
            while (!managed->in_flight_frames.empty()) {
                report_frame_result(managed->in_flight_frames.front().wait());
                managed->in_flight_frames.pop_front();
            }
        }
    }

    /// Shuts down client and releases associated runtime state.
    ///
    /// @return Returns the current shutdown client value.
    /// @note This function does not throw exceptions.
    void Application::shutdown_client() noexcept {
        if (!client_initialized_ || client_shutdown_ || engine_ == nullptr) {
            return;
        }
        client_shutdown_ = true;


        drain_render_thread();
        engine_->wait_idle();
        client_->on_shutdown(*engine_);
    }

    /// Finds managed window in the available state.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    Application::ManagedWindow *Application::find_managed_window(WindowManager::WindowId id) noexcept {
        for (auto &managed : windows_) {
            if (managed->window_id == id) {
                return managed.get();
            }
        }
        return nullptr;
    }

    /// Updates `id`'s ManagedWindow::applied_effects with `effect`, replacing any existing entry for
    /// the same WindowEffectKind rather than appending.
    ///
    /// @param id Managed window to update; a no-op if this isn't currently a managed window.
    /// @param effect Window effect to record.
    ///
    /// @note This function does not throw exceptions.
    void Application::record_applied_effect(WindowManager::WindowId id,
                                            WindowManager::WindowEffect effect) noexcept {
        ManagedWindow *managed = find_managed_window(id);
        if (managed == nullptr) {
            return;
        }
        for (WindowManager::WindowEffect &existing : managed->applied_effects) {
            if (existing.kind == effect.kind) {
                existing = effect;
                return;
            }
        }
        managed->applied_effects.push_back(effect);
    }

    /// Performs the report frame result operation for `Engine` using the supplied arguments.
    ///
    /// @param result `result` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    void Application::report_frame_result(const Core::RendererResult &result) noexcept {
        if (!result) {
            ++consecutive_render_errors_;
            if (consecutive_render_errors_ == 1 || consecutive_render_errors_ % 120 == 0) {
                const Core::GraphicsBackendError &error = result.error();
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "engine.render.frame",
                    .summary = "frame rendering failed",
                    .context = "Engine::Application::render_managed_window",
                    .cause_code = string{Core::graphics_backend_error_code_name(error.code)},
                    .cause = error.message,
                    .details = "failure occurrence " + std::to_string(consecutive_render_errors_) +
                               "; repeated failures are reported every 120 frames",
                    .help = "inspect earlier backend diagnostics; device or surface loss may require recovery",
                });
            }
        } else {
            consecutive_render_errors_ = 0;
        }
    }

    /// Spawns managed window.
    ///
    /// @param config Configuration values controlling the operation.
    /// @param factory `factory` value used by the operation.
    /// @param is_primary `is_primary` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool Application::spawn_managed_window(
        const WindowManager::WindowConfig &config,
        WindowManager::WindowFactory factory,
        bool is_primary) {
        using WindowManager::Window;
        using WindowManager::WindowError;
        using WindowManager::WindowExtent;
        using WindowManager::WindowId;
        using WindowManager::window_error_code_name;
        using SDL3Window = WindowManager::SDL3::SDL3Window;

        expected<WindowId, WindowError> id = factory != nullptr
            ? window_manager_.spawn_window(config, factory)
            : window_manager_.spawn_window<SDL3Window>(config);
        if (!id) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.window.spawn",
                .summary = "could not create the application window",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = string{window_error_code_name(id.error().code)},
                .cause = id.error().message,
                .details = {},
                .help = "check the selected window backend, display connection, and window configuration",
            });
            return false;
        }

        auto register_result = window_manager_.with_window(*id, [this, is_primary](Window &window) -> Core::RendererExpected<Core::RenderSurfaceHandle> {


            if (is_primary) {
                return engine_->initialize(window, client_->application_config().engine);
            }
            return engine_->add_window(window);
        });

        if (!register_result) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.surface.window_lost",
                .summary = "the window disappeared before graphics initialization completed",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = {},
                .cause = {},
                .details = {},
                .help = "ensure the window remains alive until its render surface is registered",
            });
        } else if (!*register_result) {
            const Core::GraphicsBackendError &error = register_result->error();
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.surface.register",
                .summary = "could not create a render surface for the application window",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = string{Core::graphics_backend_error_code_name(error.code)},
                .cause = error.message,
                .details = {},
                .help = "verify graphics feature requirements and presentation support for this display",
            });
        }
        if (!register_result || !*register_result) {
            window_manager_.destroy_window(*id);
            return false;
        }

        auto managed = std::make_unique<ManagedWindow>();
        managed->window_id = *id;
        managed->surface = **register_result;
        managed->primary = is_primary;
        managed->window_snapshot = WindowSnapshot{
            .id = *id,
            .size = config.extent,
            .framebuffer_size = config.extent,
            .position = config.position,
        };
        managed->live_resize = make_shared<LiveResizeState>();
        managed->last_frame_time = std::chrono::high_resolution_clock::now();


        if (use_render_threading_) {
            managed->render_thread = make_unique<Async::DedicatedThread>("RenderThread-" + std::to_string(static_cast<usize>(*id)));
        }
        ManagedWindow *managed_ptr = managed.get();
        windows_.push_back(std::move(managed));


        const shared_ptr<LiveResizeState> live_resize = managed_ptr->live_resize;
        window_manager_.with_window(*id, [live_resize](Window &window) -> bool {
            window.set_live_resize_callback([live_resize](WindowExtent extent) {
                live_resize->publish(extent);
            });
            return true;
        });

        return true;
    }

    /// Spawns secondary window.
    ///
    /// @param config Configuration values controlling the operation.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<Core::RenderSurfaceHandle> Application::spawn_secondary_window(
        const WindowManager::WindowConfig &config,
        WindowManager::WindowFactory factory) {
        if (!engine_ || !client_->application_config().enable_runtime_window_management) {
            return std::nullopt;
        }
        const WindowManager::WindowFactory effective_factory =
            factory != nullptr ? factory : client_->application_config().primary_window_factory;
        if (!spawn_managed_window(config, effective_factory, false)) {
            return std::nullopt;
        }

        return windows_.back()->surface;
    }

    /// Tears down the current primary window and replaces it with one spawned through `factory`,
    /// carrying the outgoing window's presentation settings (vsync/HDR/transparent-composition/...)
    /// and native window effects (blur/acrylic/transparent/...) forward onto the replacement.
    ///
    /// @param config Configuration values controlling the replacement window.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note Without the carry-forward described above, a window recreated to pick up a
    ///       graphics-API/provider change (the actual reason this function exists) would silently
    ///       drop blur/transparency/HDR and revert to plain opaque SDR.
    optional<Core::RenderSurfaceHandle> Application::recreate_primary_window(
        const WindowManager::WindowConfig &config, WindowManager::WindowFactory factory) {
        if (!engine_ || !client_->application_config().enable_runtime_window_management) {
            return std::nullopt;
        }

        ManagedWindow *old_managed = nullptr;
        for (auto &managed : windows_) {
            if (managed->primary) {
                old_managed = managed.get();
                break;
            }
        }
        if (!old_managed) {
            return std::nullopt;
        }

        // Snapshotted before spawn_managed_window() below so the replacement can carry them forward —
        // see ManagedWindow::applied_effects' own doc comment for why this is the only durable record
        // of a window's current native effects (blur/transparent/...), and Renderer::
        // presentation_settings's doc comment for why this surface's live policy is the source of
        // truth rather than, say, client_->application_config() (which is only ever the *initial*
        // config, not whatever set_presentation_settings() calls have changed it to since).
        const vector<WindowManager::WindowEffect> effects_to_restore = old_managed->applied_effects;
        const optional<Core::PresentationSettings> presentation_to_restore =
            old_managed->surface ? optional{engine_->presentation_settings(*old_managed->surface)} : std::nullopt;

        const WindowManager::WindowFactory effective_factory =
            factory != nullptr ? factory : client_->application_config().primary_window_factory;

        if (!spawn_managed_window(config, effective_factory, false)) {
            return std::nullopt;
        }

        ManagedWindow *new_managed = windows_.back().get();
        new_managed->primary = true;
        old_managed->primary = false;

        window_manager_.with_window(new_managed->window_id, [this](WindowManager::Window &window) -> bool {
            engine_->set_primary_window(window);
            return true;
        });

        // Restore the outgoing window's presentation policy and native effects onto the replacement —
        // without this, a window recreated to pick up a graphics-API/provider change (the actual
        // reason recreate_primary_window() exists) would silently drop blur/transparency/HDR and
        // revert to plain opaque SDR, even though the caller never asked for that.
        if (presentation_to_restore && new_managed->surface) {
            if (Core::RendererResult applied =
                    engine_->set_presentation_settings(*new_managed->surface, *presentation_to_restore);
                !applied.has_value()) {
                Foundation::log_warn(
                    "Engine::Application::recreate_primary_window: failed to restore presentation "
                    "settings on the replacement window: {}",
                    applied.error().message);
            }
        }
        for (const WindowManager::WindowEffect &effect : effects_to_restore) {
            window_manager_.post_to_window(new_managed->window_id, [effect](WindowManager::Window &w) {
                return w.set_effect(effect);
            });
        }
        new_managed->applied_effects = effects_to_restore;

        // Reuses request_close_window()'s own mechanism (just inlined here since it's a one-line
        // flag flip) — run()'s closing loop drains old_managed's in-flight frames and destroys it
        // exactly like any other window close, primary or not.
        old_managed->closing = true;

        return new_managed->surface;
    }

    /// Requests close window using the supplied arguments and current state.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Application::request_close_window(WindowManager::WindowId id) noexcept {
        if (ManagedWindow *managed = find_managed_window(id)) {
            managed->closing = true;
        }
    }

    /// Returns the current or globally available process window requests value.
    ///
    /// @return Returns the current process window requests value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Application::process_window_requests() {
        if (!engine_) {
            return;
        }
        for (WindowRequest &request : engine_->window_requests().drain()) {
            if (auto *spawn = std::get_if<SpawnWindowRequest>(&request)) {
                const WindowManager::WindowConfig config = spawn->window.view();
                const optional<Core::RenderSurfaceHandle> surface = spawn_secondary_window(config, spawn->factory);
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = spawn->id,
                    .kind = WindowRequestKind::Spawn,
                    .accepted = surface.has_value(),
                    .surface = surface,
                    .window = surface ? surface->window_id : WindowManager::WindowId{},
                    .message = surface ? UString{} : UString{"Runtime window management is disabled or window creation failed."},
                });
                continue;
            }

            if (auto *recreate_primary = std::get_if<RecreatePrimaryWindowRequest>(&request)) {
                const WindowManager::WindowConfig config = recreate_primary->window.view();
                const optional<Core::RenderSurfaceHandle> surface =
                    recreate_primary_window(config, recreate_primary->factory);
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = recreate_primary->id,
                    .kind = WindowRequestKind::RecreatePrimary,
                    .accepted = surface.has_value(),
                    .surface = surface,
                    .window = surface ? surface->window_id : WindowManager::WindowId{},
                    .message = surface ? UString{} : UString{"Runtime window management is disabled, there is no current primary window, or window creation failed."},
                });
                continue;
            }


            if (auto *cursor = std::get_if<SetCursorIconRequest>(&request)) {


                ManagedWindow *managed = find_managed_window(cursor->window);
                if (managed != nullptr && managed->applied_cursor_icon == cursor->icon) {
                    continue;
                }
                if (managed != nullptr) {
                    managed->applied_cursor_icon = cursor->icon;
                }
                window_manager_.post_to_window(cursor->window, [icon = cursor->icon](WindowManager::Window &w) {
                    return w.set_cursor_icon(icon);
                });
                continue;
            }

            if (auto *fullscreen = std::get_if<SetFullscreenRequest>(&request)) {
                window_manager_.post_to_window(fullscreen->window, [mode = fullscreen->mode](WindowManager::Window &w) {
                    return w.set_fullscreen(mode);
                });
                continue;
            }

            if (auto *decorated = std::get_if<SetDecoratedRequest>(&request)) {
                window_manager_.post_to_window(decorated->window, [enabled = decorated->decorated](WindowManager::Window &w) {
                    return w.set_decorated(enabled);
                });
                continue;
            }

            if (auto *transparent = std::get_if<SetTransparentRequest>(&request)) {
                record_applied_effect(transparent->window,
                                      WindowManager::WindowEffect::transparent(transparent->transparent));
                window_manager_.post_to_window(transparent->window, [enabled = transparent->transparent](WindowManager::Window &w) {
                    return w.set_transparent(enabled);
                });
                continue;
            }

            if (auto *relative_mouse = std::get_if<SetRelativeMouseModeRequest>(&request)) {
                window_manager_.post_to_window(relative_mouse->window, [enabled = relative_mouse->enabled](WindowManager::Window &w) {
                    return w.set_relative_mouse_mode(enabled);
                });
                continue;
            }

            if (auto *mouse_locked = std::get_if<SetMouseLockedRequest>(&request)) {
                window_manager_.post_to_window(mouse_locked->window, [locked = mouse_locked->locked](WindowManager::Window &w) {
                    return w.set_mouse_locked(locked);
                });
                continue;
            }

            if (auto *cursor_grabbed = std::get_if<SetCursorGrabbedRequest>(&request)) {
                window_manager_.post_to_window(cursor_grabbed->window, [grabbed = cursor_grabbed->grabbed](WindowManager::Window &w) {
                    return w.set_cursor_grabbed(grabbed);
                });
                continue;
            }

            if (auto *blur = std::get_if<SetBlurRequest>(&request)) {
                record_applied_effect(blur->window, WindowManager::WindowEffect{blur->kind, blur->enabled});
                window_manager_.post_to_window(blur->window, [kind = blur->kind, enabled = blur->enabled](WindowManager::Window &w) {
                    return w.set_effect(WindowManager::WindowEffect{kind, enabled});
                });
                continue;
            }

            if (auto *area = std::get_if<SetTextInputAreaRequest>(&request)) {
                window_manager_.post_to_window(area->window, [value = area->area](WindowManager::Window &w) {
                    return w.set_text_input_area(value);
                });
                continue;
            }

            if (auto *active = std::get_if<SetTextInputActiveRequest>(&request)) {
                window_manager_.post_to_window(active->window, [enabled = active->active](WindowManager::Window &w) {
                    return enabled ? w.start_text_input() : w.stop_text_input();
                });
                continue;
            }

            const CloseWindowRequest &close = std::get<CloseWindowRequest>(request);
            ManagedWindow *target = find_managed_window(close.window);
            if (target != nullptr && !target->pending_close_completion) {
                request_close_window(close.window);


                target->pending_close_completion = close.id;
            } else if (target != nullptr) {


                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = close.id,
                    .kind = WindowRequestKind::Close,
                    .accepted = true,
                    .window = close.window,
                    .message = {},
                });
            } else {
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = close.id,
                    .kind = WindowRequestKind::Close,
                    .accepted = false,
                    .window = close.window,
                    .message = "Managed window was not found.",
                });
            }
        }
    }

    /// Renders managed window using the current rendering state.
    ///
    /// @param managed `managed` value used by the operation.
    /// @param extent `extent` value used by the operation.
    /// @param resized `resized` value used by the operation.
    /// @param coalesce_if_backpressured `coalesce_if_backpressured` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool Application::render_managed_window(ManagedWindow &managed,
                                             WindowManager::WindowExtent extent,
                                             bool resized,
                                             bool coalesce_if_backpressured) {
        if (extent.x == 0 || extent.y == 0 || !managed.surface) {
            return false;
        }

        if (managed.render_thread && coalesce_if_backpressured) {


            while (!managed.in_flight_frames.empty() && managed.in_flight_frames.front().is_done()) {
                report_frame_result(managed.in_flight_frames.front().wait());
                managed.in_flight_frames.pop_front();
            }
            if (!managed.in_flight_frames.empty()) {
                return false;
            }
        }

        const u64 packed_extent =
            (static_cast<u64>(extent.x) << 32U) | static_cast<u64>(extent.y);
        if (resized) {


            managed.pending_resize_extent.store(packed_extent, std::memory_order_release);
        }

        constexpr f64 hitch_log_threshold_seconds = 0.1;
        const auto now = high_resolution_clock::now();
        const f64 delta_seconds = duration<f64>(now - managed.last_frame_time).count();
        managed.last_frame_time = now;
        managed.last_delta_seconds = delta_seconds;

        if (delta_seconds >= hitch_log_threshold_seconds) {
            Foundation::log_warn("Long frame detected: {}", Foundation::human_readable_time(delta_seconds));
        }

        const Core::FrameInput frame_input{
            .delta_seconds = delta_seconds,
            .frame_index = managed.frame_index,
            .framebuffer_width = extent.x,
            .framebuffer_height = extent.y,
            .live_resize = coalesce_if_backpressured,
        };
        ++managed.frame_index;


        const Core::RenderSurfaceHandle surface = *managed.surface;
        optional<RenderFrameParameters> frame_parameters =
            client_->request_render_frame(*engine_, surface, frame_input);
        if (!frame_parameters) {
            return true;
        }
        PreparedRenderFrame prepared_frame = engine_->prepare_render_frame(surface, frame_input, *frame_parameters);
        auto render_task = [this,
                            &managed,
                            surface,
                            extent,
                            packed_extent,
                            resized,
                            prepared_frame = std::move(prepared_frame)]() -> Core::RendererResult {
            if (resized) {


                u64 expected_extent = packed_extent;
                if (managed.pending_resize_extent.compare_exchange_strong(
                        expected_extent, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    engine_->on_surface_resize_needed(surface, extent);
                }
            }
            return engine_->render(prepared_frame);
        };

        if (managed.render_thread) {


            while (managed.in_flight_frames.size() >= max_frames_in_flight_) {
                report_frame_result(managed.in_flight_frames.front().wait());
                managed.in_flight_frames.pop_front();
            }
            managed.in_flight_frames.push_back(managed.render_thread->run(std::move(render_task)));
        } else {
            report_frame_result(render_task());
        }
        return true;
    }

    /// Performs the sync window state operation for `Engine` using the supplied arguments.
    ///
    /// @param window_events Window used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Application::sync_window_state(const vector<WindowManager::ManagedWindowEvents> &window_events) {
        using WindowManager::ManagedWindowEvents;
        using WindowManager::WindowEventKind;
        using WindowManager::WindowId;

        for (const ManagedWindowEvents &events : window_events) {
            ManagedWindow *managed = find_managed_window(events.window_id);
            if (managed == nullptr) {
                continue;
            }

            WindowSnapshot &snapshot = managed->window_snapshot;


            if (events.framebuffer_size && !managed->live_resize_active) {
                snapshot.framebuffer_size = *events.framebuffer_size;
            }
            for (const WindowManager::WindowEvent &event : events.events) {
                switch (event.kind) {
                    case WindowEventKind::FocusGained:
                        managed->focused = true;
                        break;
                    case WindowEventKind::FocusLost:
                        managed->focused = false;
                        break;
                    case WindowEventKind::Moved:
                        snapshot.position = event.position;
                        break;
                    case WindowEventKind::Resized:
                        snapshot.size = event.resize.current;
                        if (event.resize.framebuffer_changed) {
                            snapshot.framebuffer_size = event.resize.framebuffer;
                        }
                        break;
                    case WindowEventKind::FramebufferResized:
                        snapshot.framebuffer_size = event.resize.framebuffer;
                        break;
                    case WindowEventKind::MouseLocked:
                        snapshot.mouse_locked = true;
                        break;
                    case WindowEventKind::MouseUnlocked:
                        snapshot.mouse_locked = false;
                        break;
                    default:
                        break;
                }
            }
            snapshot.focused = managed->focused;
        }

        vector<WindowSnapshot> snapshots;
        snapshots.reserve(windows_.size());
        optional<WindowId> primary_window;
        for (const auto &managed : windows_) {
            snapshots.push_back(managed->window_snapshot);
            if (managed->primary) {
                primary_window = managed->window_id;
            }
        }

        engine_->window_state().sync(std::move(snapshots), primary_window);
    }

    /// Initializes the `Engine` for use.
    ///
    /// @return Returns the current initialize value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool Application::initialize() {
        const Foundation::Stopwatch stopwatch;
        client_initialized_ = false;
        client_shutdown_ = false;
        engine_ = make_unique<Engine>();


        const ApplicationConfig &config = client_->application_config();
        if (!spawn_managed_window(
                config.primary_window,
                config.primary_window_factory,
                true)) {
            engine_.reset();
            Async::Scheduler::shutdown();
            return false;
        }

        if (ApplicationResult consumer_initialized = client_->on_engine_initialized(*engine_);
            !consumer_initialized) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.game_logic.initialize",
                .summary = "game or application logic failed to initialize",
                .context = "Engine::Application::initialize",
                .cause_code = "engine.game_logic.failure",
                .cause = consumer_initialized.error().message.cpp_string(),
                .details = {},
                .help = "resolve the reported content, resource, or system setup failure and restart",
            });
            for (auto &managed : windows_) {
                if (managed->surface) {
                    engine_->remove_window(*managed->surface);
                    managed->surface.reset();
                }
                window_manager_.destroy_window(managed->window_id);
            }
            windows_.clear();
            engine_.reset();
            Async::Scheduler::shutdown();
            return false;
        }
        client_initialized_ = true;


        RHI::RenderThreadingCapabilities threading_caps{};
        if (Core::EngineBackend *backend = engine_->graphics_backend()) {
            threading_caps = backend->render_threading_capabilities();
        }
        use_render_threading_ = RHI::choose_render_threading_mode(threading_caps) != RHI::RenderThreadingMode::SingleThreaded;
        Foundation::log_info(
            "Application: per-window render threading {} (dedicated_thread={} parallel_recording={} platform_allows={}).",
            use_render_threading_ ? "enabled" : "disabled",
            threading_caps.backend_allows_dedicated_render_thread,
            threading_caps.backend_allows_parallel_command_recording,
            threading_caps.platform_allows_threads);
        if (use_render_threading_ && !windows_.empty()) {
            windows_.front()->render_thread =
                make_unique<Async::DedicatedThread>("RenderThread-" + std::to_string(static_cast<usize>(windows_.front()->window_id)));
        }


        max_frames_in_flight_ = engine_->capabilities().max_frames_in_flight;

        Async::Scheduler::initialize_low_latency();
        Foundation::log_info("Application initialized in {}", stopwatch.elapsed_human());
        return true;
    }

    /// Runs one iteration of the main loop body. See the declaration in Application.hpp for the
    /// full contract; this is the code shared by both the native blocking `while` loop and Web's
    /// per-requestAnimationFrame callback in Application::run() below.
    ///
    /// @return false on a fatal platform event-pump error; true otherwise.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool Application::run_frame(
        vector<WindowManager::ManagedWindowEvents> &window_events,
        std::chrono::high_resolution_clock::time_point &last_title_update,
        std::chrono::high_resolution_clock::time_point &last_tick_time) {
        using WindowManager::ManagedWindowEvents;
        using WindowManager::WindowEventKind;
        using WindowManager::WindowExtent;
        using WindowManager::window_error_code_name;

        {


            Async::pump_main_thread();

            if (auto pump = window_manager_.pump(window_events); !pump) {
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "engine.events.pump",
                    .summary = "the platform event loop failed",
                    .context = "Engine::Application::run",
                    .cause_code = string{window_error_code_name(pump.error().code)},
                    .cause = pump.error().message,
                    .details = {},
                    .help = "the application will stop cleanly; inspect the window backend state",
                });
                return false;
            }

            for (const ManagedWindowEvents &events : window_events) {
                for (const WindowManager::WindowEvent &event : events.events) {
                    engine_->queue_window_event(events.window_id, event);
                }
            }


            constexpr auto live_resize_submission_interval = std::chrono::milliseconds(16);
            constexpr auto live_resize_settle_interval = std::chrono::milliseconds(250);


            constexpr auto live_resize_retry_interval = std::chrono::milliseconds(1);
            const auto live_resize_now = high_resolution_clock::now();
            for (auto &managed_ptr : windows_) {
                ManagedWindow &managed = *managed_ptr;
                if (managed.live_resize) {
                    if (optional<WindowExtent> extent = managed.live_resize->consume()) {
                        managed.pending_live_resize = *extent;


                        managed.window_snapshot.framebuffer_size = *extent;


                        managed.live_resize_active = true;
                    }
                }

                if (managed.pending_live_resize) {


                    managed.last_frame_time = live_resize_now;
                    const bool extent_changed = !managed.submitted_live_resize ||
                        managed.submitted_live_resize->x != managed.pending_live_resize->x ||
                        managed.submitted_live_resize->y != managed.pending_live_resize->y;


                    const bool unattempted = !managed.attempted_live_resize ||
                        managed.attempted_live_resize->x != managed.pending_live_resize->x ||
                        managed.attempted_live_resize->y != managed.pending_live_resize->y;
                    if (unattempted || live_resize_now >= managed.next_live_resize_submission) {
                        managed.attempted_live_resize = managed.pending_live_resize;
                        const bool submitted = render_managed_window(
                            managed, *managed.pending_live_resize, extent_changed,
                                                          true);
                        if (submitted) {
                            managed.submitted_live_resize = managed.pending_live_resize;
                            managed.next_live_resize_submission =
                                live_resize_now + live_resize_submission_interval;
                        } else {
                            managed.next_live_resize_submission =
                                live_resize_now + live_resize_retry_interval;
                        }
                    }
                }

                if (managed.pending_live_resize || managed.live_resize_active) {
                    managed.last_frame_time = live_resize_now;
                }
            }


            for (const ManagedWindowEvents &events : window_events) {
                if (events.resized) {
                    if (ManagedWindow *managed = find_managed_window(events.window_id)) {
                        managed->pending_live_resize.reset();


                        managed->attempted_live_resize.reset();
                        managed->live_resize_active = false;
                        managed->live_resize_nonblocking_until =
                            live_resize_now + live_resize_settle_interval;
                    }
                }
            }


            sync_window_state(window_events);

            const auto tick_now = high_resolution_clock::now();
            const f64 tick_delta_seconds = duration<f64>(tick_now - last_tick_time).count();
            last_tick_time = tick_now;
            engine_->update(tick_delta_seconds);
            process_window_requests();

            for (const ManagedWindowEvents &events : window_events) {

                if (events.close_requested) {
                    if (ManagedWindow *managed = find_managed_window(events.window_id)) {
                        managed->closing = true;
                    }
                    continue;
                }

                ManagedWindow *managed = find_managed_window(events.window_id);
                if (managed == nullptr || managed->closing || !events.framebuffer_size) {
                    continue;
                }
                if (managed->live_resize_active) {


                    continue;
                }
                const bool in_live_resize_settle_interval =
                    high_resolution_clock::now() < managed->live_resize_nonblocking_until;
                const bool final_extent_already_submitted =
                    managed->submitted_live_resize &&
                    managed->submitted_live_resize->x == events.framebuffer_size->x &&
                    managed->submitted_live_resize->y == events.framebuffer_size->y;


                const bool resize_requires_notification = events.resized && !final_extent_already_submitted;
                (void)render_managed_window(
                    *managed, *events.framebuffer_size, resize_requires_notification,
                    in_live_resize_settle_interval);
                if (events.resized) {
                    managed->submitted_live_resize.reset();
                }
            }

            for (auto it = windows_.begin(); it != windows_.end();) {
                ManagedWindow &managed = **it;
                if (!managed.closing) {
                    ++it;
                    continue;
                }

                while (!managed.in_flight_frames.empty() && managed.in_flight_frames.front().is_done()) {
                    report_frame_result(managed.in_flight_frames.front().wait());
                    managed.in_flight_frames.pop_front();
                }

                if (!managed.in_flight_frames.empty()) {
                    ++it;
                    continue;
                }

                if (managed.remove_surface_task) {
                    if (!managed.remove_surface_task->is_done()) {
                        ++it;
                        continue;
                    }
                    managed.remove_surface_task->wait();
                    managed.remove_surface_task.reset();
                }

                if (managed.surface) {


                    if (windows_.size() == 1) {
                        shutdown_client();
                    }
                    const Core::RenderSurfaceHandle surface = *managed.surface;
                    managed.surface.reset();
                    if (managed.render_thread) {
                        managed.remove_surface_task = managed.render_thread->run([this, surface]() { engine_->remove_window(surface); });
                        ++it;
                        continue;
                    }
                    engine_->remove_window(surface);
                }

                const bool was_primary = managed.primary;
                window_manager_.destroy_window(managed.window_id);


                if (managed.pending_close_completion) {
                    engine_->window_requests().complete(WindowRequestCompletion{
                        .id = *managed.pending_close_completion,
                        .kind = WindowRequestKind::Close,
                        .accepted = true,
                        .window = managed.window_id,
                        .message = {},
                    });
                }
                it = windows_.erase(it);
                if (was_primary && !windows_.empty()) {
                    windows_.front()->primary = true;
                }
            }

            const auto now = high_resolution_clock::now();

            const optional<f64> title_update_interval =
                client_->application_config().primary_window_title_update_interval_seconds;
            if (title_update_interval &&
                duration<f64>(now - last_title_update).count() >= *title_update_interval) {
                ManagedWindow *primary = nullptr;
                for (const auto &managed : windows_) {
                    if (managed->primary) {
                        primary = managed.get();
                        break;
                    }
                }
                if (primary != nullptr && !primary->closing && !primary->live_resize_active &&
                    primary->last_delta_seconds > 0.0) {
                    const UString title = client_->primary_window_title(
                        *engine_,
                        ApplicationFrameStats{
                            .frame_seconds = primary->last_delta_seconds,
                            .frame_index = primary->frame_index,
                            .window_count = windows_.size(),
                        });


                    window_manager_.post_to_window(primary->window_id, [title](Window &w) -> bool {
                        if (auto result = w.set_title(title.c_str()); !result) {
                            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                                .severity = Foundation::DiagnosticSeverity::Warning,
                                .code = "engine.window.title",
                                .summary = "could not update the primary window title",
                                .context = "Engine::Application::run",
                                .cause_code = string{window_error_code_name(result.error().code)},
                                .cause = result.error().message,
                                .details = {},
                                .help = "rendering will continue; disable periodic title updates if unsupported",
                            });
                        }
                        return true;
                    });
                }
                last_title_update = now;
            }

            FrameMark;
        }
        return true;
    }

    void Application::run() {
        if (windows_.empty() || !engine_) {
            return;
        }

#if defined(STURDY_PLATFORM_WEB)
        // A blocking `while` loop never returns control to the browser's single JS thread, so the
        // page would freeze solid (no repaints, no input, and the WebGPU surface never presents).
        // emscripten_set_main_loop_arg instead schedules run_frame() to run once per
        // requestAnimationFrame and returns immediately; the trampoline below runs the same
        // teardown the native while-loop runs after it exits, once windows_ is empty or
        // run_frame() reports a fatal error.
        struct WebRunLoopState {
            Application *app;
            vector<WindowManager::ManagedWindowEvents> window_events;
            std::chrono::high_resolution_clock::time_point last_title_update;
            std::chrono::high_resolution_clock::time_point last_tick_time;
        };
        auto *state = new WebRunLoopState{
            .app = this,
            .window_events = {},
            .last_title_update = high_resolution_clock::now(),
            .last_tick_time = high_resolution_clock::now(),
        };
        emscripten_set_main_loop_arg(
            [](void *arg) {
                auto *loop_state = static_cast<WebRunLoopState *>(arg);
                Application &app = *loop_state->app;
                const bool keep_going = !app.windows_.empty() &&
                    app.run_frame(loop_state->window_events, loop_state->last_title_update, loop_state->last_tick_time);
                if (!keep_going) {
                    for (auto &managed : app.windows_) {
                        managed->surface.reset();
                    }
                    app.shutdown_client();
                    app.engine_.reset();
                    Async::Scheduler::shutdown();
                    emscripten_cancel_main_loop();
                    delete loop_state;
                }
            },
            state,
            0,   // drive from the browser's own requestAnimationFrame cadence rather than a fixed rate
            0); // simulate_infinite_loop must be 0 here, not 1: that mode fakes an infinite C loop by
                // unwinding the JS stack via an internal exception and resuming it on each rAF tick --
                // which nests a NEW resume on top of whatever the *previous* frame's suspension left
                // behind whenever the loop body itself also suspends via Asyncify (every WebGPU call
                // this backend makes through wgpuInstanceWaitAny does exactly that, see
                // WebGpuAdapter.cpp). The two suspension mechanisms compounding meant the JS call
                // stack grew by a large, unbounded increment every single frame, surfacing several
                // frames in as an unrelated-looking "Uncaught RuntimeError: index out of bounds"
                // once something else got corrupted by the runaway depth -- confirmed by symbolizing
                // the crash (a real wasm build with -g2) to a call chain of nested
                // doRewind/handleAsync/handleSleep/_emwgpuWaitAny frames, one full main-loop
                // iteration deep per repeat. With simulate_infinite_loop=0, this call registers the
                // callback and returns immediately (matching what this function already assumed --
                // run() just falls off the end right after this call either way), and Emscripten
                // invokes it via a plain, unnested rAF callback each frame instead.

        // Emscripten's runtime stays alive via the callback registered above even though run()
        // (and eventually main()) returns right here -- returning from main() does not exit a
        // Web module the way it does a native process.
#else
        auto last_title_update = high_resolution_clock::now();
        auto last_tick_time = last_title_update;
        vector<WindowManager::ManagedWindowEvents> window_events;

        while (!windows_.empty()) {
            if (!run_frame(window_events, last_title_update, last_tick_time)) {
                break;
            }
        }

        for (auto &managed : windows_) {
            managed->surface.reset();
        }
        shutdown_client();
        engine_.reset();
        Async::Scheduler::shutdown();
#endif
    }

} // namespace SFT::Engine
