#pragma once

#include <Ecs/Component.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace SFT::Ecs {


    struct ResourceKey {
        u64 high = 0;
        u64 low = 0;

        /// Converts the `ResourceKey` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return high != 0 || low != 0; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(ResourceKey, ResourceKey) noexcept = default;

        /// Returns a human-readable name for the supplied from value.
        ///
        /// @param canonical_name Name used to identify or label the target.
        ///
        /// @return Returns the newly constructed or converted value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr ResourceKey from_name(std::string_view canonical_name) noexcept {
            const ComponentKey key = ComponentKey::from_name(canonical_name);
            return ResourceKey{.high = key.high, .low = key.low};
        }
    };

    static_assert(sizeof(ResourceKey) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<ResourceKey>);
    static_assert(std::is_trivially_copyable_v<ResourceKey>);

    struct ResourceKeyHash {
        /// Invokes the callable behavior provided by `ResourceKeyHash`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize operator()(ResourceKey key) const noexcept;
    };

    template <class T>
    struct ResourceTraits {
        static constexpr std::string_view name{};
    };

    namespace Detail {

        /// Returns a human-readable name for the supplied resource value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Returns the current or globally available resource key value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <class T>
    [[nodiscard]] consteval ResourceKey resource_key() {
        return ResourceKey::from_name(Detail::resource_name<std::remove_cv_t<T>>());
    }


    template <class T>
    class ReadResource {
      public:
        /// Returns the value or resource currently represented by `ReadResource`.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const T &get() const noexcept { return *resource_; }
        /// Accesses the object referenced by this `ReadResource`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const T *operator->() const noexcept { return resource_; }
        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const T &operator*() const noexcept { return *resource_; }

      private:
        friend struct Detail::ResourceViewFactory;
        /// Constructs a `ReadResource` from the supplied initialization values.
        ///
        /// @param resource `resource` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit ReadResource(const T &resource) noexcept : resource_(&resource) {}

        const T *resource_ = nullptr;
    };

    template <class T>
    class WriteResource {
      public:
        /// Returns the value or resource currently represented by `WriteResource`.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T &get() const noexcept { return *resource_; }
        /// Accesses the object referenced by this `WriteResource`.
        ///
        /// @return Returns a pointer through which the referenced object can be accessed.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T *operator->() const noexcept { return resource_; }
        /// Dereferences this iterator or handle.
        ///
        /// @return Returns the value or reference currently addressed by the iterator/handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] T &operator*() const noexcept { return *resource_; }

      private:
        friend struct Detail::ResourceViewFactory;
        /// Constructs a `WriteResource` from the supplied initialization values.
        ///
        /// @param resource `resource` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit WriteResource(T &resource) noexcept : resource_(&resource) {}

        T *resource_ = nullptr;
    };

    namespace Detail {

        struct ResourceViewFactory {
            /// Reads the requested data from the associated source.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            template <class T>
            [[nodiscard]] static ReadResource<T> read(const T &resource) noexcept {
                return ReadResource<T>{resource};
            }

            /// Writes the supplied data to the associated destination.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            template <class T>
            [[nodiscard]] static WriteResource<T> write(T &resource) noexcept {
                return WriteResource<T>{resource};
            }
        };

        template <class T>
        struct ResourceArgumentTraits {
            static constexpr bool IsResource = false;
        };


        template <class T>
        struct ResourceArgumentTraits<ReadResource<T>> {
            static constexpr bool IsResource = true;
            static constexpr bool IsWrite = false;
            static constexpr bool IsEvent = false;
            using Resource = T;

            /// Constructs the supplied or associated value/state using the supplied arguments and current state.
            ///
            /// @param resource `resource` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
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

            /// Constructs the supplied or associated value/state using the supplied arguments and current state.
            ///
            /// @param resource `resource` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
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
