#include <Renderer/UI/Style.hpp>


namespace SFT::UI {

    /// Implements `operator<` for `UI`.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool operator<(const PaintKey &lhs, const PaintKey &rhs) noexcept {
        return lhs.z != rhs.z ? lhs.z < rhs.z : lhs.order < rhs.order;
    }

} // namespace SFT::UI

