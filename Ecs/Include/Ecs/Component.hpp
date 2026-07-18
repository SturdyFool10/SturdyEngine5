#pragma once

#include <Ecs/Contract.hpp>

#include <Foundation/Foundation.hpp>

#include <deque>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace SFT::Ecs {

    using ComponentId = u32;
    inline constexpr ComponentId invalid_component_id = std::numeric_limits<ComponentId>::max();

    // Stable, public identity for a component schema. Unlike ComponentId, this key is independent
    // of registration order and can cross a C ABI, language binding, serialized scene, or plugin
    // boundary. The dense ComponentId resolved by ComponentRegistry remains the hot-path value.
    struct ComponentKey {
        u64 high = 0;
        u64 low = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return high != 0 || low != 0;
        }

        friend constexpr bool operator==(ComponentKey, ComponentKey) noexcept = default;

        // Two independently seeded FNV-style lanes provide a deterministic 128-bit key. The high
        // lane uses ordinary UTF-8 bytes/FNV prime; the low lane salts each byte by 0x9d and uses
        // the next odd multiplier. This does not depend on RTTI, compiler type names, process
        // addresses, or registration order and is straightforward to reproduce in FFI bindings.
        // ComponentRegistry still checks the canonical name/schema when a key already exists, so
        // a hash collision becomes a registration error instead of silent type aliasing.
        [[nodiscard]] static constexpr ComponentKey from_name(std::string_view canonical_name) noexcept {
            constexpr u64 fnv_prime = 1099511628211ull;
            u64 high_hash = 14695981039346656037ull;
            u64 low_hash = 7809847782465536322ull;
            for (char character : canonical_name) {
                const auto byte = static_cast<u8>(static_cast<unsigned char>(character));
                high_hash = (high_hash ^ byte) * fnv_prime;
                low_hash = (low_hash ^ static_cast<u8>(byte + 0x9du)) * (fnv_prime + 2ull);
            }
            return ComponentKey{.high = high_hash, .low = low_hash};
        }
    };

    static_assert(sizeof(ComponentKey) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<ComponentKey>);
    static_assert(std::is_trivially_copyable_v<ComponentKey>);

    struct ComponentKeyHash {
        [[nodiscard]] usize operator()(ComponentKey key) const noexcept {
            const u64 mixed = key.low ^ (key.high + 0x9e3779b97f4a7c15ull + (key.low << 6u) + (key.low >> 2u));
            return static_cast<usize>(mixed);
        }
    };

    enum class ComponentFlags : u32 {
        None = 0,
        TriviallyCopyable = 1u << 0u,
        TriviallyDestructible = 1u << 1u,
        FfiBlittable = 1u << 2u,
        Pinned = 1u << 3u,
        Tag = 1u << 4u,
    };

    [[nodiscard]] constexpr ComponentFlags operator|(ComponentFlags lhs, ComponentFlags rhs) noexcept {
        return static_cast<ComponentFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    [[nodiscard]] constexpr bool has_flag(ComponentFlags value, ComponentFlags flag) noexcept {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    using ComponentDefaultConstructFn = void (*)(void *destination, void *user_data) noexcept;
    using ComponentCopyConstructFn = void (*)(void *destination, const void *source, void *user_data) noexcept;
    using ComponentMoveConstructFn = void (*)(void *destination, void *source, void *user_data) noexcept;
    using ComponentDestroyFn = void (*)(void *object, void *user_data) noexcept;

    // Complete runtime descriptor used by the non-templated storage core. `user_data` belongs to
    // the registering module/language runtime and is passed to every lifecycle callback. It must
    // outlive every World using this registry entry.
    struct ComponentInfo {
        ComponentKey key{};
        UString canonical_name;
        u32 schema_version = 1;
        usize size = 0;
        usize align = 0;
        ComponentFlags flags = ComponentFlags::None;
        void *user_data = nullptr;
        ComponentDefaultConstructFn default_construct = nullptr;
        ComponentCopyConstructFn copy_construct = nullptr;
        ComponentMoveConstructFn move_construct = nullptr;
        ComponentDestroyFn destroy = nullptr;
    };

    enum class ComponentRegistryErrorCode : u32 {
        InvalidDescriptor,
        UnsupportedStoragePolicy,
        StableKeyCollision,
        CanonicalNameCollision,
        ComponentLimitReached,
    };

    struct ComponentRegistryError {
        ComponentRegistryErrorCode code = ComponentRegistryErrorCode::InvalidDescriptor;
        UString message;
    };

    template <class Value>
    using ComponentRegistryExpected = std::expected<Value, ComponentRegistryError>;

    // Native component types specialize this trait with a stable canonical name. Schema version
    // and flags are optional members; the helpers below provide defaults when omitted.
    template <class T>
    struct ComponentTraits {
        static constexpr std::string_view name{};
    };

    namespace Detail {

        template <class T>
        [[nodiscard]] consteval std::string_view component_name() {
            using ComponentT = std::remove_cv_t<T>;
            constexpr std::string_view name = ComponentTraits<ComponentT>::name;
            static_assert(!name.empty(),
                          "ECS component types need a stable canonical name. Specialize "
                          "SFT::Ecs::ComponentTraits<T> or use SFT_ECS_COMPONENT(T, \"name\").");
            return name;
        }

        template <class T>
        [[nodiscard]] consteval u32 component_schema_version() {
            using ComponentT = std::remove_cv_t<T>;
            if constexpr (requires { ComponentTraits<ComponentT>::schema_version; }) {
                return ComponentTraits<ComponentT>::schema_version;
            } else {
                return 1;
            }
        }

        template <class T>
        [[nodiscard]] consteval ComponentFlags component_flags() {
            using ComponentT = std::remove_cv_t<T>;
            ComponentFlags flags = ComponentFlags::None;
            if constexpr (std::is_trivially_copyable_v<ComponentT>) {
                flags = flags | ComponentFlags::TriviallyCopyable;
            }
            if constexpr (std::is_trivially_destructible_v<ComponentT>) {
                flags = flags | ComponentFlags::TriviallyDestructible;
            }
            if constexpr (requires { ComponentTraits<ComponentT>::flags; }) {
                flags = flags | ComponentTraits<ComponentT>::flags;
            }
            return flags;
        }

        template <class T>
        [[nodiscard]] bool matches_native_component(const ComponentInfo &info) noexcept {
            using ComponentT = std::remove_cv_t<T>;
            constexpr std::string_view name = component_name<ComponentT>();
            return info.key == ComponentKey::from_name(name) &&
                   info.canonical_name.cpp_string_view() == name &&
                   info.schema_version == component_schema_version<ComponentT>() &&
                   info.size == sizeof(ComponentT) &&
                   info.align == alignof(ComponentT) &&
                   info.flags == component_flags<ComponentT>();
        }

        template <class T>
        [[nodiscard]] ComponentInfo make_component_info() {
            using ComponentT = std::remove_cv_t<T>;
            static_assert(std::is_nothrow_move_constructible_v<ComponentT>,
                          "ECS archetype components must be nothrow move-constructible.");
            static_assert(std::is_nothrow_destructible_v<ComponentT>,
                          "ECS archetype components must be nothrow destructible.");

            constexpr std::string_view name = component_name<ComponentT>();
            ComponentInfo info{
                .key = ComponentKey::from_name(name),
                .canonical_name = UString{name},
                .schema_version = component_schema_version<ComponentT>(),
                .size = sizeof(ComponentT),
                .align = alignof(ComponentT),
                .flags = component_flags<ComponentT>(),
                .move_construct = [](void *destination, void *source, void *) noexcept { ::new (destination) ComponentT(std::move(*static_cast<ComponentT *>(source))); },
                .destroy = [](void *object, void *) noexcept { static_cast<ComponentT *>(object)->~ComponentT(); },
            };
            if constexpr (std::is_nothrow_default_constructible_v<ComponentT>) {
                info.default_construct = [](void *destination, void *) noexcept {
                    ::new (destination) ComponentT();
                };
            }
            if constexpr (std::is_nothrow_copy_constructible_v<ComponentT>) {
                info.copy_construct = [](void *destination, const void *source, void *) noexcept {
                    ::new (destination) ComponentT(*static_cast<const ComponentT *>(source));
                };
            }
            return info;
        }

    } // namespace Detail

    template <class T>
    [[nodiscard]] consteval ComponentKey component_key() {
        return ComponentKey::from_name(Detail::component_name<std::remove_cv_t<T>>());
    }

    // Engine-owned registry shared by every World that needs interoperable component IDs. It is
    // intentionally non-copyable/non-movable: Worlds keep a non-owning reference and the registry
    // must outlive them. Descriptor addresses remain stable as later types register.
    class ComponentRegistry {
      public:
        ComponentRegistry() = default;
        ~ComponentRegistry() = default;

        ComponentRegistry(const ComponentRegistry &) = delete;
        ComponentRegistry &operator=(const ComponentRegistry &) = delete;
        ComponentRegistry(ComponentRegistry &&) = delete;
        ComponentRegistry &operator=(ComponentRegistry &&) = delete;

        [[nodiscard]] ComponentRegistryExpected<ComponentId> register_component(ComponentInfo info);

        template <class T>
        [[nodiscard]] ComponentRegistryExpected<ComponentId> try_register() {
            if (const auto existing = find(component_key<std::remove_cv_t<T>>())) {
                if (const ComponentInfo *descriptor = info(*existing);
                    descriptor != nullptr && Detail::matches_native_component<std::remove_cv_t<T>>(*descriptor)) {
                    return *existing;
                }
            }
            return register_component(Detail::make_component_info<std::remove_cv_t<T>>());
        }

        // Native typed operations use this contract form after startup validation. A duplicate
        // canonical name/key with incompatible metadata is a programming/configuration error, so
        // it terminates instead of injecting expected-handling into every spawn/query hot path.
        template <class T>
        [[nodiscard]] ComponentId component() {
            auto registered = try_register<std::remove_cv_t<T>>();
            if (!registered) {
                Detail::contract_violation(
                    "ECS component resolution failed for '{}': {}",
                    Detail::component_name<std::remove_cv_t<T>>(),
                    registered.error().message);
            }
            return *registered;
        }

        [[nodiscard]] std::optional<ComponentId> find(ComponentKey key) const noexcept;
        [[nodiscard]] std::optional<ComponentId> find(const ustr &canonical_name) const noexcept;
        [[nodiscard]] const ComponentInfo *info(ComponentId id) const noexcept;
        [[nodiscard]] usize size() const noexcept;

      private:
        mutable std::shared_mutex mutex_;
        std::deque<ComponentInfo> infos_;
        std::unordered_map<ComponentKey, ComponentId, ComponentKeyHash> ids_by_key_;
        std::unordered_map<UString, ComponentId> ids_by_name_;
    };

} // namespace SFT::Ecs

// Concise registration helper for ordinary native components. Use the versioned form when a
// serialized/reflected schema changes incompatibly.
#define SFT_ECS_COMPONENT(TYPE, CANONICAL_NAME)                 \
    template <>                                                 \
    struct SFT::Ecs::ComponentTraits<TYPE> {                    \
        static constexpr std::string_view name{CANONICAL_NAME}; \
    }

#define SFT_ECS_COMPONENT_VERSIONED(TYPE, CANONICAL_NAME, SCHEMA_VERSION) \
    template <>                                                           \
    struct SFT::Ecs::ComponentTraits<TYPE> {                              \
        static constexpr std::string_view name{CANONICAL_NAME};           \
        static constexpr u32 schema_version = SCHEMA_VERSION;             \
    }
