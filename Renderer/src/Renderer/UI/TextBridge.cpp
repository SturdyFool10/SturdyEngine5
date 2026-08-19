#include <Renderer/UI/TextBridge.hpp>


namespace SFT::UI {

    /// Invokes the callable behavior provided by `UI`.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize TextBridge::ShapeCacheKeyHash::operator()(const ShapeCacheKey &key) const noexcept {
        usize seed = std::hash<string>{}(key.content);
        seed ^= (static_cast<usize>(key.font_id) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= (static_cast<usize>(key.font_size) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= (static_cast<usize>(key.letter_spacing) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }

} // namespace SFT::UI

