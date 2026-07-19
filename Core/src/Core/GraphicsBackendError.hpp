#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

using std::expected;
using std::string;
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

    struct GraphicsBackendError {
        GraphicsBackendErrorCode code = GraphicsBackendErrorCode::OperationFailed;
        string message;
    };

    using RendererResult = expected<void, GraphicsBackendError>;

    template <typename Value>
    using RendererExpected = expected<Value, GraphicsBackendError>;

    [[nodiscard]] unexpected<GraphicsBackendError> graphics_backend_error(GraphicsBackendErrorCode code, string message);

} // namespace SFT::Core
