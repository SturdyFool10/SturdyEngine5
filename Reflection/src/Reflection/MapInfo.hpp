#pragma once

#include <Reflection/PrimitiveKind.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {


    /// Type-erased key/value access into an associative container living inside a non-`Trivial`
    /// field (`std::unordered_map<K, V>` in v1 — see `ContainerInfo`'s doc comment for the same
    /// "one recognized shape at a time" rationale). One `MapInfo` exists per distinct `(K, V)`
    /// pair (a process-lifetime static — see `Detail::map_info_for`), so `FieldInfo::map` is just
    /// a borrowed pointer.
    ///
    /// Unlike `ContainerInfo`, there is no index-based access — a map has no contiguous storage
    /// or stable ordering — so this is visitor- and key-based throughout.
    struct MapInfo {
        TypeId key_type{};
        usize key_size = 0;
        usize key_align = 0;
        bool key_trivial = false;
        /// See `PrimitiveKind`'s doc comment; only meaningful when `key_trivial` is set.
        PrimitiveKind key_primitive_kind = PrimitiveKind::None;

        TypeId value_type{};
        usize value_size = 0;
        usize value_align = 0;
        bool value_trivial = false;
        /// See `PrimitiveKind`'s doc comment; only meaningful when `value_trivial` is set.
        PrimitiveKind value_primitive_kind = PrimitiveKind::None;

        /// Called by `for_each` once per entry, with pointers valid only for that call.
        using VisitFn = void (*)(const void *key, const void *value, void *user_data) noexcept;

        /// Returns the map's current entry count.
        usize (*size)(const void *map) noexcept = nullptr;
        /// Removes every entry.
        void (*clear)(void *map) noexcept = nullptr;
        /// Visits every entry, in unspecified order.
        void (*for_each)(const void *map, VisitFn visitor, void *user_data) noexcept = nullptr;
        /// Inserts a new entry or overwrites an existing one with the same key.
        void (*insert_or_assign)(void *map, const void *key, const void *value) noexcept = nullptr;
        /// Placement-copies the value for `key` into `out_value`.
        bool (*find)(const void *map, const void *key, void *out_value) noexcept = nullptr;
        /// Removes the entry for `key`, if present.
        bool (*erase)(void *map, const void *key) noexcept = nullptr;
    };

    /// Returns `map`'s current entry count, or `0` if `info` is null.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline usize map_size(const MapInfo *info, const void *map) noexcept {
        return (info != nullptr && info->size != nullptr) ? info->size(map) : 0;
    }

    /// Removes every entry from `map`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool map_clear(const MapInfo *info, void *map) noexcept {
        if (info == nullptr || info->clear == nullptr) {
            return false;
        }
        info->clear(map);
        return true;
    }

    /// Inserts or overwrites the entry for `key` with `value`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool map_insert_or_assign(const MapInfo *info, void *map, const void *key, const void *value) noexcept {
        if (info == nullptr || info->insert_or_assign == nullptr) {
            return false;
        }
        info->insert_or_assign(map, key, value);
        return true;
    }

    /// Reads the value for `key` into `out_value`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null or `key` is not present.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool map_find(const MapInfo *info, const void *map, const void *key, void *out_value) noexcept {
        return (info != nullptr && info->find != nullptr) ? info->find(map, key, out_value) : false;
    }

    /// Removes the entry for `key`, if present.
    ///
    /// @return Returns `true` when an entry was removed; `false` when `info` is null or `key`
    /// was not present.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool map_erase(const MapInfo *info, void *map, const void *key) noexcept {
        return (info != nullptr && info->erase != nullptr) ? info->erase(map, key) : false;
    }


} // namespace SFT::Reflection
