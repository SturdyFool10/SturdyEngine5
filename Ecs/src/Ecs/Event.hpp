#pragma once

#include <Ecs/Resource.hpp>

#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {


    template <class T>
    class Events {
      public:
        /// Performs the send operation for `Events` using the supplied arguments.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void send(T event) noexcept { buffer_.push_back(std::move(event)); }

        /// Returns the current or globally available emplace value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        template <class... Args>
        T &emplace(Args &&...args) noexcept {
            return buffer_.emplace_back(std::forward<Args>(args)...);
        }

        /// Reads the requested data from the associated source.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const T> read() const noexcept { return buffer_; }
        /// Reports whether this `Events` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
        /// Returns the size for this `T`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `Events`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
        /// Reserves storage for at least the requested capacity without changing the logical contents.
        ///
        /// @param capacity `capacity` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void reserve(usize capacity) { buffer_.reserve(capacity); }
        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept { buffer_.clear(); }

      private:
        std::vector<T> buffer_;
    };

    template <class T>
    struct IsEventResource : std::false_type {};

    template <class T>
    struct IsEventResource<Events<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_event_resource_v = IsEventResource<std::remove_cv_t<T>>::value;


    template <class T>
    class EventWriter {
      public:
        /// Performs the send operation for `EventWriter` using the supplied arguments.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void send(T event) noexcept { events_->send(std::move(event)); }

        /// Returns the current or globally available emplace value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        template <class... Args>
        T &emplace(Args &&...args) noexcept {
            return events_->emplace(std::forward<Args>(args)...);
        }

      private:
        friend struct Detail::ResourceArgumentTraits<EventWriter<T>>;
        /// Constructs a `EventWriter` from the supplied initialization values.
        ///
        /// @param events Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit EventWriter(Events<T> &events) noexcept : events_(&events) {}

        Events<T> *events_ = nullptr;
    };


    template <class T>
    class EventReader {
      public:
        /// Reads the requested data from the associated source.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const T> read() const noexcept { return events_->read(); }
        /// Reports whether this `EventReader` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept { return events_->empty(); }
        /// Returns the size for this `T`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `EventReader`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept { return events_->size(); }
        /// Returns an iterator to the first element in the range.
        ///
        /// @return Returns an iterator referring to the first element.
        /// @note This function does not throw exceptions.
        [[nodiscard]] auto begin() const noexcept { return read().begin(); }
        /// Returns the one-past-the-end iterator for the range.
        ///
        /// @return Returns the one-past-the-end iterator.
        /// @note This function does not throw exceptions.
        [[nodiscard]] auto end() const noexcept { return read().end(); }

      private:
        friend struct Detail::ResourceArgumentTraits<EventReader<T>>;
        /// Constructs a `EventReader` from the supplied initialization values.
        ///
        /// @param events Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit EventReader(const Events<T> &events) noexcept : events_(&events) {}

        const Events<T> *events_ = nullptr;
    };

    namespace Detail {

        template <class T>
        struct ResourceArgumentTraits<EventWriter<T>> {
            static constexpr bool IsResource = true;
            static constexpr bool IsWrite = true;
            static constexpr bool IsEvent = true;
            using Resource = Events<T>;

            /// Constructs the supplied or associated value/state using the supplied arguments and current state.
            ///
            /// @param resource `resource` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] static EventWriter<T> construct(Resource &resource) noexcept {
                return EventWriter<T>{resource};
            }
        };

        template <class T>
        struct ResourceArgumentTraits<EventReader<T>> {
            static constexpr bool IsResource = true;
            static constexpr bool IsWrite = false;
            static constexpr bool IsEvent = true;
            using Resource = Events<T>;

            /// Constructs the supplied or associated value/state using the supplied arguments and current state.
            ///
            /// @param resource `resource` value used by the operation.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] static EventReader<T> construct(Resource &resource) noexcept {
                return EventReader<T>{resource};
            }
        };

    } // namespace Detail

} // namespace SFT::Ecs


#define SFT_ECS_EVENT(TYPE, CANONICAL_NAME)                            \
    template <>                                                       \
    struct SFT::Ecs::ResourceTraits<SFT::Ecs::Events<TYPE>> {         \
        static constexpr std::string_view name{CANONICAL_NAME};       \
    }
