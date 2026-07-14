#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <utility>
#pragma endregion

using std::expected;
using std::string;
using std::unexpected;

namespace SFT::Text {

    // The Text package's own error taxonomy — deliberately separate from RHI::RhiError/
    // Core::GraphicsBackendError so this stays a standalone, GPU-independent contract. Mirrors the
    // same `expected`-based, exception-free shape used everywhere else in the engine
    // (see RHI/Error.cppm).
    enum class TextErrorCode {
        // A caller-supplied argument was structurally invalid (empty font data, an out-of-range
        // glyph id, ...) — a caller-side bug rather than a runtime condition.
        InvalidArgument,
        // Font data could not be parsed into a usable HarfBuzz face/font.
        LoadFailed,
        // HarfBuzz shaping did not produce a usable glyph run.
        ShapingFailed,
        // Outline extraction (hb-draw) or SDF/MSDF generation (msdfgen) failed for a glyph.
        RasterizationFailed,
    };

    struct TextError {
        TextErrorCode code = TextErrorCode::LoadFailed;
        string message;
    };

    // `TextResult` — a fallible Text call with no value; `TextExpected<T>` — one that yields a `T`.
    using TextResult = expected<void, TextError>;

    template <typename Value>
    using TextExpected = expected<Value, TextError>;

    [[nodiscard]] inline unexpected<TextError> text_error(TextErrorCode code, string message) {
        return unexpected(TextError{code, std::move(message)});
    }

} // namespace SFT::Text
