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

} // namespace SFT::Core
