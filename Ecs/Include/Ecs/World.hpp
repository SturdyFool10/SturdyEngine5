#pragma once

#include <Ecs/Archetype.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/Query.hpp>
#include <Ecs/Signature.hpp>

#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // Owns every entity and archetype. First-cut scope: no add_component/remove_component yet — see
    // Archetype.hpp's doc comment on why that (archetype-transition) support is deferred rather than
    // shipped half-working.
    class World {
      public:
        World() = default;
        ~World() = default;
        World(const World &) = delete;
        World &operator=(const World &) = delete;
        World(World &&) noexcept = default;
        World &operator=(World &&) noexcept = default;

        // Spawns one entity with exactly the given components — its archetype is fixed at spawn time
        // from the argument types (there's no way to add/remove components afterward yet). `Ts...`
        // deduce to the decayed argument types (pass components by value or as rvalues).
        template <class... Ts>
        [[nodiscard]] Entity spawn(Ts &&...components) {
            const Signature signature = make_signature<std::decay_t<Ts>...>();
            const u32 archetype_index = archetype_index_for(signature);
            Archetype &archetype = archetypes_[archetype_index];

            const Entity entity = allocate_entity();
            const u32 row = archetype.add_row(entity);
            (::new (archetype.row_pointer(archetype.column_index_of(component_id<std::decay_t<Ts>>()), row))
                 std::decay_t<Ts>(std::forward<Ts>(components)),
             ...);

            EntityRecord &record = entity_records_[entity.index];
            record.archetype_index = archetype_index;
            record.row = row;
            return entity;
        }

        void destroy(Entity entity) noexcept {
            if (!is_alive(entity)) {
                return;
            }
            EntityRecord &record = entity_records_[entity.index];
            Archetype &archetype = archetypes_[record.archetype_index];
            const Entity moved = archetype.remove_row(record.row);
            if (moved) {
                entity_records_[moved.index].row = record.row;
            }
            ++record.generation;
            free_indices_.push_back(entity.index);
        }

        [[nodiscard]] bool is_alive(Entity entity) const noexcept {
            return entity.generation != 0 && entity.index < entity_records_.size() &&
                   entity_records_[entity.index].generation == entity.generation;
        }

        template <class T>
        [[nodiscard]] T *get_component(Entity entity) noexcept {
            return const_cast<T *>(std::as_const(*this).get_component<T>(entity));
        }

        template <class T>
        [[nodiscard]] const T *get_component(Entity entity) const noexcept {
            if (!is_alive(entity)) {
                return nullptr;
            }
            const EntityRecord &record = entity_records_[entity.index];
            const Archetype &archetype = archetypes_[record.archetype_index];
            const u32 column = archetype.column_index_of(component_id<T>());
            if (column == ~0u) {
                return nullptr;
            }
            return static_cast<const T *>(archetype.row_pointer(column, record.row));
        }

        // Every archetype whose signature is a superset of {remove_const_t<Ts>...} — matched fresh on
        // each call. Archetype counts are small (a handful to a few dozen distinct component-set
        // combinations, not per-entity), so this linear scan is cheap; caching per-Ts... results is a
        // documented, not-yet-needed optimization (see plans/ecs-design.md).
        template <class... Ts>
        [[nodiscard]] Query<Ts...> query() {
            const Signature required = make_signature<std::remove_const_t<Ts>...>();
            std::vector<u32> matches;
            for (usize i = 0; i < archetypes_.size(); ++i) {
                if (signature_is_superset(archetypes_[i].signature(), required)) {
                    matches.push_back(static_cast<u32>(i));
                }
            }
            return Query<Ts...>(&archetypes_, std::move(matches));
        }

      private:
        struct EntityRecord {
            u32 generation = 0;
            u32 archetype_index = ~0u;
            u32 row = ~0u;
        };

        [[nodiscard]] Entity allocate_entity() {
            if (!free_indices_.empty()) {
                const u32 index = free_indices_.back();
                free_indices_.pop_back();
                return Entity{.index = index, .generation = entity_records_[index].generation};
            }
            const auto index = static_cast<u32>(entity_records_.size());
            entity_records_.push_back(EntityRecord{.generation = 1});
            return Entity{.index = index, .generation = 1};
        }

        [[nodiscard]] u32 archetype_index_for(const Signature &signature) {
            for (usize i = 0; i < archetypes_.size(); ++i) {
                if (archetypes_[i].signature() == signature) {
                    return static_cast<u32>(i);
                }
            }
            archetypes_.emplace_back(signature);
            return static_cast<u32>(archetypes_.size() - 1);
        }

        std::vector<EntityRecord> entity_records_;
        std::vector<u32> free_indices_;
        std::vector<Archetype> archetypes_;
    };

} // namespace SFT::Ecs
