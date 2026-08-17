#include <UI/src/UI/TextBridge.hpp>


namespace SFT::UI {

    usize TextBridge::ShapeCacheKeyHash::operator()(const ShapeCacheKey &key) const noexcept {
        usize seed = std::hash<string>{}(key.content);
        seed ^= (static_cast<usize>(key.font_id) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= (static_cast<usize>(key.font_size) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= (static_cast<usize>(key.letter_spacing) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }

} // namespace SFT::UI

