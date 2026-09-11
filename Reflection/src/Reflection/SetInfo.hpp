#pragma once

#include <Reflection/PrimitiveKind.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {


    /// Type-erased element-level access into `std::set<T>`/`std::unordered_set<T>` living inside a
    /// non-`Trivial` field — the key-only counterpart to `MapInfo` (no value, and no index-based
    /// access, matching `MapInfo`'s "visitor- and key-based throughout" rationale). One `SetInfo`
    /// exists per distinct element type `T` (a process-lifetime static — see `Detail::set_info_for`).
    struct SetInfo {
        TypeId element_type{};
        usize element_size = 0;
        usize element_align = 0;
        bool element_trivial = false;
        /// See `PrimitiveKind`'s doc comment; only meaningful when `element_trivial` is set.
        PrimitiveKind element_primitive_kind = PrimitiveKind::None;

        using VisitFn = void (*)(const void *element, void *user_data) noexcept;

        /// Returns the set's current element count.
        usize (*size)(const void *set) noexcept = nullptr;
        /// Removes every element.
        void (*clear)(void *set) noexcept = nullptr;
        /// Visits every element, in unspecified order.
        void (*for_each)(const void *set, VisitFn visitor, void *user_data) noexcept = nullptr;
        /// Inserts `element` (a no-op if an equal element is already present).
        void (*insert)(void *set, const void *element) noexcept = nullptr;
        /// Reports whether an element equal to `element` is present.
        bool (*contains)(const void *set, const void *element) noexcept = nullptr;
        /// Removes the element equal to `element`, if present.
        bool (*erase)(void *set, const void *element) noexcept = nullptr;
    };

    /// Returns `set`'s current element count, or `0` if `info` is null.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline usize set_size(const SetInfo *info, const void *set) noexcept {
        return (info != nullptr && info->size != nullptr) ? info->size(set) : 0;
    }

    /// Removes every element from `set`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool set_clear(const SetInfo *info, void *set) noexcept {
        if (info == nullptr || info->clear == nullptr) {
            return false;
        }
        info->clear(set);
        return true;
    }

    /// Inserts `element` into `set`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool set_insert(const SetInfo *info, void *set, const void *element) noexcept {
        if (info == nullptr || info->insert == nullptr) {
            return false;
        }
        info->insert(set, element);
        return true;
    }

    /// Reports whether `set` contains an element equal to `element`.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool set_contains(const SetInfo *info, const void *set, const void *element) noexcept {
        return (info != nullptr && info->contains != nullptr) ? info->contains(set, element) : false;
    }

    /// Removes the element equal to `element`, if present.
    ///
    /// @return Returns `true` when an element was removed; `false` when `info` is null or
    /// `element` was not present.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool set_erase(const SetInfo *info, void *set, const void *element) noexcept {
        return (info != nullptr && info->erase != nullptr) ? info->erase(set, element) : false;
    }


} // namespace SFT::Reflection
