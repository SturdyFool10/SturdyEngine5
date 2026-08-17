#pragma once

#include <Ecs/src/Contract.hpp>

#include <Foundation/src/Foundation.hpp>

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

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

    using ComponentId = u32;
    inline constexpr ComponentId invalid_component_id = std::numeric_limits<ComponentId>::max();


    struct ComponentKey {
        u64 high = 0;
        u64 low = 0;

        /// Converts the `ComponentKey` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return high != 0 || low != 0;
        }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(ComponentKey, ComponentKey) noexcept = default;


        /// Returns a human-readable name for the supplied from value.
        ///
        /// @param canonical_name Name used to identify or label the target.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function does not throw exceptions.
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
        /// Invokes the callable behavior provided by `ComponentKeyHash`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize operator()(ComponentKey key) const noexcept;
    };

    enum class ComponentFlags : u32 {
        None = 0,
        TriviallyCopyable = 1u << 0u,
        TriviallyDestructible = 1u << 1u,
        FfiBlittable = 1u << 2u,
        Pinned = 1u << 3u,
        Tag = 1u << 4u,
    };

    /// Combines the operands with bitwise OR.
    ///
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr ComponentFlags operator|(ComponentFlags lhs, ComponentFlags rhs) noexcept {
        return static_cast<ComponentFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    /// Reports whether flag is available.
    ///
    /// @param value Value consumed by the operation.
    /// @param flag `flag` value used by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool has_flag(ComponentFlags value, ComponentFlags flag) noexcept {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
    }

    using ComponentDefaultConstructFn = void (*)(void *destination, void *user_data) noexcept;
    using ComponentCopyConstructFn = void (*)(void *destination, const void *source, void *user_data) noexcept;
    using ComponentMoveConstructFn = void (*)(void *destination, void *source, void *user_data) noexcept;
    using ComponentDestroyFn = void (*)(void *object, void *user_data) noexcept;


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


    template <class T>
    struct ComponentTraits {
        static constexpr std::string_view name{};
    };

    namespace Detail {

        /// Returns a human-readable name for the supplied component value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        [[nodiscard]] consteval std::string_view component_name() {
            using ComponentT = std::remove_cv_t<T>;
            constexpr std::string_view name = ComponentTraits<ComponentT>::name;
            static_assert(!name.empty(),
                          "ECS component types need a stable canonical name. Specialize "
                          "SFT::Ecs::ComponentTraits<T> or use SFT_ECS_COMPONENT(T, \"name\").");
            return name;
        }

        /// Returns the current or globally available component schema version value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        [[nodiscard]] consteval u32 component_schema_version() {
            using ComponentT = std::remove_cv_t<T>;
            if constexpr (requires { ComponentTraits<ComponentT>::schema_version; }) {
                return ComponentTraits<ComponentT>::schema_version;
            } else {
                return 1;
            }
        }

        /// Returns the current or globally available component flags value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Returns the current or globally available matches native component value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
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

        /// Returns the current make component info.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Returns the current or globally available component key value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval ComponentKey component_key() {
        return ComponentKey::from_name(Detail::component_name<std::remove_cv_t<T>>());
    }


    class ComponentRegistry {
      public:
        /// Constructs a `ComponentRegistry` in its default state.
        ///
        /// @note This function does not throw exceptions.
        ComponentRegistry() = default;
        /// Destroys the `ComponentRegistry` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~ComponentRegistry() = default;

        /// Disables this construction form for `ComponentRegistry`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentRegistry(const ComponentRegistry &) = delete;
        /// Assigns a new value to this `ComponentRegistry`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentRegistry &operator=(const ComponentRegistry &) = delete;
        /// Disables this construction form for `ComponentRegistry`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentRegistry(ComponentRegistry &&) = delete;
        /// Assigns a new value to this `ComponentRegistry`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ComponentRegistry &operator=(ComponentRegistry &&) = delete;

        /// Registers component using the supplied arguments and current state.
        ///
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `ComponentRegistryErrorCode::InvalidDescriptor`, `ComponentRegistryErrorCode::UnsupportedStoragePolicy`, `ComponentRegistryErrorCode::StableKeyCollision`, `ComponentRegistryErrorCode::CanonicalNameCollision`, `ComponentRegistryErrorCode::ComponentLimitReached`.
        [[nodiscard]] ComponentRegistryExpected<ComponentId> register_component(ComponentInfo info);

        /// Attempts to register without requiring normal failure to be exceptional.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        template <class T>
        [[nodiscard]] ComponentRegistryExpected<ComponentId> try_register() {
            ZoneScopedN("ComponentRegistry::try_register");
            if (const auto existing = find(component_key<std::remove_cv_t<T>>())) {
                if (const ComponentInfo *descriptor = info(*existing);
                    descriptor != nullptr && Detail::matches_native_component<std::remove_cv_t<T>>(*descriptor)) {
                    return *existing;
                }
            }
            return register_component(Detail::make_component_info<std::remove_cv_t<T>>());
        }


        /// Returns the current or globally available component value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        [[nodiscard]] ComponentId component() {
            ZoneScopedN("ComponentRegistry::component");
            auto registered = try_register<std::remove_cv_t<T>>();
            if (!registered) {
                Detail::contract_violation(
                    "ECS component resolution failed for '{}': {}",
                    Detail::component_name<std::remove_cv_t<T>>(),
                    registered.error().message);
            }
            return *registered;
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<ComponentId> find(ComponentKey key) const noexcept;
        /// Finds the requested entry in the available state.
        ///
        /// @param canonical_name Name used to identify or label the target.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<ComponentId> find(const ustr &canonical_name) const noexcept;
        /// Performs the info operation for `ComponentRegistry` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const ComponentInfo *info(ComponentId id) const noexcept;
        /// Returns the size for this `ComponentRegistry`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `ComponentRegistry`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;

      private:
        mutable std::shared_mutex mutex_;
        std::deque<ComponentInfo> infos_;
        std::unordered_map<ComponentKey, ComponentId, ComponentKeyHash> ids_by_key_;
        std::unordered_map<UString, ComponentId> ids_by_name_;
    };

} // namespace SFT::Ecs


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
