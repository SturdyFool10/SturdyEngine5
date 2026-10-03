#pragma once

#include <Foundation/Foundation.hpp>

#include <array>
#include <optional>
#include <span>

namespace SFT::Physics {

    using LayerMask = u64;
    inline constexpr u32 max_layers = 64;

    /// Named collision layers. Each layer owns one bit of a 64-bit mask, and a symmetric matrix says which layers
    /// collide with which; queries and bodies carry a layer index plus the mask the matrix derives for it.
    class LayerTable {
      public:
        LayerTable();

        /// Registers `name` (idempotent) and returns its index, or nothing when all 64 are taken. A new layer collides
        /// with every layer.
        std::optional<u32> add(const ustr &name);
        [[nodiscard]] std::optional<u32> find(const ustr &name) const noexcept;
        /// Empty for an unregistered layer.
        [[nodiscard]] const UString &name(u32 layer) const noexcept;
        [[nodiscard]] u32 count() const noexcept { return count_; }

        /// `1 << layer`.
        [[nodiscard]] static constexpr LayerMask bit(u32 layer) noexcept { return layer < max_layers ? (LayerMask{1} << layer) : 0; }
        /// Union of the named layers' bits (unknown names are ignored).
        [[nodiscard]] LayerMask mask_of(std::span<const UString> names) const noexcept;
        /// Every registered layer.
        [[nodiscard]] LayerMask all() const noexcept;

        /// Symmetric: `set_collides(a, b, false)` also stops b from colliding with a.
        void set_collides(u32 a, u32 b, bool collides) noexcept;
        [[nodiscard]] bool collides(u32 a, u32 b) const noexcept;
        /// The layers `layer` collides with, ready to hand to a solver as its filter mask.
        [[nodiscard]] LayerMask collision_mask(u32 layer) const noexcept;

      private:
        std::array<UString, max_layers> names_;
        std::array<LayerMask, max_layers> masks_{};
        u32 count_ = 0;
    };

} // namespace SFT::Physics
