#pragma once

#include <Ecs/src/Resource.hpp>

#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // A per-tick event buffer bound to the World like any other singleton resource. Registered with
    // SFT_ECS_EVENT (below) rather than SFT_ECS_RESOURCE so its canonical name is distinct from an
    // ordinary resource sharing the same T. World::bind_resource auto-clears every bound Events<T>
    // once at the start of each Schedule::run() (see World.hpp) — callers never call clear()
    // themselves, matching how RenderFrameRequests's manual begin_frame() is exactly what this design
    // avoids repeating for every event type.
    template <class T>
    class Events {
      public:
        void send(T event) noexcept { buffer_.push_back(std::move(event)); }

        template <class... Args>
        T &emplace(Args &&...args) noexcept {
            return buffer_.emplace_back(std::forward<Args>(args)...);
        }

        [[nodiscard]] std::span<const T> read() const noexcept { return buffer_; }
        [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
        [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
        void reserve(usize capacity) { buffer_.reserve(capacity); }
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

    // Ergonomic write-only view over Events<T>, resolved by Schedule exactly like WriteResource<T> —
    // multiple systems sending the same event type in one stage serialize automatically because both
    // participate in the same resource-write conflict analysis.
    template <class T>
    class EventWriter {
      public:
        void send(T event) noexcept { events_->send(std::move(event)); }

        template <class... Args>
        T &emplace(Args &&...args) noexcept {
            return events_->emplace(std::forward<Args>(args)...);
        }

      private:
        friend struct Detail::ResourceArgumentTraits<EventWriter<T>>;
        explicit EventWriter(Events<T> &events) noexcept : events_(&events) {}

        Events<T> *events_ = nullptr;
    };

    // Ergonomic read-only view over Events<T>, resolved like ReadResource<T>. Every reader sees the
    // full set of events sent so far this Schedule::run() — there is no per-reader cursor (see
    // plans/ecs-events-and-modules.md's open items for why that's an acceptable v1 tradeoff).
    template <class T>
    class EventReader {
      public:
        [[nodiscard]] std::span<const T> read() const noexcept { return events_->read(); }
        [[nodiscard]] bool empty() const noexcept { return events_->empty(); }
        [[nodiscard]] usize size() const noexcept { return events_->size(); }
        [[nodiscard]] auto begin() const noexcept { return read().begin(); }
        [[nodiscard]] auto end() const noexcept { return read().end(); }

      private:
        friend struct Detail::ResourceArgumentTraits<EventReader<T>>;
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

            [[nodiscard]] static EventReader<T> construct(Resource &resource) noexcept {
                return EventReader<T>{resource};
            }
        };

    } // namespace Detail

} // namespace SFT::Ecs

// Registers an event payload type. Distinct from SFT_ECS_RESOURCE so Events<T> gets its own
// canonical name/key rather than colliding with a plain resource named after the same T.
#define SFT_ECS_EVENT(TYPE, CANONICAL_NAME)                            \
    template <>                                                       \
    struct SFT::Ecs::ResourceTraits<SFT::Ecs::Events<TYPE>> {         \
        static constexpr std::string_view name{CANONICAL_NAME};       \
    }
