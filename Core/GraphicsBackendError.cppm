module;

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

export module Sturdy.Core:GraphicsBackendError;

import Sturdy.Foundation;

using std::expected;
using std::string;
using std::unexpected;

export namespace SFT::Core {

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

    [[nodiscard]] inline unexpected<GraphicsBackendError> graphics_backend_error(GraphicsBackendErrorCode code, string message) {
        return unexpected(GraphicsBackendError{code, std::move(message)});
    }

} // namespace SFT::Core
