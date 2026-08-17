#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <utility>
#pragma endregion

using std::expected;
using std::unexpected;

namespace SFT::Text {


    enum class TextErrorCode {


        InvalidArgument,

        LoadFailed,

        ShapingFailed,

        RasterizationFailed,
    };

    struct TextError {
        TextErrorCode code = TextErrorCode::LoadFailed;
        UString message;
    };


    using TextResult = expected<void, TextError>;

    template <typename Value>
    using TextExpected = expected<Value, TextError>;

    /// Creates an error result describing the supplied text failure.
    ///
    /// @param code `code` value used by the operation.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] unexpected<TextError> text_error(TextErrorCode code, UString message);

} // namespace SFT::Text
