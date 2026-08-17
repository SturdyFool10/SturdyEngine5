#pragma once

#include <Foundation/src/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/src/Async.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include "EngineModule.hpp"
#include "GameLogic.hpp"
#include "WindowState.hpp"
#include <Core/Core.hpp>
#include <Platform/Platform.hpp>

using std::atomic;
using std::optional;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace SFT::Engine {

    struct ApplicationConfig {
        Platform::Windowing::WindowConfig primary_window;


        Platform::Windowing::WindowFactory primary_window_factory = nullptr;
        EngineConfig engine;

        optional<f64> primary_window_title_update_interval_seconds;


        bool enable_runtime_window_management = false;
    };

    struct ApplicationFrameStats {
        f64 frame_seconds = 0.0;
        u64 frame_index = 0;
        usize window_count = 0;
    };


    using ApplicationError = GameLogicError;
    using ApplicationResult = GameLogicResult;


    class ApplicationClient : public GameLogic {
      public:
        /// Destroys the `ApplicationClient` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ApplicationClient() override = default;

        /// Returns the current or globally available application config value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const ApplicationConfig &application_config() const noexcept = 0;
        /// Performs the primary window title operation for `ApplicationClient` using the supplied arguments.
        ///
        /// @param engine `engine` value used by the operation.
        /// @param stats `stats` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] virtual UString primary_window_title(
            Engine &engine,
            const ApplicationFrameStats &stats) = 0;
    };


    class Application {
      public:
        /// Constructs a `Application` from the supplied initialization values.
        ///
        /// @param client `client` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Application(ApplicationClient &client) noexcept;
        /// Destroys the `Application` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Application();

        /// Initializes the `Application` for use.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool initialize();
        /// Runs the requested work.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void run();


        /// Spawns secondary window.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::RenderSurfaceHandle> spawn_secondary_window(
            const Platform::Windowing::WindowConfig &config,
            Platform::Windowing::WindowFactory factory = nullptr);


        /// Recreates primary window using the supplied arguments and current state.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<Core::RenderSurfaceHandle> recreate_primary_window(
            const Platform::Windowing::WindowConfig &config,
            Platform::Windowing::WindowFactory factory = nullptr);


        /// Requests close window using the supplied arguments and current state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void request_close_window(Platform::Windowing::WindowId id) noexcept;

      private:


        struct LiveResizeState {
            /// Performs the publish operation for `LiveResizeState` using the supplied arguments.
            ///
            /// @param extent `extent` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            void publish(Platform::Windowing::WindowExtent extent) noexcept;

            /// Returns the current or globally available consume value.
            ///
            /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
            /// @note Normal inability to produce a value is represented by an empty optional.
            /// @note This function does not throw exceptions.
            [[nodiscard]] optional<Platform::Windowing::WindowExtent> consume() noexcept;

          private:
            atomic<u64> pending_extent{0};
        };


        struct ManagedWindow {
            Platform::Windowing::WindowId window_id{};
            optional<Core::RenderSurfaceHandle> surface;
            bool primary = false;
            bool closing = false;


            bool focused = false;
            WindowSnapshot window_snapshot{};


            optional<Platform::Windowing::CursorIcon> applied_cursor_icon;


            atomic<u64> pending_resize_extent{0};
            shared_ptr<LiveResizeState> live_resize;
            optional<Platform::Windowing::WindowExtent> pending_live_resize;


            optional<Platform::Windowing::WindowExtent> submitted_live_resize;


            optional<Platform::Windowing::WindowExtent> attempted_live_resize;
            std::chrono::high_resolution_clock::time_point next_live_resize_submission{};


            std::chrono::high_resolution_clock::time_point live_resize_nonblocking_until{};
            bool live_resize_active = false;
            std::deque<Async::TaskHandle<Core::RendererResult>> in_flight_frames;
            optional<Async::TaskHandle<void>> remove_surface_task;


            optional<WindowRequestId> pending_close_completion;


            vector<Platform::Windowing::WindowEffect> applied_effects;


            unique_ptr<Async::DedicatedThread> render_thread;
            u64 frame_index = 0;
            std::chrono::high_resolution_clock::time_point last_frame_time{};
            f64 last_delta_seconds = 0.0;
        };


        /// Drains render thread using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void drain_render_thread() noexcept;


        /// Shuts down client and releases associated runtime state.
        ///
        /// @note This function does not throw exceptions.
        void shutdown_client() noexcept;

        /// Finds managed window in the available state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ManagedWindow *find_managed_window(Platform::Windowing::WindowId id) noexcept;
        /// Performs the process window requests operation for `Application` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void process_window_requests();


        /// Records applied effect using the supplied arguments and current state.
        ///
        /// @param id Identifier of the target object or resource.
        /// @param effect `effect` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void record_applied_effect(Platform::Windowing::WindowId id, Platform::Windowing::WindowEffect effect) noexcept;


        /// Spawns managed window.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        /// @param is_primary `is_primary` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool spawn_managed_window(
            const Platform::Windowing::WindowConfig &config,
            Platform::Windowing::WindowFactory factory,
            bool is_primary);


        /// Renders managed window using the current rendering state.
        ///
        /// @param managed `managed` value used by the operation.
        /// @param extent `extent` value used by the operation.
        /// @param resized `resized` value used by the operation.
        /// @param coalesce_if_backpressured `coalesce_if_backpressured` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool render_managed_window(ManagedWindow &managed,
                                                 Platform::Windowing::WindowExtent extent,
                                                 bool resized,
                                                 bool coalesce_if_backpressured = false);
        /// Performs the report frame result operation for `Application` using the supplied arguments.
        ///
        /// @param result `result` value used by the operation.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        void report_frame_result(const Core::RendererResult &result) noexcept;


        /// Performs the sync window state operation for `Application` using the supplied arguments.
        ///
        /// @param window_events Window used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void sync_window_state(const vector<Platform::Windowing::ManagedWindowEvents> &window_events);

        Platform::Windowing::WindowManager window_manager_{
            Platform::Windowing::WindowManagerPolicy{.event_pump_mode = Platform::Windowing::WindowEventPumpMode::DedicatedEventThread,
                                                     .platform_allows_threads = true}};
        vector<unique_ptr<ManagedWindow>> windows_;
        unique_ptr<Engine> engine_;
        ApplicationClient *client_ = nullptr;
        bool client_initialized_ = false;
        bool client_shutdown_ = false;


        bool use_render_threading_ = false;
        u32 max_frames_in_flight_ = 2;
        u32 consecutive_render_errors_ = 0;
    };

} // namespace SFT::Engine
