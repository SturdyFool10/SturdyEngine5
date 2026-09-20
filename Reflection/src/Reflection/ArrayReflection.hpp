#pragma once

#include <Reflection/ContainerInfo.hpp>
#include <Reflection/FieldInfo.hpp>
#include <Reflection/MapInfo.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/OptionalInfo.hpp>
#include <Reflection/SetInfo.hpp>
#include <Reflection/TypeId.hpp>

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

/// The `java.lang.reflect.Array`-style ergonomic layer over this package's raw, function-pointer
/// based `ContainerInfo`/`MapInfo`/`SetInfo`/`OptionalInfo` machinery.
///
/// The raw machinery (see those four headers) is deliberately minimal: one `struct` of function
/// pointers per recognized shape, plus a handful of free functions that null-check and forward.
/// That is the right shape for the *producers* of this metadata (`Detail::container_info_for` and
/// friends in `Macros.hpp`, one Meyers-singleton per distinct element/key/value type), but it is an
/// awkward shape for *consumers* that want to walk an unknown field generically: they have to check
/// all four of `FieldInfo::container`/`map`/`set`/`optional` by hand, remember which raw function
/// pointer needs which bounds/null check, and manage placement-new/placement-destroy buffers
/// themselves when they want a real typed value rather than a raw-byte view.
///
/// This file wraps each shape in a small value type (`ReflectedSequence`/`ReflectedMap`/
/// `ReflectedSet`/`ReflectedOptional`) that borrows a `...Info*` and a `void*` container pointer —
/// exactly two pointers, trivially copyable, no ownership — and adds:
///   - Bounds/null-checking by construction (every method simply forwards to the existing
///     `container_get_element`/`map_find`/... free functions, which already fail closed; nothing
///     here makes those *more* safe, but wrapping them removes the chance of a caller forgetting to
///     check).
///   - Templated convenience overloads (`get<T>(index)`, `find<K, V>(key)`, ...) for the common case
///     where the caller *does* know the element/key/value type at the call site, so it can work with
///     real `T`/`K`/`V` values instead of raw aligned byte buffers — while still verifying, at
///     runtime, that `T`/`K`/`V` actually matches the field's real erased type before trusting the
///     cast. That check is done via `type_id_for<T>()` (`Macros.hpp`), the same identity
///     `ContainerInfo::element_type`/`MapInfo::key_type`/etc. were built from — it resolves through
///     `structural_type_id<T>()` whenever `T` is nameable or structurally composed (a container of
///     nameable things), and only falls back to a `typeid(T).name()`-derived identity for a type
///     with no canonical name at all, exactly mirroring how the rest of this package treats type
///     identity. A caller whose `T` is not nameable still gets *a* check (internally consistent
///     within one build), not a silently-skipped one.
///   - A dispatching entry point (`dispatch_reflected_field`) that inspects a `FieldInfo`'s four
///     mutually-exclusive shape pointers once and calls a visitor with whichever wrapper applies —
///     the actual "don't make every caller re-derive the same four-way branch" win.
///
/// None of this changes what is representable: every wrapper is a thin, always-inlinable forwarder
/// to the existing free functions, so anything expressible through the raw API remains expressible
/// (and identically fast) through these types. What changes is how much boilerplate a caller needs
/// to get there safely.
namespace SFT::Reflection {

    // ── ReflectedSequence ──────────────────────────────────────────────────────────────────────

    /// Ergonomic wrapper over a `ContainerInfo` + its backing storage — the sequence/array
    /// counterpart of `java.lang.reflect.Array`. Two borrowed pointers, valid for exactly as long
    /// as `info`/`container` are (this type never allocates, never owns, and is trivially
    /// copyable) — constructing one is not itself a safety boundary, using it out past the
    /// container's lifetime is exactly as much of a use-after-free as touching `container` directly
    /// would be.
    class ReflectedSequence {
      public:
        /// Wraps `container` (a live `std::vector<Element>`/`std::array<Element, N>` instance, or
        /// any future shape `info` describes) for element-level access via `info`.
        ///
        /// @note This function does not throw exceptions.
        ReflectedSequence(const ContainerInfo &info, void *container) noexcept : info_(&info), container_(container) {}

