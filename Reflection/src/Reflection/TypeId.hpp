#pragma once

#include <Foundation/Foundation.hpp>

#include <string_view>

namespace SFT::Reflection {


    /// A stable, name-derived identity for a reflected type, field, or method.
    ///
    /// Wraps `Foundation::Fnv1a128` rather than reimplementing FNV-1a locally, unlike
    /// `Ecs::ComponentKey` (which predates this shared utility).
    struct TypeId {
        Foundation::Fnv1a128 hash{};

        /// Converts the `TypeId` to `bool`.
        ///
        /// @return Returns `true` when this id was derived from a non-empty name.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return hash.high != 0 || hash.low != 0;
        }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(TypeId, TypeId) noexcept = default;

        /// Derives a `TypeId` from a stable, human-readable name.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @return Returns the newly constructed id.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr TypeId from_name(std::string_view name) noexcept {
            return TypeId{.hash = Foundation::Fnv1a128::from_bytes(name)};
        }
    };

    static_assert(sizeof(TypeId) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<TypeId>);
    static_assert(std::is_trivially_copyable_v<TypeId>);

    /// Hash functor for `TypeId`, for use with `std::unordered_map`/`std::unordered_set`.
    struct TypeIdHash {
        /// Invokes the callable behavior provided by `TypeIdHash`.
        ///
        /// @param id Id used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize operator()(TypeId id) const noexcept {
            return Foundation::Fnv1a128Hash{}(id.hash);
        }
    };


} // namespace SFT::Reflection
