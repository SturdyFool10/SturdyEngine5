module;

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

export module Sturdy.RHI:Error;

import Sturdy.Foundation;

using std::expected;
using std::string;
using std::unexpected;

export namespace SFT::RHI {

    // The RHI's own error taxonomy — deliberately separate from Core's GraphicsBackendError so the RHI
    // stays a standalone contract (Core will implement the RHI, not the reverse). Mirrors the same
    // `expected`-based, exception-free shape used everywhere else in the engine.
    enum class RhiErrorCode {
        // The backend was asked for something it structurally cannot do on this device/API (a
        // missing feature, an unsupported format, a limit exceeded). Distinct from OperationFailed:
        // retrying or freeing memory won't help — the request itself is out of the device's reach.
        Unsupported,
        // A resource-creation or command-recording call failed for a reason the backend couldn't
        // classify more specifically.
        OperationFailed,
        // Ran out of device or host memory servicing the request.
        OutOfMemory,
        // The logical device was lost (TDR/reset/removal). Everything created from it is now
        // invalid and the whole device must be rebuilt.
        DeviceLost,
        // The presentation surface became invalid (window closed/resized out from under us); the
        // swapchain must be recreated before presenting again.
        SurfaceLost,
        // A handle/descriptor handed to the backend was malformed or referred to a destroyed
        // resource — a caller-side bug rather than a device condition.
        InvalidArgument,
    };

    struct RhiError {
        RhiErrorCode code = RhiErrorCode::OperationFailed;
        string message;
    };

    // `RhiResult` — a fallible RHI call with no value; `RhiExpected<T>` — one that yields a `T`.
    using RhiResult = expected<void, RhiError>;

    template <typename Value>
    using RhiExpected = expected<Value, RhiError>;

    [[nodiscard]] inline unexpected<RhiError> rhi_error(RhiErrorCode code, string message) {
        return unexpected(RhiError{code, std::move(message)});
    }

} // namespace SFT::RHI