        /// The `java.lang.reflect.Array.getLength` equivalent: the container's current element
        /// count.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize length() const noexcept {
            return container_size(info_, container_);
        }

        /// Reports whether this is a fixed-capacity container (`std::array<T, N>`) — `resize` can
        /// only ever succeed against its one true size for these, never grow or shrink them.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_fixed_size() const noexcept {
            return info_->fixed_size;
        }

        /// The element type's identity, for a caller that wants to compare/dispatch on it before
        /// touching any element (e.g. to pick which `get<T>`/`set<T>` instantiation applies).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeId element_type() const noexcept {
            return info_->element_type;
        }

        /// The element type's byte size — matches what a raw `void*` buffer passed to `get`/`set`
        /// must be sized to.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize element_size() const noexcept {
            return info_->element_size;
        }

        /// Reports whether the element type is trivially copyable (safe to `memcpy` in bulk via
        /// `data`/`mutable_data` rather than element-by-element via `get`/`set`).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool element_is_trivial() const noexcept {
            return info_->element_trivial;
        }

        /// Type-erased element read: placement-copies the element at `index` into `out_value`
        /// (caller-owned storage of at least `element_size()` bytes, aligned to at least the
        /// element's alignment — the same "trust the caller's buffer, no allocation" convention
        /// `FieldInfo::copy_field_out` uses).
        ///
        /// @return Returns `true` on success; `false` when `index >= length()` — checked here (via
        /// `container_get_element`), not left to the caller, since the raw `ContainerInfo::get_element`
        /// function pointer itself performs no bounds check at all and reading past the end would be
        /// undefined behavior.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool get(usize index, void *out_value) const noexcept {
            return container_get_element(info_, container_, index, out_value);
        }

        /// Type-erased element write. See `get` for the bounds-checking guarantee (`set` gets the
        /// same one, via `container_set_element`).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set(usize index, const void *in_value) const noexcept {
            return container_set_element(info_, container_, index, in_value);
        }

        /// The `java.lang.reflect.Array.newInstance`/resize-in-place equivalent: resizes to exactly
        /// `new_length` elements.
        ///
        /// @return Returns `true` on success; `false` when this is fixed-size and `new_length !=
        /// length()`, or the element type has no default constructor (`ContainerInfo::resize` is
        /// null for those — see its doc comment).
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool resize(usize new_length) const noexcept {
            return container_resize(info_, container_, new_length);
        }

        /// A read-only view over the container's contiguous backing storage, or `nullptr` when the
        /// shape offers none (`ContainerInfo::data` is currently always populated for every
        /// recognized shape, but this stays nullable to track that field honestly).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] const void *data() const noexcept {
            return info_->data != nullptr ? info_->data(container_) : nullptr;
        }

        /// Mutable counterpart to `data`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] void *mutable_data() const noexcept {
            return info_->mutable_data != nullptr ? info_->mutable_data(container_) : nullptr;
        }

        /// Typed convenience read: returns a real `T` copy of the element at `index`, or
        /// `std::nullopt` when `index` is out of range or `T` does not match the container's actual
        /// element type (checked via `type_id_for<T>()` against `element_type()`, plus a `sizeof`
        /// sanity check — two unrelated types can share a size, so the `TypeId` comparison is the
        /// one that actually matters, but the size check catches a stale/mismatched `TypeId` cheaply
        /// before doing anything unsafe with the byte buffer).
        ///
        /// Unlike `OptionalInfo::data`-based access, `ContainerInfo::get_element` always
        /// placement-copies via the element's own copy constructor (never restricted to trivially
        /// copyable elements — see `Detail::container_info_for`), so this works for any
        /// copy-constructible element type, not just scalars.
        ///
        /// @note This function does not throw exceptions unless `T`'s move/copy constructor does.
        template <class T>
        [[nodiscard]] std::optional<T> get(usize index) const {
            if (!matches_element_type<T>()) {
                return std::nullopt;
            }
            alignas(T) std::byte storage[sizeof(T)];
            if (!get(index, static_cast<void *>(storage))) {
                return std::nullopt;
            }
            T *typed = std::launder(reinterpret_cast<T *>(storage));
            std::optional<T> result(std::in_place, std::move(*typed));
            std::destroy_at(typed);
            return result;
        }

        /// Typed convenience write: assigns `value` into the element at `index`, after confirming
        /// `T` matches the container's real element type. See `get<T>` for the matching rule.
        ///
        /// @return Returns `true` on success; `false` on a type mismatch or an out-of-range `index`.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool set(usize index, const T &value) const noexcept {
            if (!matches_element_type<T>()) {
                return false;
            }
            return set(index, static_cast<const void *>(std::addressof(value)));
        }

        /// A zero-copy, type-checked `std::span` over the container's contiguous storage — the
        /// natural way to read every element at once for a `T` known at the call site, without a
        /// `get<T>(i)` call per index. Empty when `T` does not match, or when the shape offers no
        /// contiguous `data()` (see `data`'s doc comment — currently never happens for a recognized
        /// shape, but the empty span degrades gracefully rather than dereferencing a null pointer).
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] std::span<const T> view() const noexcept {
            if (!matches_element_type<T>()) {
                return {};
            }
            const void *raw = data();
            if (raw == nullptr) {
                return {};
            }
            return std::span<const T>(static_cast<const T *>(raw), length());
        }

      private:
        template <class T>
        [[nodiscard]] bool matches_element_type() const noexcept {
            return sizeof(T) == info_->element_size && type_id_for<T>() == info_->element_type;
        }

        const ContainerInfo *info_;
        void *container_;
    };

    // ── ReflectedMap ───────────────────────────────────────────────────────────────────────────

    /// Ergonomic wrapper over a `MapInfo` + its backing storage. See `ReflectedSequence`'s doc
    /// comment for the general "two borrowed pointers, no ownership" contract, which applies here
    /// identically.
    class ReflectedMap {
      public:
        /// Wraps `map` (a live `std::unordered_map<K, V>`/`std::map<K, V>` instance) for key/value
        /// access via `info`.
        ///
        /// @note This function does not throw exceptions.
        ReflectedMap(const MapInfo &info, void *map) noexcept : info_(&info), map_(map) {}

        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept {
            return map_size(info_, map_);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeId key_type() const noexcept {
            return info_->key_type;
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeId value_type() const noexcept {
            return info_->value_type;
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear() const noexcept {
            return map_clear(info_, map_);
        }

        /// Type-erased insert/overwrite. `key`/`value` must point at `key_size()`/`value_size()`
        /// bytes of the map's real key/value type.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool insert_or_assign(const void *key, const void *value) const noexcept {
            return map_insert_or_assign(info_, map_, key, value);
        }

        /// Type-erased lookup: placement-copies the value for `key` into `out_value`.
        ///
        /// @return Returns `true` on success; `false` when `key` is not present.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool find(const void *key, void *out_value) const noexcept {
            return map_find(info_, map_, key, out_value);
        }

        /// @return Returns `true` when an entry was removed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool erase(const void *key) const noexcept {
            return map_erase(info_, map_, key);
        }

        /// Type-erased visitation: `visitor` is called once per entry as `visitor(const void *key,
        /// const void *value)`, both pointers valid only for the duration of that one call (mirrors
        /// `MapInfo::for_each`'s own contract exactly).
        ///
        /// Unlike the raw `MapInfo::for_each`, `visitor` here can be any callable (a capturing
        /// lambda included) rather than a capture-less `MapInfo::VisitFn` plus a manually threaded
        /// `void *user_data` — this function is the trampoline that recovers the callable from
        /// `user_data` on the caller's behalf, at zero added indirection beyond the one the raw API
        /// already pays for.
        ///
        /// @note This function does not throw exceptions unless `visitor` does — and since
        /// `MapInfo::for_each`'s underlying visitor pointer is itself `noexcept`, a `visitor` that
        /// throws here terminates the process exactly as it would calling the raw API directly with
        /// a throwing callback; this wrapper does not change that hazard, only who can express it.
        template <class Visitor>
        void for_each(Visitor &&visitor) const {
            if (info_ == nullptr || info_->for_each == nullptr) {
                return;
            }
            info_->for_each(map_, &for_each_trampoline<Visitor>, static_cast<void *>(std::addressof(visitor)));
        }

        /// Typed convenience visitation: `visitor` is called once per entry as `visitor(const K
        /// &key, const V &value)`. Silently visits nothing (rather than reinterpret-casting garbage)
        /// when `K`/`V` do not match this map's real key/value types — matching every other typed
        /// accessor in this file's "fail closed" rule.
        ///
        /// @note This function does not throw exceptions unless `visitor` does.
        template <class K, class V, class Visitor>
        void for_each(Visitor &&visitor) const {
            if (!matches_key_type<K>() || !matches_value_type<V>()) {
                return;
            }
            for_each([&visitor](const void *key, const void *value) {
                visitor(*static_cast<const K *>(key), *static_cast<const V *>(value));
            });
        }

        /// Typed convenience insert/overwrite. See `ReflectedSequence::get<T>` for the type-matching
        /// rule this and every other typed accessor below shares.
        ///
        /// @note This function does not throw exceptions.
        template <class K, class V>
        [[nodiscard]] bool insert_or_assign(const K &key, const V &value) const noexcept {
            if (!matches_key_type<K>() || !matches_value_type<V>()) {
                return false;
            }
            return insert_or_assign(static_cast<const void *>(std::addressof(key)), static_cast<const void *>(std::addressof(value)));
        }

        /// Typed convenience lookup: returns a real `V` copy of the value for `key`, or
        /// `std::nullopt` when absent or on a type mismatch.
        ///
        /// @note This function does not throw exceptions unless `V`'s move/copy constructor does.
        template <class K, class V>
        [[nodiscard]] std::optional<V> find(const K &key) const {
            if (!matches_key_type<K>() || !matches_value_type<V>()) {
                return std::nullopt;
            }
            alignas(V) std::byte storage[sizeof(V)];
            if (!find(static_cast<const void *>(std::addressof(key)), static_cast<void *>(storage))) {
                return std::nullopt;
            }
            V *typed = std::launder(reinterpret_cast<V *>(storage));
            std::optional<V> result(std::in_place, std::move(*typed));
            std::destroy_at(typed);
            return result;
        }

        /// Typed convenience erase.
        ///
        /// @note This function does not throw exceptions.
        template <class K>
        [[nodiscard]] bool erase(const K &key) const noexcept {
            if (!matches_key_type<K>()) {
                return false;
            }
            return erase(static_cast<const void *>(std::addressof(key)));
        }

      private:
        template <class Visitor>
        static void for_each_trampoline(const void *key, const void *value, void *user_data) noexcept {
            (*static_cast<Visitor *>(user_data))(key, value);
        }

        template <class K>
        [[nodiscard]] bool matches_key_type() const noexcept {
            return sizeof(K) == info_->key_size && type_id_for<K>() == info_->key_type;
        }

        template <class V>
        [[nodiscard]] bool matches_value_type() const noexcept {
            return sizeof(V) == info_->value_size && type_id_for<V>() == info_->value_type;
        }

        const MapInfo *info_;
        void *map_;
    };

    // ── ReflectedSet ───────────────────────────────────────────────────────────────────────────

    /// Ergonomic wrapper over a `SetInfo` + its backing storage — `ReflectedMap`'s key-only
    /// counterpart, mirroring `SetInfo` being `MapInfo`'s key-only counterpart.
    class ReflectedSet {
      public:
        /// @note This function does not throw exceptions.
        ReflectedSet(const SetInfo &info, void *set) noexcept : info_(&info), set_(set) {}

        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept {
            return set_size(info_, set_);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeId element_type() const noexcept {
            return info_->element_type;
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear() const noexcept {
            return set_clear(info_, set_);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool insert(const void *element) const noexcept {
            return set_insert(info_, set_, element);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(const void *element) const noexcept {
            return set_contains(info_, set_, element);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool erase(const void *element) const noexcept {
            return set_erase(info_, set_, element);
        }

        /// Type-erased visitation. See `ReflectedMap::for_each`'s doc comment — identical shape and
        /// caveats, one element pointer per call instead of a key/value pair.
        ///
        /// @note This function does not throw exceptions unless `visitor` does.
        template <class Visitor>
        void for_each(Visitor &&visitor) const {
            if (info_ == nullptr || info_->for_each == nullptr) {
                return;
            }
            info_->for_each(set_, &for_each_trampoline<Visitor>, static_cast<void *>(std::addressof(visitor)));
        }

        /// Typed convenience visitation. See `ReflectedMap::for_each<K, V>`.
        ///
        /// @note This function does not throw exceptions unless `visitor` does.
        template <class T, class Visitor>
        void for_each(Visitor &&visitor) const {
            if (!matches_element_type<T>()) {
                return;
            }
            for_each([&visitor](const void *element) {
                visitor(*static_cast<const T *>(element));
            });
        }

        /// Typed convenience insert.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool insert(const T &element) const noexcept {
            if (!matches_element_type<T>()) {
                return false;
            }
            return insert(static_cast<const void *>(std::addressof(element)));
        }

        /// Typed convenience membership check.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool contains(const T &element) const noexcept {
            if (!matches_element_type<T>()) {
                return false;
            }
            return contains(static_cast<const void *>(std::addressof(element)));
        }

        /// Typed convenience erase.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool erase(const T &element) const noexcept {
            if (!matches_element_type<T>()) {
                return false;
            }
            return erase(static_cast<const void *>(std::addressof(element)));
        }

      private:
        template <class Visitor>
        static void for_each_trampoline(const void *element, void *user_data) noexcept {
            (*static_cast<Visitor *>(user_data))(element);
        }

        template <class T>
        [[nodiscard]] bool matches_element_type() const noexcept {
            return sizeof(T) == info_->element_size && type_id_for<T>() == info_->element_type;
        }

        const SetInfo *info_;
        void *set_;
    };

    // ── ReflectedOptional ──────────────────────────────────────────────────────────────────────

    /// Ergonomic wrapper over an `OptionalInfo` + its backing storage — the "nullable single value"
    /// shape shared by `std::optional<T>`/`std::unique_ptr<T>`/`std::shared_ptr<T>` (see
    /// `FieldInfo::optional`'s doc comment). Not literally array-shaped, but belongs in the same
    /// unified-erased-access story: its raw API has the same "four raw function pointers, no bounds/
    /// presence checking done for you" ergonomics problem the container shapes do.
    class ReflectedOptional {
      public:
        /// @note This function does not throw exceptions.
        ReflectedOptional(const OptionalInfo &info, void *optional) noexcept : info_(&info), optional_(optional) {}

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_value() const noexcept {
            return optional_has_value(info_, optional_);
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeId value_type() const noexcept {
            return info_->value_type;
        }

        /// @note This function does not throw exceptions.
        [[nodiscard]] bool reset() const noexcept {
            return optional_reset(info_, optional_);
        }

        /// Type-erased emplace: sets the optional to hold a copy of `*in_value`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set(const void *in_value) const noexcept {
            return optional_set(info_, optional_, in_value);
        }

        /// A read-only pointer to the contained value, or `nullptr` when empty — unlike
        /// `optional_get` (`OptionalInfo.hpp`), this is not restricted to trivially copyable values:
        /// it is just the raw `OptionalInfo::data` pointer, which is valid for any value type.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] const void *data() const noexcept {
            return info_->data != nullptr ? info_->data(optional_) : nullptr;
        }

        /// Mutable counterpart to `data`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] void *mutable_data() const noexcept {
            return info_->mutable_data != nullptr ? info_->mutable_data(optional_) : nullptr;
        }

        /// Typed convenience read: returns a real `T` copy of the contained value, or
        /// `std::nullopt` when empty or on a type mismatch. Works for any copy-constructible `T`
        /// (not just trivially copyable ones, unlike `optional_get`), because this reads through
        /// `data()` and copy-constructs `T` directly rather than going through `OptionalInfo`'s
        /// trivial-only `memcpy` path.
        ///
        /// @note This function does not throw exceptions unless `T`'s copy constructor does.
        template <class T>
        [[nodiscard]] std::optional<T> get() const {
            if (!matches_value_type<T>()) {
                return std::nullopt;
            }
            const void *raw = data();
            if (raw == nullptr) {
                return std::nullopt;
            }
            return std::optional<T>(std::in_place, *static_cast<const T *>(raw));
        }

        /// Typed convenience emplace.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] bool set(const T &value) const noexcept {
            if (!matches_value_type<T>()) {
                return false;
            }
            return set(static_cast<const void *>(std::addressof(value)));
        }

        /// A type-checked mutable pointer to the contained value, or `nullptr` when empty or on a
        /// type mismatch — for in-place mutation of a non-trivial value without a copy round-trip.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] T *mutable_data_as() const noexcept {
            if (!matches_value_type<T>()) {
                return nullptr;
            }
            return static_cast<T *>(mutable_data());
        }

      private:
        template <class T>
        [[nodiscard]] bool matches_value_type() const noexcept {
            return sizeof(T) == info_->value_size && type_id_for<T>() == info_->value_type;
        }

        const OptionalInfo *info_;
        void *optional_;
    };

    // ── Dispatch ───────────────────────────────────────────────────────────────────────────────

    /// Which of the four mutually-exclusive shapes (see `FieldInfo`'s doc comment) a field is, or
    /// none. A lighter-weight alternative to `dispatch_reflected_field` for a caller that only wants
    /// to branch on the shape without constructing a wrapper yet.
    enum class ReflectedFieldShape : u8 {
        None,
        Sequence,
        Map,
        Set,
        Optional,
    };

    /// Reports which shape `field` is, purely by inspecting its four (mutually exclusive, per
    /// `FieldInfo`'s doc comment) shape pointers.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline ReflectedFieldShape reflected_field_shape(const FieldInfo &field) noexcept {
        if (field.container != nullptr) {
            return ReflectedFieldShape::Sequence;
        }
        if (field.map != nullptr) {
            return ReflectedFieldShape::Map;
        }
        if (field.set != nullptr) {
            return ReflectedFieldShape::Set;
        }
        if (field.optional != nullptr) {
            return ReflectedFieldShape::Optional;
        }
        return ReflectedFieldShape::None;
    }

    namespace Detail {

        /// Resolves `field`'s real storage address on `object` — `object + field.offset` for an
        /// instance field, `field.static_address` for a static one (`offset` is always `0` for
        /// those; see `FieldFlags::Static`'s doc comment and `copy_static_field_out`, which this
        /// mirrors exactly).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] inline void *reflected_field_storage(const FieldInfo &field, void *object) noexcept {
            if (has_flag(field.flags, FieldFlags::Static)) {
                return field.static_address;
            }
            return static_cast<void *>(static_cast<unsigned char *>(object) + field.offset);
        }

    } // namespace Detail

    /// The actual "don't make every caller re-derive the same four-way branch" entry point:
    /// inspects `field`'s shape once and calls `visitor` with the one wrapper that applies —
    /// `visitor(ReflectedSequence{...})`, `visitor(ReflectedMap{...})`, `visitor(ReflectedSet{...})`,
    /// or `visitor(ReflectedOptional{...})` — or does not call `visitor` at all when `field` is none
    /// of those (an ordinary scalar/struct field, handled through `copy_field_out`/`copy_field_in`
    /// instead).
    ///
    /// `visitor` is expected to be a generic lambda (or an overload set) accepting all four wrapper
    /// types, matching this codebase's established "generic lambda with per-case handling" idiom
    /// (see `TypeTraits<T>::for_each_member`'s call sites in `Macros.hpp`, which take a generic
    /// lambda keyed on `MemberKind` via `if constexpr` the same way):
    /// @code
    /// dispatch_reflected_field(field, &object, [](auto &&reflected) {
    ///     using Reflected = std::decay_t<decltype(reflected)>;
    ///     if constexpr (std::is_same_v<Reflected, ReflectedSequence>) {
    ///         // reflected.length(), reflected.get<T>(i), ...
    ///     } else if constexpr (std::is_same_v<Reflected, ReflectedMap>) {
    ///         // reflected.for_each<K, V>(...), ...
    ///     }
    ///     // ReflectedSet / ReflectedOptional cases, as needed.
    /// });
    /// @endcode
    ///
    /// @return Returns `true` when `field` was one of the four recognized shapes and `visitor` was
    /// called; `false` when `field` is a plain (non-container-shaped) field and `visitor` was never
    /// invoked.
    /// @note This function does not throw exceptions unless `visitor` does.
    template <class Visitor>
    bool dispatch_reflected_field(const FieldInfo &field, void *object, Visitor &&visitor) {
        void *storage = Detail::reflected_field_storage(field, object);
        if (field.container != nullptr) {
            visitor(ReflectedSequence{*field.container, storage});
            return true;
        }
        if (field.map != nullptr) {
            visitor(ReflectedMap{*field.map, storage});
            return true;
        }
        if (field.set != nullptr) {
            visitor(ReflectedSet{*field.set, storage});
            return true;
        }
        if (field.optional != nullptr) {
            visitor(ReflectedOptional{*field.optional, storage});
            return true;
        }
        return false;
    }

    /// Non-visitor convenience built on `dispatch_reflected_field`, for a caller that just wants
    /// "give me the sequence wrapper if this field is one" without writing a visitor.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline std::optional<ReflectedSequence> as_reflected_sequence(const FieldInfo &field, void *object) noexcept {
        if (field.container == nullptr) {
            return std::nullopt;
        }
        return ReflectedSequence{*field.container, Detail::reflected_field_storage(field, object)};
    }

    /// See `as_reflected_sequence`.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline std::optional<ReflectedMap> as_reflected_map(const FieldInfo &field, void *object) noexcept {
        if (field.map == nullptr) {
            return std::nullopt;
        }
        return ReflectedMap{*field.map, Detail::reflected_field_storage(field, object)};
    }

    /// See `as_reflected_sequence`.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline std::optional<ReflectedSet> as_reflected_set(const FieldInfo &field, void *object) noexcept {
        if (field.set == nullptr) {
            return std::nullopt;
        }
        return ReflectedSet{*field.set, Detail::reflected_field_storage(field, object)};
    }

    /// See `as_reflected_sequence`.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] inline std::optional<ReflectedOptional> as_reflected_optional(const FieldInfo &field, void *object) noexcept {
        if (field.optional == nullptr) {
            return std::nullopt;
        }
        return ReflectedOptional{*field.optional, Detail::reflected_field_storage(field, object)};
    }


} // namespace SFT::Reflection
