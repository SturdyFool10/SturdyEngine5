#pragma once

#include <Reflection/PrimitiveKind.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <cstring>

namespace SFT::Reflection {


    /// Type-erased has-value-aware access into a `std::optional<T>` field. One `OptionalInfo`
    /// exists per distinct `T` (a process-lifetime static — see `Detail::optional_info_for`), so
    /// `FieldInfo::optional` is just a borrowed pointer, mirroring `ContainerInfo`/`MapInfo`.
    struct OptionalInfo {
        TypeId value_type{};
        usize value_size = 0;
        usize value_align = 0;
        bool value_trivial = false;
        /// See `PrimitiveKind`'s doc comment; only meaningful when `value_trivial` is set.
        PrimitiveKind value_primitive_kind = PrimitiveKind::None;

        /// Reports whether the optional currently holds a value.
        bool (*has_value)(const void *optional) noexcept = nullptr;
        /// Returns a read-only pointer to the contained value, or `nullptr` when empty.
        const void *(*data)(const void *optional) noexcept = nullptr;
        /// Returns a mutable pointer to the contained value, or `nullptr` when empty.
        void *(*mutable_data)(void *optional) noexcept = nullptr;
        /// Sets the optional to hold a copy of `*value`, replacing any existing value.
        void (*emplace_copy)(void *optional, const void *value) noexcept = nullptr;
        /// Clears the optional back to empty, destroying any contained value.
        void (*reset)(void *optional) noexcept = nullptr;
    };

    /// Reports whether `optional` currently holds a value.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool optional_has_value(const OptionalInfo *info, const void *optional) noexcept {
        return info != nullptr && info->has_value != nullptr && info->has_value(optional);
    }

    /// Copies the contained value out of `optional` into `out_value`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null or the optional is empty.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool optional_get(const OptionalInfo *info, const void *optional, void *out_value) noexcept {
        if (info == nullptr || info->data == nullptr) {
            return false;
        }
        const void *value = info->data(optional);
        if (value == nullptr) {
            return false;
        }
        // Trivial values are `memcpy`-safe; non-trivial ones need their own copy trampoline —
        // but OptionalInfo, unlike FieldInfo, has no separate copy_get for that, since the value
        // is already right here and safe to read directly by the caller via `optional_data`
        // instead when it needs more than a raw-byte view of a non-trivial value. `optional_get`
        // itself only promises a byte-for-byte copy, so it is only meaningful for Trivial values.
        if (!info->value_trivial) {
            return false;
        }
        std::memcpy(out_value, value, info->value_size);
        return true;
    }

    /// Sets `optional` to hold a copy of `*in_value`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool optional_set(const OptionalInfo *info, void *optional, const void *in_value) noexcept {
        if (info == nullptr || info->emplace_copy == nullptr) {
            return false;
        }
        info->emplace_copy(optional, in_value);
        return true;
    }

    /// Clears `optional` back to empty.
    ///
    /// @return Returns `true` on success; `false` when `info` is null.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool optional_reset(const OptionalInfo *info, void *optional) noexcept {
        if (info == nullptr || info->reset == nullptr) {
            return false;
        }
        info->reset(optional);
        return true;
    }


} // namespace SFT::Reflection
