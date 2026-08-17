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


        FullScreenExclusiveLost,
        OutOfMemory,
        Unsupported,
        OperationFailed,
    };

    /// Returns a human-readable name for the supplied graphics backend error code value.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
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

    /// Creates an error result describing the supplied graphics backend failure.
    ///
    /// @param code `code` value used by the operation.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] unexpected<GraphicsBackendError> graphics_backend_error(GraphicsBackendErrorCode code, string message);


    enum class PresentOutcome {
        Success,
        Suboptimal,
        OutOfDate,
    };

} // namespace SFT::Core
