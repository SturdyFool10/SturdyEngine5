#include "Extensions.hpp"

namespace SFT::RHI {

/// Reports whether extension holds for this `RHI`.
///
/// @param supported `supported` value used by the operation.
/// @param requested `requested` value used by the operation.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
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
