#pragma once

#include <Reflection/Attribute.hpp>
#include <Reflection/Contract.hpp>
#include <Reflection/EnumInfo.hpp>
#include <Reflection/InvokeException.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <array>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SFT::Reflection::Detail {

    /// Detects `std::vector<T, Alloc>` specifically (the only recognized sequence container — see
    /// `ContainerInfo`'s doc comment; `std::array<T, N>` is handled separately by `IsStdArray`
    /// below, since it needs a fixed-size accessor rather than a resizable one).
    template <class T>
    struct IsStdVector : std::false_type {};
    template <class T, class Alloc>
    struct IsStdVector<std::vector<T, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::array<T, N>`.
    template <class T>
    struct IsStdArray : std::false_type {};
    template <class T, usize N>
    struct IsStdArray<std::array<T, N>> : std::true_type {
        using Element = T;
        static constexpr usize Size = N;
    };

    /// Detects `std::unordered_map<K, V, Hash, Eq, Alloc>`.
    template <class T>
    struct IsStdUnorderedMap : std::false_type {};
    template <class K, class V, class Hash, class Eq, class Alloc>
    struct IsStdUnorderedMap<std::unordered_map<K, V, Hash, Eq, Alloc>> : std::true_type {
        using Key = K;
        using Value = V;
    };

    /// Detects `std::map<K, V, Compare, Alloc>` (ordered) — reuses the exact same runtime
    /// `MapInfo` shape as `std::unordered_map`: `MapInfo`'s operations (`for_each`/
    /// `insert_or_assign`/`find`/`erase`/`clear`) never depended on hashing vs. ordering in the
    /// first place, so the only thing that differs is which container type the generated lambdas
    /// close over (see `map_info_for`).
    template <class T>
    struct IsStdMap : std::false_type {};
    template <class K, class V, class Compare, class Alloc>
    struct IsStdMap<std::map<K, V, Compare, Alloc>> : std::true_type {
        using Key = K;
        using Value = V;
    };

    /// Detects `std::set<T, Compare, Alloc>`.
    template <class T>
    struct IsStdSet : std::false_type {};
    template <class T, class Compare, class Alloc>
    struct IsStdSet<std::set<T, Compare, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::unordered_set<T, Hash, Eq, Alloc>`.
    template <class T>
    struct IsStdUnorderedSet : std::false_type {};
    template <class T, class Hash, class Eq, class Alloc>
    struct IsStdUnorderedSet<std::unordered_set<T, Hash, Eq, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::optional<T>`.
    template <class T>
    struct IsStdOptional : std::false_type {};
    template <class T>
    struct IsStdOptional<std::optional<T>> : std::true_type {
        using Value = T;
    };

    /// Detects `std::unique_ptr<T, Deleter>` (the default deleter only — a custom deleter can't
    /// generically be reconstructed from a bare copy of `*T`, so it falls through to the opaque
    /// non-trivial-type path like any other unrecognized shape).
    template <class T>
    struct IsStdUniquePtr : std::false_type {};
    template <class T>
    struct IsStdUniquePtr<std::unique_ptr<T>> : std::true_type {
        using Value = T;
    };

    /// Detects `std::shared_ptr<T>`.
    template <class T>
    struct IsStdSharedPtr : std::false_type {};
    template <class T>
    struct IsStdSharedPtr<std::shared_ptr<T>> : std::true_type {
        using Value = T;
    };


    /// Returns a reflected type's stable canonical name.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid for the
    /// program's lifetime, since it aliases the `TypeTraits<T>` specialization's static storage.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval std::string_view type_name() {
        using TypeT = std::remove_cv_t<T>;
        constexpr std::string_view name = TypeTraits<TypeT>::name;
        static_assert(!name.empty(),
                      "Reflected types need a stable canonical name. Specialize "
                      "SFT::Reflection::TypeTraits<T> or use SFT_REFLECT_TYPE(T, \"name\").");
        return name;
    }

    /// Returns a reflected type's schema version, defaulting to 1 when unspecified.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval u32 type_schema_version() {
        using TypeT = std::remove_cv_t<T>;
        if constexpr (requires { TypeTraits<TypeT>::schema_version; }) {
            return TypeTraits<TypeT>::schema_version;
        } else {
            return 1;
        }
    }

    /// Returns the derived `TypeFlags` for a reflected type.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval TypeFlags type_flags() {
        using TypeT = std::remove_cv_t<T>;
        TypeFlags flags = TypeFlags::None;
        if constexpr (std::is_trivially_copyable_v<TypeT>) {
            flags = flags | TypeFlags::TriviallyCopyable;
        }
        if constexpr (requires { TypeTraits<TypeT>::flags; }) {
            flags = flags | TypeTraits<TypeT>::flags;
        }
        return flags;
    }

    /// Derives a stable-for-this-process-lifetime `TypeId` for an arbitrary C++ type, used to
    /// tag `FieldInfo::field_type`/`MethodInfo::return_type`/`MethodInfo::param_types`.
    ///
    /// When `M` is itself reflected (has a non-empty `TypeTraits<M>::name`), this returns
    /// *exactly* what `TypeRegistry::instance().type<M>().key` would — i.e. the same
    /// canonical-name-derived `TypeId` `make_type_info<M>()` gives that type's own `TypeInfo`, not
    /// a second, incompatible identity for it. This is what lets a serializer (or anything else
    /// walking fields generically) recognize "this field's type is itself a registered
    /// `TypeInfo`" and recurse via `TypeRegistry::find(field.field_type)`, rather than every
    /// reflected type accidentally having two unrelated identities depending on whether you
    /// reached it as a top-level registration or as somebody else's field.
    ///
    /// Also checks `EnumTraits<M>` (for the same reason, symmetrically): a reflected enum used as
    /// a field/parameter type gets its real `EnumInfo::key` identity, not a second one.
    ///
    /// Falls back to `typeid(M).name()` for everything else (`int`, `float`, `UString`, ...) —
    /// not portable/pretty across compilers, but cheap, requires no polymorphic RTTI machinery
    /// for non-polymorphic `M`, and evaluated once per field/method at registration time, never
    /// on the hot access path.
    ///
    /// @return Returns the newly constructed id.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class M>
    [[nodiscard]] TypeId erased_type_id() {
        using TypeM = std::remove_cv_t<M>;
        if constexpr (!TypeTraits<TypeM>::name.empty()) {
            return TypeId::from_name(TypeTraits<TypeM>::name);
        } else if constexpr (!EnumTraits<TypeM>::name.empty()) {
            return TypeId::from_name(EnumTraits<TypeM>::name);
        } else {
            return TypeId::from_name(typeid(TypeM).name());
        }
    }

} // namespace SFT::Reflection::Detail

namespace SFT::Reflection {

    /// Public-facing alias for `Detail::erased_type_id<T>()`, for callers building a `TypeInfo`
    /// by hand (see `TypeInfoBuilder`) rather than through `SFT_REFLECT_FIELD`/`SFT_REFLECT_METHOD`.
    ///
    /// @return Returns the newly constructed id.
    template <class T>
    [[nodiscard]] TypeId type_id_for() {
        return Detail::erased_type_id<std::remove_cv_t<T>>();
    }

} // namespace SFT::Reflection

namespace SFT::Reflection::Detail {

    /// Computes a data member's byte offset from its member pointer.
    ///
    /// Lays out an uninitialized `T`-sized buffer, reinterprets it as `T*`, and measures where
    /// the member lands — the same idiom `offsetof` uses for aggregate members.
    /// `reinterpret_cast` makes this impossible to evaluate at compile time, which is why this —
    /// unlike `TypeTraits<T>::for_each_member` — is not `consteval`.
    ///
    /// @note v1 requires `T` non-polymorphic (see the `static_assert` below) — a vtable pointer
    /// would need real construction to be in a valid state before member access through it means
    /// anything, which this uninitialized-buffer trick never does. This is deliberately *not* a
    /// `std::is_standard_layout_v<T>` check: that trait requires every subobject, recursively, to
    /// itself be standard-layout, which most STL containers (`std::unordered_map` in every
    /// implementation tested) are not — under that check, no struct with a map field, or any
    /// field of a type with one, could ever be reflected. Polymorphism is the actual hazard this
    /// technique cares about; a non-virtual struct's own member layout is placed at a fixed,
    /// computable offset by every ABI this engine targets (Itanium, MSVC) regardless of whether a
    /// member's own type happens to be standard-layout. Real C++26 static reflection (P2996)
    /// removes the need for this technique — and this caveat with it — entirely, by enumerating
    /// members instead of taking their address.
    template <class T, class M>
    [[nodiscard]] usize member_offset(M T::*member) noexcept {
        static_assert(!std::is_polymorphic_v<T>,
                      "SFT_REFLECT_FIELD requires a non-polymorphic type in the macro-based frontend.");
        alignas(T) unsigned char storage[sizeof(T)]{};
        auto *base = reinterpret_cast<T *>(static_cast<void *>(storage));
        const auto *field = std::addressof(base->*member);
        return static_cast<usize>(reinterpret_cast<const unsigned char *>(field) - storage);
    }

    /// Returns the process-lifetime `ContainerInfo` for element type `T` (a Meyers-singleton
    /// static, one per distinct element type — never allocated per-field, so `FieldInfo::container`
    /// is just a borrowed pointer). `Element` must be default-constructible and copyable for the
    /// full set of operations; `resize` is left null otherwise (matches how `TypeInfo`'s own
    /// `default_construct` is left null for non-default-constructible types).
    template <class Element>
    [[nodiscard]] const ContainerInfo *container_info_for() {
        static const ContainerInfo info{
            .element_type = erased_type_id<Element>(),
            .element_size = sizeof(Element),
            .element_align = alignof(Element),
            .element_trivial = std::is_trivially_copyable_v<Element>,
            .element_primitive_kind = primitive_kind_of<Element>(),
            .size = [](const void *container) noexcept -> usize {
                return static_cast<const std::vector<Element> *>(container)->size();
            },
            .get_element = [](const void *container, usize index, void *out_value) noexcept {
                const auto &value = (*static_cast<const std::vector<Element> *>(container))[index];
                ::new (out_value) Element(value);
            },
            .set_element = [](void *container, usize index, const void *in_value) noexcept {
                (*static_cast<std::vector<Element> *>(container))[index] = *static_cast<const Element *>(in_value);
            },
            .resize = []() -> void (*)(void *, usize) noexcept {
                if constexpr (std::is_nothrow_default_constructible_v<Element>) {
                    return [](void *container, usize new_size) noexcept {
                        static_cast<std::vector<Element> *>(container)->resize(new_size);
                    };
                } else {
                    return nullptr;
                }
            }(),
            .data = [](const void *container) noexcept -> const void * {
                return static_cast<const std::vector<Element> *>(container)->data();
            },
            .mutable_data = [](void *container) noexcept -> void * {
                return static_cast<std::vector<Element> *>(container)->data();
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `MapInfo` for `(MapT, Key, Value)` — a Meyers-singleton
    /// static, one per distinct triple, mirroring `container_info_for`. Templated on the actual
    /// map type (not just `Key`/`Value`) so `std::unordered_map<K, V>` and `std::map<K, V>` can
    /// share this one implementation: every `MapInfo` operation is expressed purely in terms of
    /// `MapT`'s ordinary container interface (`size`/`clear`/range-for/`insert_or_assign`/`find`/
    /// `erase`), none of which cares whether `MapT` happens to be hashed or ordered.
    template <class MapT, class Key, class Value>
    [[nodiscard]] const MapInfo *map_info_for() {
        static const MapInfo info{
            .key_type = erased_type_id<Key>(),
            .key_size = sizeof(Key),
            .key_align = alignof(Key),
            .key_trivial = std::is_trivially_copyable_v<Key>,
            .key_primitive_kind = primitive_kind_of<Key>(),
            .value_type = erased_type_id<Value>(),
            .value_size = sizeof(Value),
            .value_align = alignof(Value),
            .value_trivial = std::is_trivially_copyable_v<Value>,
            .value_primitive_kind = primitive_kind_of<Value>(),
            .size = [](const void *map) noexcept -> usize {
                return static_cast<const MapT *>(map)->size();
            },
            .clear = [](void *map) noexcept {
                static_cast<MapT *>(map)->clear();
            },
            .for_each = [](const void *map, MapInfo::VisitFn visitor, void *user_data) noexcept {
                for (const auto &entry : *static_cast<const MapT *>(map)) {
                    visitor(&entry.first, &entry.second, user_data);
                }
            },
            .insert_or_assign = [](void *map, const void *key, const void *value) noexcept {
                static_cast<MapT *>(map)->insert_or_assign(*static_cast<const Key *>(key), *static_cast<const Value *>(value));
            },
            .find = [](const void *map, const void *key, void *out_value) noexcept -> bool {
                const auto &typed_map = *static_cast<const MapT *>(map);
                const auto found = typed_map.find(*static_cast<const Key *>(key));
                if (found == typed_map.end()) {
                    return false;
                }
                ::new (out_value) Value(found->second);
                return true;
            },
            .erase = [](void *map, const void *key) noexcept -> bool {
                return static_cast<MapT *>(map)->erase(*static_cast<const Key *>(key)) != 0;
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `ContainerInfo` for `std::array<Element, N>` — a
    /// Meyers-singleton static per distinct `(Element, N)` pair, mirroring `container_info_for`.
    /// `resize` is a real (non-null) no-op function pointer rather than left null: `resize` is
    /// only ever actually invoked by `container_resize` when `new_size` already equals `N` (see
    /// `ContainerInfo::fixed_size`'s doc comment), at which point there is nothing to do.
    template <class Element, usize N>
    [[nodiscard]] const ContainerInfo *array_container_info_for() {
        static const ContainerInfo info{
            .element_type = erased_type_id<Element>(),
            .element_size = sizeof(Element),
            .element_align = alignof(Element),
            .element_trivial = std::is_trivially_copyable_v<Element>,
            .element_primitive_kind = primitive_kind_of<Element>(),
            .fixed_size = true,
            .size = [](const void *) noexcept -> usize { return N; },
            .get_element = [](const void *container, usize index, void *out_value) noexcept {
                const auto &value = (*static_cast<const std::array<Element, N> *>(container))[index];
                ::new (out_value) Element(value);
            },
            .set_element = [](void *container, usize index, const void *in_value) noexcept {
                (*static_cast<std::array<Element, N> *>(container))[index] = *static_cast<const Element *>(in_value);
            },
            .resize = [](void *, usize) noexcept {},
            .data = [](const void *container) noexcept -> const void * {
                return static_cast<const std::array<Element, N> *>(container)->data();
            },
            .mutable_data = [](void *container) noexcept -> void * {
                return static_cast<std::array<Element, N> *>(container)->data();
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `SetInfo` for `(SetT, Element)` — a Meyers-singleton static,
    /// mirroring `map_info_for`'s "templated on the real container type" shape so
    /// `std::set<T>`/`std::unordered_set<T>` share this one implementation.
    template <class SetT, class Element>
    [[nodiscard]] const SetInfo *set_info_for() {
        static const SetInfo info{
            .element_type = erased_type_id<Element>(),
            .element_size = sizeof(Element),
            .element_align = alignof(Element),
            .element_trivial = std::is_trivially_copyable_v<Element>,
            .element_primitive_kind = primitive_kind_of<Element>(),
            .size = [](const void *set) noexcept -> usize {
                return static_cast<const SetT *>(set)->size();
            },
            .clear = [](void *set) noexcept {
                static_cast<SetT *>(set)->clear();
            },
            .for_each = [](const void *set, SetInfo::VisitFn visitor, void *user_data) noexcept {
                for (const auto &element : *static_cast<const SetT *>(set)) {
                    visitor(&element, user_data);
                }
            },
            .insert = [](void *set, const void *element) noexcept {
                static_cast<SetT *>(set)->insert(*static_cast<const Element *>(element));
            },
            .contains = [](const void *set, const void *element) noexcept -> bool {
                return static_cast<const SetT *>(set)->contains(*static_cast<const Element *>(element));
            },
            .erase = [](void *set, const void *element) noexcept -> bool {
                return static_cast<SetT *>(set)->erase(*static_cast<const Element *>(element)) != 0;
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `OptionalInfo` for `Value` — a Meyers-singleton static, one
    /// per distinct value type, mirroring `container_info_for`/`map_info_for`.
    template <class Value>
    [[nodiscard]] const OptionalInfo *optional_info_for() {
        static const OptionalInfo info{
            .value_type = erased_type_id<Value>(),
            .value_size = sizeof(Value),
            .value_align = alignof(Value),
            .value_trivial = std::is_trivially_copyable_v<Value>,
            .value_primitive_kind = primitive_kind_of<Value>(),
            .has_value = [](const void *optional) noexcept -> bool {
                return static_cast<const std::optional<Value> *>(optional)->has_value();
            },
            .data = [](const void *optional) noexcept -> const void * {
                const auto &opt = *static_cast<const std::optional<Value> *>(optional);
                return opt.has_value() ? &*opt : nullptr;
            },
            .mutable_data = [](void *optional) noexcept -> void * {
                auto &opt = *static_cast<std::optional<Value> *>(optional);
                return opt.has_value() ? &*opt : nullptr;
            },
            .emplace_copy = [](void *optional, const void *value) noexcept {
                static_cast<std::optional<Value> *>(optional)->emplace(*static_cast<const Value *>(value));
            },
            .reset = [](void *optional) noexcept {
                static_cast<std::optional<Value> *>(optional)->reset();
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `OptionalInfo` for `std::unique_ptr<Value>` — reuses
    /// `OptionalInfo`'s shape exactly (see `FieldInfo::optional`'s doc comment): a unique_ptr is,
    /// for reflection purposes, just another nullable single value. `emplace_copy` requires
    /// `Value` to be copy-constructible (it builds a brand-new owned copy via `make_unique`,
    /// since a `unique_ptr` cannot share ownership of the caller-supplied source value).
    template <class Value>
    [[nodiscard]] const OptionalInfo *unique_ptr_info_for() {
        static const OptionalInfo info{
            .value_type = erased_type_id<Value>(),
            .value_size = sizeof(Value),
            .value_align = alignof(Value),
            .value_trivial = std::is_trivially_copyable_v<Value>,
            .value_primitive_kind = primitive_kind_of<Value>(),
            .has_value = [](const void *optional) noexcept -> bool {
                return static_cast<const std::unique_ptr<Value> *>(optional)->operator bool();
            },
            .data = [](const void *optional) noexcept -> const void * {
                return static_cast<const std::unique_ptr<Value> *>(optional)->get();
            },
            .mutable_data = [](void *optional) noexcept -> void * {
                return static_cast<std::unique_ptr<Value> *>(optional)->get();
            },
            .emplace_copy = [](void *optional, const void *value) noexcept {
                *static_cast<std::unique_ptr<Value> *>(optional) = std::make_unique<Value>(*static_cast<const Value *>(value));
            },
            .reset = [](void *optional) noexcept {
                static_cast<std::unique_ptr<Value> *>(optional)->reset();
            },
        };
        return &info;
    }

    /// Returns the process-lifetime `OptionalInfo` for `std::shared_ptr<Value>`. See
    /// `unique_ptr_info_for` — identical shape, `make_shared` instead of `make_unique`.
    template <class Value>
    [[nodiscard]] const OptionalInfo *shared_ptr_info_for() {
        static const OptionalInfo info{
            .value_type = erased_type_id<Value>(),
            .value_size = sizeof(Value),
            .value_align = alignof(Value),
            .value_trivial = std::is_trivially_copyable_v<Value>,
            .value_primitive_kind = primitive_kind_of<Value>(),
            .has_value = [](const void *optional) noexcept -> bool {
                return static_cast<const std::shared_ptr<Value> *>(optional)->operator bool();
            },
            .data = [](const void *optional) noexcept -> const void * {
                return static_cast<const std::shared_ptr<Value> *>(optional)->get();
            },
            .mutable_data = [](void *optional) noexcept -> void * {
                return static_cast<std::shared_ptr<Value> *>(optional)->get();
            },
            .emplace_copy = [](void *optional, const void *value) noexcept {
                *static_cast<std::shared_ptr<Value> *>(optional) = std::make_shared<Value>(*static_cast<const Value *>(value));
            },
            .reset = [](void *optional) noexcept {
                static_cast<std::shared_ptr<Value> *>(optional)->reset();
            },
        };
        return &info;
    }

    /// Builds a runtime `FieldInfo` for one data member, given its member pointer as a
    /// non-type template parameter (so it can be referenced from the generated trampolines
    /// below without a lambda capture, letting them decay to plain function pointers).
    /// `attrs` are forwarded straight from `SFT_REFLECT_FIELD`'s trailing `SFT_ATTR_*` arguments.
    template <class T, auto Member>
    [[nodiscard]] FieldInfo build_field_info(std::string_view name, auto &&...attrs) {
        using FieldT = std::remove_reference_t<decltype(std::declval<T &>().*Member)>;
        // std::optional<U>/std::array<U, N> can be trivially copyable whenever U is (their
        // special members are conditionally trivial) — std::optional<int>/std::array<int, 4> are
        // real, common examples. They still need to be treated as their own recognized shape
        // (OptionalInfo/ContainerInfo, has-value- or index-aware access/serialization), not as an
        // opaque Trivial blob, so these checks happen unconditionally, ahead of the Trivial-vs-not
        // decision below (std::vector/std::unordered_map/std::set never have this problem: owning
        // heap memory makes their special members non-trivial unconditionally).
        constexpr bool is_optional = IsStdOptional<FieldT>::value;
        constexpr bool is_pointer_optional = IsStdUniquePtr<FieldT>::value || IsStdSharedPtr<FieldT>::value;
        constexpr bool is_fixed_array = IsStdArray<FieldT>::value;
        constexpr bool is_opaque_trivial = !is_optional && !is_pointer_optional && !is_fixed_array && std::is_trivially_copyable_v<FieldT>;
        FieldFlags flags = FieldFlags::None;
        if constexpr (is_opaque_trivial) {
            flags = flags | FieldFlags::Trivial;
        }
        FieldInfo info{
            .key = TypeId::from_name(name),
            .name = UString{name},
            .field_type = erased_type_id<FieldT>(),
            .offset = member_offset<T>(Member),
            .size = sizeof(FieldT),
            .align = alignof(FieldT),
            .flags = flags,
            .attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...},
        };
        if constexpr (is_opaque_trivial) {
            info.primitive_kind = primitive_kind_of<FieldT>();
            return info;
        }
        // Every remaining shape needs the generic whole-value copy_get/copy_set fallback (used by
        // `copy_field_out`/`copy_field_in` regardless of which of container/map/set/optional is
        // also populated below), plus its specific accessor table.
        info.copy_get = [](const void *object, void *out_value) noexcept {
            const auto *typed = static_cast<const T *>(object);
            ::new (out_value) FieldT(typed->*Member);
        };
        info.copy_set = [](void *object, const void *in_value) noexcept {
            auto *typed = static_cast<T *>(object);
            typed->*Member = *static_cast<const FieldT *>(in_value);
        };
        if constexpr (is_optional) {
            info.optional = optional_info_for<typename IsStdOptional<FieldT>::Value>();
        } else if constexpr (IsStdUniquePtr<FieldT>::value) {
            info.optional = unique_ptr_info_for<typename IsStdUniquePtr<FieldT>::Value>();
        } else if constexpr (IsStdSharedPtr<FieldT>::value) {
            info.optional = shared_ptr_info_for<typename IsStdSharedPtr<FieldT>::Value>();
        } else if constexpr (is_fixed_array) {
            info.container = array_container_info_for<typename IsStdArray<FieldT>::Element, IsStdArray<FieldT>::Size>();
        } else if constexpr (IsStdVector<FieldT>::value) {
            info.container = container_info_for<typename IsStdVector<FieldT>::Element>();
        } else if constexpr (IsStdUnorderedMap<FieldT>::value) {
            info.map = map_info_for<FieldT, typename IsStdUnorderedMap<FieldT>::Key, typename IsStdUnorderedMap<FieldT>::Value>();
        } else if constexpr (IsStdMap<FieldT>::value) {
            info.map = map_info_for<FieldT, typename IsStdMap<FieldT>::Key, typename IsStdMap<FieldT>::Value>();
        } else if constexpr (IsStdSet<FieldT>::value) {
            info.set = set_info_for<FieldT, typename IsStdSet<FieldT>::Element>();
        } else if constexpr (IsStdUnorderedSet<FieldT>::value) {
            info.set = set_info_for<FieldT, typename IsStdUnorderedSet<FieldT>::Element>();
        }
        return info;
    }

    /// Builds a runtime `FieldInfo` for one static data member, given its address (a plain
    /// pointer, not a pointer-to-member — `&ReflectedType::static_member` is just a `FieldT*` for
    /// a static) as a non-type template parameter. `offset` is always `0` and `static_address`
    /// holds the real address; see `FieldFlags::Static`.
    template <auto Member>
    [[nodiscard]] FieldInfo build_static_field_info(std::string_view name, auto &&...attrs) {
        using FieldT = std::remove_reference_t<decltype(*Member)>;
        FieldFlags flags = FieldFlags::Static;
        if constexpr (std::is_trivially_copyable_v<FieldT>) {
            flags = flags | FieldFlags::Trivial;
        }
        FieldInfo info{
            .key = TypeId::from_name(name),
            .name = UString{name},
            .field_type = erased_type_id<FieldT>(),
            .offset = 0,
            .size = sizeof(FieldT),
            .align = alignof(FieldT),
            .flags = flags,
            .attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...},
            .static_address = const_cast<void *>(static_cast<const void *>(Member)),
        };
        if constexpr (!std::is_trivially_copyable_v<FieldT>) {
            info.copy_get = [](const void *object, void *out_value) noexcept {
                ::new (out_value) FieldT(*static_cast<const FieldT *>(object));
            };
            info.copy_set = [](void *object, const void *in_value) noexcept {
                *static_cast<FieldT *>(object) = *static_cast<const FieldT *>(in_value);
            };
        }
        return info;
    }

    /// Extracts the return type and parameter-type tuple from a pointer-to-member-function type.
    /// Covers the four cv/noexcept-qualification combinations a plain (non-overloaded) reflected
    /// method can have; overload sets are not supported in v1 (take `&T::method` of an overload
    /// set is ill-formed anyway without an explicit signature cast at the macro call site).
    template <class M>
    struct MemberFunctionTraits;

    template <class T, class R, class... Args>
    struct MemberFunctionTraits<R (T::*)(Args...)> {
        using Return = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr usize arity = sizeof...(Args);
    };
    template <class T, class R, class... Args>
    struct MemberFunctionTraits<R (T::*)(Args...) noexcept> : MemberFunctionTraits<R (T::*)(Args...)> {};
    template <class T, class R, class... Args>
    struct MemberFunctionTraits<R (T::*)(Args...) const> : MemberFunctionTraits<R (T::*)(Args...)> {};
    template <class T, class R, class... Args>
    struct MemberFunctionTraits<R (T::*)(Args...) const noexcept> : MemberFunctionTraits<R (T::*)(Args...)> {};

    /// Static member functions decay to plain function pointers (no `T::*`) — these
    /// specializations let `MemberFunctionTraits` serve `Detail::build_static_method_info` too.
    template <class R, class... Args>
    struct MemberFunctionTraits<R (*)(Args...)> {
        using Return = R;
        using ArgsTuple = std::tuple<Args...>;
        static constexpr usize arity = sizeof...(Args);
    };
    template <class R, class... Args>
    struct MemberFunctionTraits<R (*)(Args...) noexcept> : MemberFunctionTraits<R (*)(Args...)> {};

    template <class Traits, usize... Is>
    [[nodiscard]] std::vector<TypeId> collect_param_types(std::index_sequence<Is...>) {
        return std::vector<TypeId>{
            erased_type_id<std::remove_cvref_t<std::tuple_element_t<Is, typename Traits::ArgsTuple>>>()...
        };
    }

    /// Derives a method's `MethodInfo::key` from its name *and* parameter-type signature, rather
    /// than the name alone — what makes overloaded methods (`void take_damage(int)` and
    /// `void take_damage(int, DamageType)`, say) get distinct registry identities instead of
    /// colliding on one shared key (previously: two methods sharing a name would silently
    /// overwrite each other's slot). `TypeInfo::find_method(name)`/`TypeRegistry::find_method`'s
    /// by-name overloads still exist and return the first declared overload with that name (kept
    /// for the overwhelmingly common non-overloaded case); a caller that needs to disambiguate an
    /// overload set calls `TypeInfo::find_method(name, param_types)`, or (for a compile-time-known
    /// member pointer, as `SFT_REFLECT_INVOKE` has) `TypeId` this exact function computes from
    /// `Args...`, matching what was registered.
    ///
    /// Builds a plain text signature (`"name#<hash-hi>:<hash-lo>#<hash-hi>:<hash-lo>#..."`, one
    /// `#`-separated segment per parameter) rather than reaching into `Foundation::Fnv1a128`'s
    /// internals to extend a hash incrementally — this is registration/lookup-time cost only
    /// (paid once per declared method, and once per call site's function-local `static` cache in
    /// `Detail::invoke_reflected`), never on the hot per-call path.
    [[nodiscard]] inline TypeId compute_method_key(std::string_view name, std::span<const TypeId> param_types) {
        std::string signature{name};
        for (TypeId param : param_types) {
            signature += '#';
            signature += std::to_string(param.hash.high);
            signature += ':';
            signature += std::to_string(param.hash.low);
        }
        return TypeId::from_name(signature);
    }

    /// Records that the currently-running compiled trampoline threw, into the thread-local
    /// `InvokeException` slot `invoke_method`/`construct_instance` read back after the call (see
    /// `InvokeException`'s doc comment for why this is a thread-local rather than an added
    /// function-pointer parameter).
    inline void record_invoke_exception(const std::exception &e) noexcept {
        g_last_invoke_exception = InvokeException{.threw = true, .message = UString{e.what()}};
    }
    inline void record_unknown_invoke_exception() noexcept {
        g_last_invoke_exception = InvokeException{.threw = true, .message = UString{"non-std::exception thrown across a reflected call"}};
    }

    /// Unpacks `args[Is]` (each an already-typed, caller-marshalled pointer) and calls through
    /// `Member`, writing a non-`void` result into `out_return` via placement-new. The call is
    /// wrapped in `try`/`catch` so a real C++ exception thrown by the game's own method
    /// implementation is caught here — never allowed to cross back out through the `noexcept`
    /// `MethodInvokeFn` boundary (`invoke_trampoline`, below), which would otherwise call
    /// `std::terminate` — and is instead recorded for `invoke_method` to report back to whichever
    /// caller asked for it via `InvokeException`.
    template <class T, auto Member, class Traits, usize... Is>
    void invoke_trampoline_impl(void *object, const void *const *args, void *out_return, std::index_sequence<Is...>) noexcept {
        using Return = typename Traits::Return;
        auto *typed = static_cast<T *>(object);
        try {
            if constexpr (std::is_void_v<Return>) {
                (typed->*Member)(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, typename Traits::ArgsTuple>> *>(
                    const_cast<void *>(args[Is]))...);
            } else {
                Return result = (typed->*Member)(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, typename Traits::ArgsTuple>> *>(
                    const_cast<void *>(args[Is]))...);
                ::new (out_return) Return(std::move(result));
            }
        } catch (const std::exception &e) {
            record_invoke_exception(e);
        } catch (...) {
            record_unknown_invoke_exception();
        }
    }

    template <class T, auto Member, class Traits>
    void invoke_trampoline(void *object, const void *const *args, void *out_return, void * /*user_data*/) noexcept {
        invoke_trampoline_impl<T, Member, Traits>(object, args, out_return, std::make_index_sequence<Traits::arity>{});
    }

    /// Builds a runtime `MethodInfo` for one member function, given its member pointer as a
    /// non-type template parameter (see `build_field_info` for why). `attrs` are forwarded
    /// straight from `SFT_REFLECT_METHOD`'s trailing `SFT_ATTR_*` arguments.
    template <class T, auto Member>
    [[nodiscard]] MethodInfo build_method_info(std::string_view name, auto &&...attrs) {
        using Traits = MemberFunctionTraits<decltype(Member)>;
        MethodInfo info{};
        info.name = UString{name};
        info.return_type = erased_type_id<typename Traits::Return>();
        info.param_types = collect_param_types<Traits>(std::make_index_sequence<Traits::arity>{});
        info.key = compute_method_key(name, info.param_types);
        info.invoke = &invoke_trampoline<T, Member, Traits>;
        info.attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...};
        return info;
    }

    /// Same as `invoke_trampoline_impl`, but for a static member function: no receiver, so
    /// `Member` is called directly (`(*Member)(...)`) rather than through `typed->*Member`.
    template <auto Member, class Traits, usize... Is>
    void invoke_static_trampoline_impl(const void *const *args, void *out_return, std::index_sequence<Is...>) noexcept {
        using Return = typename Traits::Return;
        try {
            if constexpr (std::is_void_v<Return>) {
                (*Member)(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, typename Traits::ArgsTuple>> *>(
                    const_cast<void *>(args[Is]))...);
            } else {
                Return result = (*Member)(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, typename Traits::ArgsTuple>> *>(
                    const_cast<void *>(args[Is]))...);
                ::new (out_return) Return(std::move(result));
            }
        } catch (const std::exception &e) {
            record_invoke_exception(e);
        } catch (...) {
            record_unknown_invoke_exception();
        }
    }

    /// Matches `MethodInvokeFn`'s signature exactly (so `MethodInfo`/`invoke_method`/`Multicast`
    /// hooks/overrides all work unchanged for static methods) — the `object` parameter is simply
    /// ignored, which is what lets `invoke_static_method` call through with `object = nullptr`.
    template <auto Member, class Traits>
    void invoke_static_trampoline(void * /*object*/, const void *const *args, void *out_return, void * /*user_data*/) noexcept {
        invoke_static_trampoline_impl<Member, Traits>(args, out_return, std::make_index_sequence<Traits::arity>{});
    }

    /// Builds a runtime `MethodInfo` for one static member function, given its address (a plain
    /// function pointer) as a non-type template parameter. Sets `MethodInfo::is_static`.
    template <auto Member>
    [[nodiscard]] MethodInfo build_static_method_info(std::string_view name, auto &&...attrs) {
        using Traits = MemberFunctionTraits<decltype(Member)>;
        MethodInfo info{};
        info.name = UString{name};
        info.return_type = erased_type_id<typename Traits::Return>();
        info.param_types = collect_param_types<Traits>(std::make_index_sequence<Traits::arity>{});
        info.key = compute_method_key(name, info.param_types);
        info.invoke = &invoke_static_trampoline<Member, Traits>;
        info.attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...};
        info.is_static = true;
        return info;
    }

    /// Builds a runtime `EventInfo` for one declared event. Unlike fields/methods, an event has
    /// no backing member pointer — `SFT_REFLECT_EVENT` supplies its parameter types directly as
    /// a template argument pack instead. `attrs` exists for API symmetry with
    /// `build_field_info`/`build_method_info`; no macro currently forwards any (`SFT_REFLECT_EVENT`
    /// already uses its variadic argument for parameter types).
    template <class... Args>
    [[nodiscard]] EventInfo build_event_info(std::string_view name, auto &&...attrs) {
        EventInfo info{};
        info.key = TypeId::from_name(name);
        info.name = UString{name};
        info.param_types = std::vector<TypeId>{erased_type_id<std::remove_cvref_t<Args>>()...};
        info.attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...};
        return info;
    }

    /// Placement-constructs a `T` into `destination` from `args` (one already-typed,
    /// caller-marshalled pointer per parameter, matching `Args`). Args are not moved from — they
    /// may come from a caller who still owns them — matching `invoke_trampoline_impl`'s treatment
    /// of method arguments.
    template <class T, class... Args, usize... Is>
    void construct_trampoline_impl(void *destination, const void *const *args, std::index_sequence<Is...>) noexcept {
        try {
            ::new (destination) T(*static_cast<std::remove_cvref_t<Args> *>(const_cast<void *>(args[Is]))...);
        } catch (const std::exception &e) {
            record_invoke_exception(e);
        } catch (...) {
            record_unknown_invoke_exception();
        }
    }

    template <class T, class... Args>
    void construct_trampoline(void *destination, const void *const *args, void * /*user_data*/) noexcept {
        construct_trampoline_impl<T, Args...>(destination, args, std::index_sequence_for<Args...>{});
    }

    /// Builds a runtime `ConstructorInfo` for one parameterized constructor, given its parameter
    /// types directly as a template argument pack — like `build_event_info`, a constructor has no
    /// address to take, so `SFT_REFLECT_CONSTRUCTOR` supplies the signature this way instead.
    template <class T, class... Args>
    [[nodiscard]] ConstructorInfo build_constructor_info(auto &&...attrs) {
        static_assert(std::is_constructible_v<T, Args...>,
                      "SFT_REFLECT_CONSTRUCTOR's parameter types must actually construct T.");
        ConstructorInfo info{};
        info.param_types = std::vector<TypeId>{erased_type_id<std::remove_cvref_t<Args>>()...};
        info.invoke = &construct_trampoline<T, Args...>;
        info.attributes = std::vector<Attribute>{std::forward<decltype(attrs)>(attrs)...};
        return info;
    }

    /// Builds the full runtime `TypeInfo` for `T` by walking `TypeTraits<T>::for_each_member`
    /// (populated by `SFT_REFLECT_TYPE`/`SFT_REFLECT_FIELD`/`SFT_REFLECT_METHOD`).
    ///
    /// Not `consteval` — like `Ecs::Detail::make_component_info`, it produces function pointers
    /// and heap-owning members (`UString`, `std::vector`), which cannot be constant-evaluated.
    /// Called lazily, once per type, from `TypeRegistry::try_register`.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] TypeInfo make_type_info() {
        using TypeT = std::remove_cv_t<T>;
        static_assert(std::is_nothrow_move_constructible_v<TypeT>,
                      "Reflected types must be nothrow move-constructible.");
        static_assert(std::is_nothrow_destructible_v<TypeT>,
                      "Reflected types must be nothrow destructible.");

        constexpr std::string_view name = type_name<TypeT>();
        TypeInfo info{};
        info.key = TypeId::from_name(name);
        info.canonical_name = UString{name};
        info.schema_version = type_schema_version<TypeT>();
        info.size = sizeof(TypeT);
        info.align = alignof(TypeT);
        info.flags = type_flags<TypeT>();
        info.move_construct = [](void *destination, void *source, void *) noexcept {
            ::new (destination) TypeT(std::move(*static_cast<TypeT *>(source)));
        };
        info.destroy = [](void *object, void *) noexcept {
            static_cast<TypeT *>(object)->~TypeT();
        };
        if constexpr (std::is_nothrow_default_constructible_v<TypeT>) {
            info.default_construct = [](void *destination, void *) noexcept {
                ::new (destination) TypeT();
            };
        }
        if constexpr (std::is_nothrow_copy_constructible_v<TypeT>) {
            info.copy_construct = [](void *destination, const void *source, void *) noexcept {
                ::new (destination) TypeT(*static_cast<const TypeT *>(source));
            };
        }
        if constexpr (requires { typename TypeTraits<TypeT>::BaseType; }) {
            info.base_type = TypeId::from_name(type_name<typename TypeTraits<TypeT>::BaseType>());
        }

        TypeTraits<TypeT>::for_each_member([&info]<auto Member, MemberKind Kind, class... MemberTypeArgs>(std::string_view member_name, auto &&...attrs) {
            if constexpr (Kind == MemberKind::Field) {
                info.fields.push_back(build_field_info<TypeT, Member>(member_name, std::forward<decltype(attrs)>(attrs)...));
            } else if constexpr (Kind == MemberKind::Method) {
                info.methods.push_back(build_method_info<TypeT, Member>(member_name, std::forward<decltype(attrs)>(attrs)...));
            } else if constexpr (Kind == MemberKind::Event) {
                info.events.push_back(build_event_info<MemberTypeArgs...>(member_name, std::forward<decltype(attrs)>(attrs)...));
            } else if constexpr (Kind == MemberKind::StaticField) {
                info.static_fields.push_back(build_static_field_info<Member>(member_name, std::forward<decltype(attrs)>(attrs)...));
            } else if constexpr (Kind == MemberKind::StaticMethod) {
                info.static_methods.push_back(build_static_method_info<Member>(member_name, std::forward<decltype(attrs)>(attrs)...));
            } else {
                info.constructors.push_back(build_constructor_info<TypeT, MemberTypeArgs...>(std::forward<decltype(attrs)>(attrs)...));
            }
        });

        return info;
    }

    /// Returns a reflected enum's stable canonical name. Mirrors `type_name<T>()`.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid for the
    /// program's lifetime, since it aliases the `EnumTraits<T>` specialization's static storage.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval std::string_view enum_name() {
        using EnumT = std::remove_cv_t<T>;
        constexpr std::string_view name = EnumTraits<EnumT>::name;
        static_assert(!name.empty(),
                      "Reflected enums need a stable canonical name. Specialize "
                      "SFT::Reflection::EnumTraits<T> or use SFT_REFLECT_ENUM(T, \"name\").");
        return name;
    }

    /// Builds the full runtime `EnumInfo` for `T` by walking `EnumTraits<T>::for_each_enumerator`
    /// (populated by `SFT_REFLECT_ENUM`/`SFT_REFLECT_ENUM_VALUE`). Not `consteval`, for the same
    /// reason `make_type_info` isn't (it produces heap-owning members). Called lazily, once per
    /// enum, from `TypeRegistry::try_register_enum`.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it
    /// invokes propagate to the caller.
    template <class T>
    [[nodiscard]] EnumInfo make_enum_info() {
        using EnumT = std::remove_cv_t<T>;
        static_assert(std::is_enum_v<EnumT>, "SFT_REFLECT_ENUM requires an enum type.");
        using Underlying = std::underlying_type_t<EnumT>;

        constexpr std::string_view name = enum_name<EnumT>();
        EnumInfo info{};
        info.key = TypeId::from_name(name);
        info.canonical_name = UString{name};
        info.underlying_type = erased_type_id<Underlying>();
        info.size = sizeof(EnumT);
        info.align = alignof(EnumT);

        EnumTraits<EnumT>::for_each_enumerator([&info]<auto Value>(std::string_view enumerator_name) {
            info.enumerators.push_back(EnumeratorInfo{
                .name = UString{enumerator_name},
                .value = static_cast<i64>(Value),
                .attributes = {},
            });
        });

        return info;
    }

} // namespace SFT::Reflection::Detail


