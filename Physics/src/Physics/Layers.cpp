#include <Physics/Layers.hpp>

namespace SFT::Physics {

    LayerTable::LayerTable() = default;

    std::optional<u32> LayerTable::add(const ustr &name) {
        if (const auto existing = find(name)) {
            return existing;
        }
        if (count_ >= max_layers) {
            return std::nullopt;
        }
        const u32 layer = count_++;
        names_[layer] = name;
        // Collide with everything registered so far, and let every earlier layer collide with this one.
        masks_[layer] = all();
        for (LayerMask &mask : Foundation::iter(masks_).take(layer)) {
            mask |= bit(layer);
        }
        return layer;
    }

    std::optional<u32> LayerTable::find(const ustr &name) const noexcept {
        for (auto &&[i, layer_name] : Foundation::iter(names_).take(count_).enumerate()) {
            if (layer_name == name) {
                return static_cast<u32>(i);
            }
        }
        return std::nullopt;
    }

    const UString &LayerTable::name(u32 layer) const noexcept {
        static const UString none;
        return layer < count_ ? names_[layer] : none;
    }

    LayerMask LayerTable::mask_of(std::span<const UString> names) const noexcept {
        LayerMask mask = 0;
        for (const UString &n : names) {
            if (const auto layer = find(n)) {
                mask |= bit(*layer);
            }
        }
        return mask;
    }

    LayerMask LayerTable::all() const noexcept {
        return count_ >= max_layers ? ~LayerMask{0} : ((LayerMask{1} << count_) - 1);
    }

    void LayerTable::set_collides(u32 a, u32 b, bool collides) noexcept {
        if (a >= count_ || b >= count_) {
            return;
        }
        if (collides) {
            masks_[a] |= bit(b);
            masks_[b] |= bit(a);
        } else {
            masks_[a] &= ~bit(b);
            masks_[b] &= ~bit(a);
        }
    }

    bool LayerTable::collides(u32 a, u32 b) const noexcept {
        return a < count_ && b < count_ && (masks_[a] & bit(b)) != 0;
    }

    LayerMask LayerTable::collision_mask(u32 layer) const noexcept {
        return layer < count_ ? masks_[layer] : 0;
    }

} // namespace SFT::Physics
