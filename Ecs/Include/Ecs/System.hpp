#pragma once

#include <Ecs/Component.hpp>
#include <Ecs/Query.hpp>
#include <Ecs/World.hpp>

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // A system's declared data dependencies, derived (never hand-authored — see Detail::QueryAccessOf
    // below) from its Query<Ts...> parameter's own type signature.
    struct SystemAccess {
        std::vector<ComponentId> reads;
        std::vector<ComponentId> writes;
    };

    // True whenever two systems' declared access can't run concurrently: either side writes a
    // component the other side touches at all (write/write or write/read on the same component).
    // Read/read never conflicts.
    [[nodiscard]] inline bool system_access_conflicts(const SystemAccess &a, const SystemAccess &b) noexcept {
        for (ComponentId write : a.writes) {
            for (ComponentId read : b.reads) {
                if (read == write) {
                    return true;
                }
            }
            for (ComponentId other_write : b.writes) {
                if (other_write == write) {
                    return true;
                }
            }
        }
        for (ComponentId write : b.writes) {
            for (ComponentId read : a.reads) {
                if (read == write) {
                    return true;
                }
            }
        }
        return false;
    }

    namespace Detail {

        // Extracts a callable's operator()'s argument types as a std::tuple<Args...> — used to pull a
        // registered system's `Query<Ts...>` parameter type back out of an arbitrary lambda without
        // the caller having to name it twice.
        template <class F>
        struct CallableTraits : CallableTraits<decltype(&F::operator())> {};

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...) const> {
            using ArgsTuple = std::tuple<Args...>;
        };

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...)> {
            using ArgsTuple = std::tuple<Args...>;
        };

        template <class Q>
        struct QueryAccessOf; // undefined primary — only Query<Ts...> below is ever instantiated

        template <class... Ts>
        struct QueryAccessOf<Query<Ts...>> {
            [[nodiscard]] static SystemAccess access() {
                SystemAccess result;
                (accumulate<Ts>(result), ...);
                return result;
            }

          private:
            template <class T>
            static void accumulate(SystemAccess &result) {
                if constexpr (std::is_const_v<T>) {
                    result.reads.push_back(component_id<std::remove_const_t<T>>());
                } else {
                    result.writes.push_back(component_id<T>());
                }
            }
        };

        // Rebuilds a Query<Ts...> for a fresh World reference — the trait exists purely so
        // Schedule::add_system can go from "the Query<Ts...> type a system asked for" back to "call
        // World::query<Ts...>()" without needing Ts... spelled out a second time by the caller.
        template <class Q>
        struct QueryFactory;

        template <class... Ts>
        struct QueryFactory<Query<Ts...>> {
            [[nodiscard]] static Query<Ts...> build(World &world) { return world.query<Ts...>(); }
        };

    } // namespace Detail

    // Registers systems and runs them staged: mutually non-conflicting systems (per
    // system_access_conflicts()) execute concurrently, one Async::Scheduler::spawn per system in the
    // stage, with a wait-all before the next stage starts. Conflicting systems land in different
    // stages instead, ordering them relative to each other without either needing to name the other.
    class Schedule {
      public:
        // `fn` must be callable as `fn(World &, Query<Ts...>)` for some component pack `Ts...` — the
        // Query parameter *is* the access declaration (const Ts => read, non-const Ts => write); nothing
        // else needs stating up front. Registration order otherwise doesn't matter — conflicts (not
        // insertion order) decide relative ordering between stages.
        template <class F>
        void add_system(F fn) {
            using ArgsTuple = typename Detail::CallableTraits<F>::ArgsTuple;
            using QueryT = std::tuple_element_t<1, ArgsTuple>;

            SystemEntry entry;
            entry.access = Detail::QueryAccessOf<QueryT>::access();
            entry.run = [fn = std::move(fn)](World &world) mutable { fn(world, Detail::QueryFactory<QueryT>::build(world)); };
            systems_.push_back(std::move(entry));
            stages_dirty_ = true;
        }

        void run(World &world);

      private:
        struct SystemEntry {
            SystemAccess access;
            std::function<void(World &)> run;
        };

        void rebuild_stages();

        std::vector<SystemEntry> systems_;
        std::vector<std::vector<usize>> stages_; // each inner vector: indices into systems_
        bool stages_dirty_ = true;
    };

} // namespace SFT::Ecs
