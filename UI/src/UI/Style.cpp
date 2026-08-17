#include <UI/src/UI/Style.hpp>


namespace SFT::UI {

    bool operator<(const PaintKey &lhs, const PaintKey &rhs) noexcept {
        return lhs.z != rhs.z ? lhs.z < rhs.z : lhs.order < rhs.order;
    }

} // namespace SFT::UI

