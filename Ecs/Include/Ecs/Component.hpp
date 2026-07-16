#pragma once

#include <Foundation/Foundation.hpp>

#include <utility>
#include <vector>

namespace SFT::Ecs {

    using ComponentId = u32;

    // Type-erased operations Archetype needs to manage a component's storage without knowing its
    // static type: moving a live object out of one row's memory into another (used by swap-remove,
    // see Archetype::remove_row) and destroying one in place. Components must be nothrow-movable and
    // never throw on destroy — this codebase uses no exceptions anywhere, so anything that could
    // throw from these two operations isn't supported as a component.
    struct ComponentInfo {
        usize size = 0;
        usize align = 0;
        void (*move_construct)(void *destination, void *source) noexcept = nullptr;
        void (*destroy)(void *object) noexcept = nullptr;
    };

    namespace Detail {

        // Indexed by ComponentId; grows by exactly one entry the first time each distinct T is seen
        // via component_id<T>(). Never shrinks — component kinds are a closed, small set fixed by the
        // program's own source, not something spawned/destroyed at runtime.
        [[nodiscard]] inline std::vector<ComponentInfo> &component_registry() noexcept {
            static std::vector<ComponentInfo> registry;
            return registry;
        }

        template <class T>
        [[nodiscard]] ComponentInfo make_component_info() noexcept {
            return ComponentInfo{
                .size = sizeof(T),
                .align = alignof(T),
                .move_construct =
                    [](void *destination, void *source) noexcept {
                        ::new (destination) T(std::move(*static_cast<T *>(source)));
                    },
                .destroy =
                    [](void *object) noexcept {
                        static_cast<T *>(object)->~T();
                    },
            };
        }

    } // namespace Detail

    // Mints (on first call for a given T) and returns a stable id for T, registering its
    // ComponentInfo alongside so later type-erased code (Archetype) can look it up by id alone.
    template <class T>
    [[nodiscard]] ComponentId component_id() noexcept {
        static const ComponentId id = [] {
            std::vector<ComponentInfo> &registry = Detail::component_registry();
            const auto new_id = static_cast<ComponentId>(registry.size());
            registry.push_back(Detail::make_component_info<T>());
            return new_id;
        }();
        return id;
    }

    [[nodiscard]] inline const ComponentInfo &component_info(ComponentId id) noexcept {
        return Detail::component_registry()[id];
    }

} // namespace SFT::Ecs
