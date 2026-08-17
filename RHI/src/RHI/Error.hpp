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

namespace SFT::RHI {


    enum class RhiErrorCode {


        Unsupported,


        OperationFailed,

        OutOfMemory,


        DeviceLost,


        SurfaceLost,


        FullScreenExclusiveLost,


        NotReady,


        InvalidArgument,
    };

    struct RhiError {
        RhiErrorCode code = RhiErrorCode::OperationFailed;
        string message;
    };


    using RhiResult = expected<void, RhiError>;

    template <typename Value>
    using RhiExpected = expected<Value, RhiError>;

    /// Creates an error result describing the supplied RHI failure.
    ///
    /// @param code `code` value used by the operation.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] unexpected<RhiError> rhi_error(RhiErrorCode code, string message);

} // namespace SFT::RHI
