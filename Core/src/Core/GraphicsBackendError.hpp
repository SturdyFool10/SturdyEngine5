#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#pragma endregion

using std::expected;
using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::Core {

    enum class GraphicsBackendErrorCode {
        InitializationFailed,
        DeviceLost,
        SurfaceLost,
        // The presentation engine revoked exclusive-fullscreen ownership it had previously granted
        // (VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) — a normal, recoverable state transition
        // (another app took focus, the user alt-tabbed, ...), not a device/surface failure. The
        // swapchain itself is still valid; callers should fall back to windowed/borderless
        // presentation and mark the swapchain dirty for rebuild rather than hard-failing.
        FullScreenExclusiveLost,
        OutOfMemory,
        Unsupported,
        OperationFailed,
    };

    [[nodiscard]] constexpr string_view graphics_backend_error_code_name(
        GraphicsBackendErrorCode code) noexcept {
        switch (code) {
            case GraphicsBackendErrorCode::InitializationFailed: return "core.graphics.initialization_failed";
            case GraphicsBackendErrorCode::DeviceLost: return "core.graphics.device_lost";
            case GraphicsBackendErrorCode::SurfaceLost: return "core.graphics.surface_lost";
            case GraphicsBackendErrorCode::FullScreenExclusiveLost: return "core.graphics.full_screen_exclusive_lost";
            case GraphicsBackendErrorCode::OutOfMemory: return "core.graphics.out_of_memory";
            case GraphicsBackendErrorCode::Unsupported: return "core.graphics.unsupported";
            case GraphicsBackendErrorCode::OperationFailed: return "core.graphics.operation_failed";
        }
        return "core.graphics.unknown";
    }

    struct GraphicsBackendError {
        GraphicsBackendErrorCode code = GraphicsBackendErrorCode::OperationFailed;
        string message;
    };

    using RendererResult = expected<void, GraphicsBackendError>;

    template <typename Value>
    using RendererExpected = expected<Value, GraphicsBackendError>;

    [[nodiscard]] unexpected<GraphicsBackendError> graphics_backend_error(GraphicsBackendErrorCode code, string message);

    // Distinguishes the non-error outcomes a successful vkQueuePresentKHR call can still report.
    // Suboptimal and OutOfDate both presented (the driver honored the call) but differ in urgency:
    // Suboptimal means the swapchain still works and can keep presenting, OutOfDate means it should
    // stop being used for new acquisitions until rebuilt. Real errors (device/surface/fullscreen-
    // exclusive loss) are reported through GraphicsBackendError, not this enum.
    enum class PresentOutcome {
        Success,
        Suboptimal,
        OutOfDate,
    };

} // namespace SFT::Core
