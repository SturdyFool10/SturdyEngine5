#include "Extensions.hpp"

namespace SFT::RHI {

bool contains_extension(span<const ExtensionId> supported,
                                                 ExtensionId requested) noexcept {
        for (ExtensionId extension : supported) {
            if (extension_matches(extension, requested)) {
                return true;
            }
        }
        return false;
    }

} // namespace SFT::RHI
