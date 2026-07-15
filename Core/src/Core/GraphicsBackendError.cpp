#include "GraphicsBackendError.hpp"

namespace SFT::Core {

unexpected<GraphicsBackendError> graphics_backend_error(GraphicsBackendErrorCode code, string message) {
        return unexpected(GraphicsBackendError{code, std::move(message)});
    }

} // namespace SFT::Core
