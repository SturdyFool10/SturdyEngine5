module;

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

export module Sturdy.Core:RendererError;

#pragma region Imports
import Sturdy.Foundation;
#pragma endregion

using std::expected;
using std::string;
using std::unexpected;

export namespace SFT::Core {

    enum class RendererErrorCode {
        InitializationFailed,
        DeviceLost,
        SurfaceLost,
        OutOfMemory,
        Unsupported,
        OperationFailed,
    };

    struct RendererError {
        RendererErrorCode code = RendererErrorCode::OperationFailed;
        string message;
    };

    using RendererResult = expected<void, RendererError>;

    template <typename Value>
    using RendererExpected = expected<Value, RendererError>;

    [[nodiscard]] inline unexpected<RendererError> renderer_error(RendererErrorCode code, string message) {
        return unexpected(RendererError{code, std::move(message)});
    }

} // namespace SFT::Core
