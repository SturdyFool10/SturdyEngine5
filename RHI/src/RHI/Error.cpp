#include "Error.hpp"

namespace SFT::RHI {

unexpected<RhiError> rhi_error(RhiErrorCode code, string message) {
        return unexpected(RhiError{code, std::move(message)});
    }

} // namespace SFT::RHI
