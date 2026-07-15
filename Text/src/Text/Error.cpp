#include "Error.hpp"

namespace SFT::Text {

unexpected<TextError> text_error(TextErrorCode code, string message) {
        return unexpected(TextError{code, std::move(message)});
    }

} // namespace SFT::Text
