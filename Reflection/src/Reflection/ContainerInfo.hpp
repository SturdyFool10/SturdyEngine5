#pragma once

#include <Reflection/PrimitiveKind.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {


    /// Type-erased element-level access into a container living inside a non-`Trivial` field —
    /// what lets a serializer (or a mod) walk a `std::vector<T>` field element-by-element instead
    /// of treating it as an opaque blob. One `ContainerInfo` exists per element type `T` (a
    /// process-lifetime static, never allocated per-field — see `Detail::container_info_for`),
    /// so `FieldInfo::container` is just a borrowed pointer, `nullptr` for non-container fields.
    ///
    /// v1 supports `std::vector<T>` only (the container actually used by gameplay code in this
    /// engine); other containers (`std::array`, `std::deque`, associative containers, ...) are a
    /// documented, separate piece of future work, not silently mishandled — a field of one of
    /// those still falls through to the existing "opaque non-trivial type" path (clean
    /// `copy_get`/`copy_set` if provided, or a clean `SerializeErrorCode::UnsupportedFieldType`).
    struct ContainerInfo {
        /// Identifies the element type (see `type_id_for`).
        TypeId element_type{};
        usize element_size = 0;
        usize element_align = 0;
        /// Set when the element type is trivially copyable, so callers can `memcpy` an element
        /// directly instead of going through `get_element`/`set_element`.
        bool element_trivial = false;
        /// See `PrimitiveKind`'s doc comment; only meaningful when `element_trivial` is set.
        PrimitiveKind element_primitive_kind = PrimitiveKind::None;
        /// Set for a fixed-capacity container (`std::array<T, N>`) — `resize` is a real,
        /// non-null function pointer for these too (so serialization code doesn't need a special
        /// case), but it is only ever actually called when `new_size` already equals the
        /// container's one true size; `container_resize` enforces that and fails closed
        /// otherwise, since a `std::array` can never grow or shrink.
        bool fixed_size = false;

        /// Returns the container's current element count.
        usize (*size)(const void *container) noexcept = nullptr;
        /// Placement-copies the element at `index` into `out_value` (`element_size` bytes,
        /// aligned to `element_align`). `index` must be `< size(container)`.
        void (*get_element)(const void *container, usize index, void *out_value) noexcept = nullptr;
        /// Assigns `in_value` into the element at `index`. `index` must be `< size(container)`.
        void (*set_element)(void *container, usize index, const void *in_value) noexcept = nullptr;
        /// Resizes the container to exactly `new_size` elements (default-constructing new ones,
        /// destroying truncated ones) — null when the element type has no default constructor.
        void (*resize)(void *container, usize new_size) noexcept = nullptr;
        /// Returns a read-only pointer to the container's contiguous element storage (`size()`
        /// elements, `element_size` bytes apart) — lets `element_trivial` containers be
        /// serialized with one bulk `memcpy` instead of a per-element `get_element` call.
        const void *(*data)(const void *container) noexcept = nullptr;
        /// Mutable counterpart to `data`, for writing a `resize`d `element_trivial` container's
        /// elements back in one bulk `memcpy` during deserialization.
        void *(*mutable_data)(void *container) noexcept = nullptr;
    };

    /// Returns `container`'s element count, or `0` if `info` is null (a field with no
    /// `ContainerInfo` isn't a recognized container at all).
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline usize container_size(const ContainerInfo *info, const void *container) noexcept {
        return (info != nullptr && info->size != nullptr) ? info->size(container) : 0;
    }

    /// Reads the element at `index` out of `container` into `out_value`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null, `index` is out of range,
    /// or the container has no readable element representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool container_get_element(const ContainerInfo *info, const void *container, usize index, void *out_value) noexcept {
        if (info == nullptr || info->get_element == nullptr || index >= container_size(info, container)) {
            return false;
        }
        info->get_element(container, index, out_value);
        return true;
    }

    /// Writes `in_value` into the element at `index` of `container`.
    ///
    /// @return Returns `true` on success; `false` when `info` is null, `index` is out of range,
    /// or the container has no writable element representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool container_set_element(const ContainerInfo *info, void *container, usize index, const void *in_value) noexcept {
        if (info == nullptr || info->set_element == nullptr || index >= container_size(info, container)) {
            return false;
        }
        info->set_element(container, index, in_value);
        return true;
    }

    /// Resizes `container` to exactly `new_size` elements.
    ///
    /// @return Returns `true` on success; `false` when `info` is null or the element type has no
    /// default constructor (`ContainerInfo::resize` is null).
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline bool container_resize(const ContainerInfo *info, void *container, usize new_size) noexcept {
        if (info == nullptr || info->resize == nullptr) {
            return false;
        }
        if (info->fixed_size && container_size(info, container) != new_size) {
            return false;
        }
        info->resize(container, new_size);
        return true;
    }


} // namespace SFT::Reflection
