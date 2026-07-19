#pragma once

#include <Ecs/src/Component.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace SFT::Ecs {

    // Resources are singleton objects bound to a World. Their stable key uses a separate type and
    // namespace from ComponentKey so the eventual C/FFI API can describe global engine services
    // without pretending they are per-entity data.
    struct ResourceKey {
        u64 high = 0;
        u64 low = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return high != 0 || low != 0; }
        friend constexpr bool operator==(ResourceKey, ResourceKey) noexcept = default;

        [[nodiscard]] static constexpr ResourceKey from_name(std::string_view canonical_name) noexcept {
            const ComponentKey key = ComponentKey::from_name(canonical_name);
            return ResourceKey{.high = key.high, .low = key.low};
        }
    };

    static_assert(sizeof(ResourceKey) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<ResourceKey>);
    static_assert(std::is_trivially_copyable_v<ResourceKey>);

    struct ResourceKeyHash {
        [[nodiscard]] usize operator()(ResourceKey key) const noexcept {
            const u64 mixed = key.low ^ (key.high + 0x9e3779b97f4a7c15ull + (key.low << 6u) + (key.low >> 2u));
            return static_cast<usize>(mixed);
        }
    };

    template <class T>
    struct ResourceTraits {
        static constexpr std::string_view name{};
    };

    namespace Detail {

        template <class T>
        [[nodiscard]] consteval std::string_view resource_name() {
            using ResourceT = std::remove_cv_t<T>;
            constexpr std::string_view name = ResourceTraits<ResourceT>::name;
            static_assert(!name.empty(),
                          "ECS resources need a stable canonical name. Specialize "
                          "SFT::Ecs::ResourceTraits<T> or use SFT_ECS_RESOURCE(T, \"name\").");
            return name;
        }

        struct ResourceViewFactory;

    } // namespace Detail

    template <class T>
    [[nodiscard]] consteval ResourceKey resource_key() {
        return ResourceKey::from_name(Detail::resource_name<std::remove_cv_t<T>>());
    }

    // Explicit system parameters make singleton access visible to the scheduler:
    //   ReadResource<T>  -> shared, immutable access
    //   WriteResource<T> -> exclusive access; the system's chunks execute serially
    // Values are lightweight non-owning views and are valid only for one system invocation.
    template <class T>
    class ReadResource {
      public:
        [[nodiscard]] const T &get() const noexcept { return *resource_; }
        [[nodiscard]] const T *operator->() const noexcept { return resource_; }
        [[nodiscard]] const T &operator*() const noexcept { return *resource_; }

      private:
        friend struct Detail::ResourceViewFactory;
        explicit ReadResource(const T &resource) noexcept : resource_(&resource) {}

        const T *resource_ = nullptr;
    };

    template <class T>
    class WriteResource {
      public:
        [[nodiscard]] T &get() const noexcept { return *resource_; }
        [[nodiscard]] T *operator->() const noexcept { return resource_; }
        [[nodiscard]] T &operator*() const noexcept { return *resource_; }

      private:
        friend struct Detail::ResourceViewFactory;
        explicit WriteResource(T &resource) noexcept : resource_(&resource) {}

        T *resource_ = nullptr;
    };

    namespace Detail {

        struct ResourceViewFactory {
            template <class T>
            [[nodiscard]] static ReadResource<T> read(const T &resource) noexcept {
                return ReadResource<T>{resource};
            }

            template <class T>
            [[nodiscard]] static WriteResource<T> write(T &resource) noexcept {
                return WriteResource<T>{resource};
            }
        };

        template <class T>
        struct ResourceArgumentTraits {
            static constexpr bool IsResource = false;
        };

        // Every resource-view argument type (ReadResource/WriteResource here, EventReader/EventWriter
        // in Ecs/Event.hpp) implements this same trait shape: IsWrite/IsEvent declare the argument's
        // access for Schedule's conflict analysis, Resource names the singleton it resolves against,
        // and construct() builds the argument value from that singleton. resolve_resource() in
        // Ecs/System.hpp calls Traits::construct() generically, so a new resource-view kind never
        // needs a change there — only a new ResourceArgumentTraits specialization.
        template <class T>
        struct ResourceArgumentTraits<ReadResource<T>> {
            static constexpr bool IsResource = true;
            static constexpr bool IsWrite = false;
            static constexpr bool IsEvent = false;
            using Resource = T;

            [[nodiscard]] static ReadResource<T> construct(Resource &resource) noexcept {
                return ResourceViewFactory::read(std::as_const(resource));
            }
        };

        template <class T>
        struct ResourceArgumentTraits<WriteResource<T>> {
            static constexpr bool IsResource = true;
            static constexpr bool IsWrite = true;
            static constexpr bool IsEvent = false;
            using Resource = T;

            [[nodiscard]] static WriteResource<T> construct(Resource &resource) noexcept {
                return ResourceViewFactory::write(resource);
            }
        };

        template <class T>
        inline constexpr bool is_resource_argument_v = ResourceArgumentTraits<std::remove_cvref_t<T>>::IsResource;

    } // namespace Detail

} // namespace SFT::Ecs

#define SFT_ECS_RESOURCE(TYPE, CANONICAL_NAME)                  \
    template <>                                                 \
    struct SFT::Ecs::ResourceTraits<TYPE> {                     \
        static constexpr std::string_view name{CANONICAL_NAME}; \
    }