/// Opens a `TypeTraits<TYPE>` specialization; follow with any number of `SFT_REFLECT_FIELD`/
/// `SFT_REFLECT_METHOD` calls (each terminated by the caller with `;`, matching
/// `SFT_ECS_COMPONENT`'s convention) and close with `SFT_REFLECT_END()`.
///
/// Pure compile-time: like `SFT_ECS_COMPONENT`, this only specializes a template — it performs
/// no static registration. The type is registered lazily, on first `TypeRegistry::type<TYPE>()`
/// or `try_register<TYPE>()` call.
#define SFT_REFLECT_TYPE(TYPE, CANONICAL_NAME)                      \
    template <>                                                     \
    struct SFT::Reflection::TypeTraits<TYPE> {                      \
        using ReflectedType [[maybe_unused]] = TYPE;                \
        static constexpr std::string_view name [[maybe_unused]] {CANONICAL_NAME};     \
        template <class Visitor>                                    \
        static constexpr void for_each_member(Visitor &&visitor) {

/// Same as `SFT_REFLECT_TYPE`, additionally pinning an explicit schema version (mirrors
/// `SFT_ECS_COMPONENT_VERSIONED`).
#define SFT_REFLECT_TYPE_VERSIONED(TYPE, CANONICAL_NAME, SCHEMA_VERSION) \
    template <>                                                          \
    struct SFT::Reflection::TypeTraits<TYPE> {                           \
        using ReflectedType [[maybe_unused]] = TYPE;                     \
        static constexpr std::string_view name [[maybe_unused]] {CANONICAL_NAME};          \
        static constexpr u32 schema_version = SCHEMA_VERSION;            \
        template <class Visitor>                                         \
        static constexpr void for_each_member(Visitor &&visitor) {

/// Same as `SFT_REFLECT_TYPE`, additionally recording `BASE` as this type's reflected base
/// (`TypeInfo::base_type`), enabling `TypeRegistry::find_field`/`find_method`/`find_event` to
/// fall through to inherited members the way Java's `getField`/`getMethod` search superclasses.
/// `BASE` must itself be reflected via `SFT_REFLECT_TYPE`. Single inheritance only in v1.
#define SFT_REFLECT_TYPE_WITH_BASE(TYPE, CANONICAL_NAME, BASE)     \
    template <>                                                    \
    struct SFT::Reflection::TypeTraits<TYPE> {                     \
        using ReflectedType [[maybe_unused]] = TYPE;                \
        using BaseType [[maybe_unused]] = BASE;                     \
        static constexpr std::string_view name [[maybe_unused]] {CANONICAL_NAME};     \
        template <class Visitor>                                    \
        static constexpr void for_each_member(Visitor &&visitor) {

/// Declares one public data member as reflectable. Must appear between `SFT_REFLECT_TYPE`/
/// `SFT_REFLECT_TYPE_VERSIONED`/`SFT_REFLECT_TYPE_WITH_BASE` and `SFT_REFLECT_END`, one statement
/// per member (caller supplies the trailing `;`). Optional trailing arguments are
/// `SFT_ATTR_BOOL`/`_INT`/`_FLOAT`/`_STRING` values, e.g.
/// `SFT_REFLECT_FIELD(health, SFT_ATTR_INT("min", 0), SFT_ATTR_INT("max", 999))`.
#define SFT_REFLECT_FIELD(MEMBER, ...)                                                                            \
    visitor.template operator()<&ReflectedType::MEMBER, SFT::Reflection::MemberKind::Field>(std::string_view{#MEMBER} __VA_OPT__(, ) __VA_ARGS__)

/// Declares one public member function as reflectable/invocable/overridable. Must appear between
/// `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`/`SFT_REFLECT_TYPE_WITH_BASE` and
/// `SFT_REFLECT_END`, one statement per method (caller supplies the trailing `;`). Overload sets
/// are not supported (see `Detail::MemberFunctionTraits`). Optional trailing arguments are
/// attributes, same as `SFT_REFLECT_FIELD`.
#define SFT_REFLECT_METHOD(MEMBER, ...)                                                                             \
    visitor.template operator()<&ReflectedType::MEMBER, SFT::Reflection::MemberKind::Method>(std::string_view{#MEMBER} __VA_OPT__(, ) __VA_ARGS__)

/// Same as `SFT_REFLECT_METHOD`, for one member of an overload set. `&ReflectedType::MEMBER` is
/// ill-formed on its own when `MEMBER` names more than one overload — the compiler has no return
/// type/parameter list to pick one from — so `POINTER_TYPE` supplies the exact pointer-to-member
/// type to select it, e.g. `SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int))`
/// and `SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int, DamageType))` for a
/// type with both a `take_damage(int)` and a `take_damage(int, DamageType)`. `ReflectedType` (the
/// alias `SFT_REFLECT_TYPE` opens) is in scope, so `POINTER_TYPE` can reference it directly. Both
/// overloads are then reachable as `MEMBER` — resolved unambiguously by signature at each call
/// site through `Detail::compute_method_key` (see `TypeInfo::find_method`'s `param_types`
/// overload, and `SFT_REFLECT_INVOKE`, which resolves by the real, compile-time-known signature of
/// whichever `&TYPE::METHOD` it names, so no `_OVERLOAD` variant is needed there).
#define SFT_REFLECT_METHOD_OVERLOAD(MEMBER, POINTER_TYPE, ...)                                                       \
    visitor.template operator()<static_cast<POINTER_TYPE>(&ReflectedType::MEMBER), SFT::Reflection::MemberKind::Method>(std::string_view{#MEMBER} __VA_OPT__(, ) __VA_ARGS__)

/// Declares a named hook point with no backing C++ member — game code fires it explicitly via
/// `SFT_REFLECT_FIRE_EVENT`, and any number of mods may subscribe listeners
/// (`TypeRegistry::subscribe_event`) without needing to replace a method. `...` lists the
/// event's parameter types, e.g. `SFT_REFLECT_EVENT("on_damaged", int, PlayerController *)`.
/// Must appear between `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`/
/// `SFT_REFLECT_TYPE_WITH_BASE` and `SFT_REFLECT_END` (caller supplies the trailing `;`).
#define SFT_REFLECT_EVENT(NAME, ...)                                                                              \
    visitor.template operator()<nullptr, SFT::Reflection::MemberKind::Event __VA_OPT__(, ) __VA_ARGS__>(std::string_view{NAME})

/// Declares one public static data member as reflectable. Must appear between `SFT_REFLECT_TYPE`/
/// `SFT_REFLECT_TYPE_VERSIONED`/`SFT_REFLECT_TYPE_WITH_BASE` and `SFT_REFLECT_END`, one statement
/// per member (caller supplies the trailing `;`). Optional trailing arguments are attributes,
/// same as `SFT_REFLECT_FIELD`. Reachable through the same `TypeRegistry::find_field`/
/// `TypeInfo::find_field` as instance fields — check `has_flag(field->flags, FieldFlags::Static)`
/// and use `copy_static_field_out`/`_in` to access it.
#define SFT_REFLECT_STATIC_FIELD(MEMBER, ...)                                                                            \
    visitor.template operator()<&ReflectedType::MEMBER, SFT::Reflection::MemberKind::StaticField>(std::string_view{#MEMBER} __VA_OPT__(, ) __VA_ARGS__)

/// Declares one public static member function as reflectable/invocable/overridable. Must appear
/// between `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`/`SFT_REFLECT_TYPE_WITH_BASE` and
/// `SFT_REFLECT_END`, one statement per method (caller supplies the trailing `;`). Optional
/// trailing arguments are attributes, same as `SFT_REFLECT_METHOD`. Reachable through the same
/// `TypeRegistry::find_method`/`TypeInfo::find_method` as instance methods (check
/// `method->is_static`); overrides/hooks work identically. Call through `invoke_static_method`,
/// not `invoke_method`.
#define SFT_REFLECT_STATIC_METHOD(MEMBER, ...)                                                                             \
    visitor.template operator()<&ReflectedType::MEMBER, SFT::Reflection::MemberKind::StaticMethod>(std::string_view{#MEMBER} __VA_OPT__(, ) __VA_ARGS__)

/// Declares one parameterized constructor — the Java-`Constructor.newInstance(args)` analog. `...`
/// lists the constructor's parameter types, e.g. `SFT_REFLECT_CONSTRUCTOR(int, float)` for a
/// `T(int, float)` constructor. Constructors have no name in C++, so unlike fields/methods/events
/// this takes no name argument and is found by signature (`TypeInfo::find_constructor`), not by
/// name. Must appear between `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED`/
/// `SFT_REFLECT_TYPE_WITH_BASE` and `SFT_REFLECT_END` (caller supplies the trailing `;`).
#define SFT_REFLECT_CONSTRUCTOR(...)                                                                              \
    visitor.template operator()<nullptr, SFT::Reflection::MemberKind::Constructor __VA_OPT__(, ) __VA_ARGS__>(std::string_view{})

/// Closes a `SFT_REFLECT_TYPE`/`SFT_REFLECT_TYPE_VERSIONED` block (caller supplies the trailing `;`).
/// The first brace closes `for_each_member`'s body, the second closes the `TypeTraits<TYPE>` struct.
#define SFT_REFLECT_END() \
    }                      \
    }

/// Opens an `EnumTraits<TYPE>` specialization; follow with any number of `SFT_REFLECT_ENUM_VALUE`
/// calls (each terminated by the caller with `;`) and close with `SFT_REFLECT_ENUM_END()`. Pure
/// compile-time, registered lazily on first `TypeRegistry::enum_type<TYPE>()`/
/// `try_register_enum<TYPE>()` call — mirrors `SFT_REFLECT_TYPE`'s discipline exactly, for enums.
#define SFT_REFLECT_ENUM(TYPE, CANONICAL_NAME)                      \
    template <>                                                     \
    struct SFT::Reflection::EnumTraits<TYPE> {                      \
        using ReflectedEnum [[maybe_unused]] = TYPE;                \
        static constexpr std::string_view name [[maybe_unused]] {CANONICAL_NAME};     \
        template <class Visitor>                                    \
        static constexpr void for_each_enumerator(Visitor &&visitor) {

/// Declares one enumerator as reflectable. Must appear between `SFT_REFLECT_ENUM` and
/// `SFT_REFLECT_ENUM_END`, one statement per enumerator (caller supplies the trailing `;`).
#define SFT_REFLECT_ENUM_VALUE(VALUE) \
    visitor.template operator()<ReflectedEnum::VALUE>(std::string_view{#VALUE})

/// Closes a `SFT_REFLECT_ENUM` block (caller supplies the trailing `;`).
#define SFT_REFLECT_ENUM_END() \
    }                           \
    }
